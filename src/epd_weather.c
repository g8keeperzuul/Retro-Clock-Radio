/*
 * epd_weather.c - The daily weather screen. See the header.
 *
 * The mock-up is square; the usable box is 478 x 460, which is squarer still
 * but not square. The circle therefore shrinks to leave room for the sunrise
 * and sunset blocks either side of it, and everything else is pinned to the
 * circle rather than to the panel.
 *
 * The circle is a twenty-four hour dial: noon at the top, midnight at the foot,
 * running clockwise. Noon and midnight are fixed, so that divider is always
 * vertical, but sunrise and sunset are drawn where they actually fall. A six
 * o'clock sunrise lands due left and a six o'clock sunset due right, giving the
 * flat horizontal of the mock-up; every other pair tilts. A late sunrise swings
 * the left divider up above the horizontal, an early one dips it below, and the
 * sunset divider does the same on the right -- so the four parts of the day are
 * drawn at the size they really are, and a long summer evening looks long.
 */
#include "epd_weather.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "epd_5in83g.h"
#include "epd_font.h"
#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather_icons.h"

static const char *TAG = "epd-weather";

#define VIEW_W EPD_USABLE_WIDTH    /* 478 */
#define VIEW_H EPD_USABLE_HEIGHT   /* 460 */

#define CX (VIEW_W / 2)            /* 239 */
#define CY 238                     /* circle centre, below the title */
#define RING_R 165                 /* circle radius */
#define RING_T 2                   /* its line weight */

#define TITLE_Y 2

/*
 * A sector's contents sit out along the middle of its arc: icon, then the
 * condition, then the temperature, stacked into a 124 px column and centred on
 * that point.
 */
#define CONTENT_HALF_H ((WX_ICON_SIZE + 4 + 29 + 2 + 29) / 2)
#define TEXT_HALF_H    30          /* the same stack with the icon left off */
#define CONTENT_PAD     4          /* clearance between a label and a divider */

/*
 * How far past the ideal radius a group may still keep its icon. The width
 * test uses the corner of a bounding box, and the corner of a centred line of
 * text is almost always blank -- so a few pixels over is nothing you can see,
 * and losing the icon over it would be plainly worse.
 */
#define CONTENT_SLACK  12

#define ICON_DY     (-CONTENT_HALF_H)               /* icon top, from the centre */
#define WORD_DY     (ICON_DY + WX_ICON_SIZE + 4)
#define TEMP_DY     (WORD_DY + 31)

/*
 * How far out a sector's contents sit. A sector is a wedge, so the room across
 * it grows with the radius -- a narrow one therefore pushes its contents
 * outward to find width. Bounded at both ends: never so close in that the four
 * groups collide at the hub, never so far out that the stack leaves the rim.
 */
#define CONTENT_R_MIN 78
#define CONTENT_R_MAX (RING_R - CONTENT_HALF_H)

/*
 * Sunrise and sunset ride just outside the circle on their own dividers, so
 * they move with them. The times carry no AM or PM: there is no room for it,
 * and the icon has already said which end of the day this is.
 */
#define EDGE_R       (RING_R + 52)
#define EDGE_GROUP_H (WX_ICON_SIZE + 4 + 29)   /* icon, gap, one line of label */

#define PI_F 3.14159265f

/*
 * The moon used to straddle the foot of the circle, as in the mock-up. It now
 * sits wholly inside it: the foot is the night half and therefore black, so the
 * glyph is drawn white and has to stay on that background to be seen at all.
 * Its label drops below the rim onto the page, where black reads.
 */
#define MOON_ICON_Y (CY + RING_R - WX_ICON_SIZE - 6)
#define MOON_TEXT_Y (CY + RING_R + 8)

/* Draw a 1-bit icon with its top-left at (x, y). */
static void draw_icon(uint8_t *fb, int x, int y, wx_icon_t which, epd_color_t ink)
{
    const uint8_t *bits = WEATHER_ICONS[which].bits;

    for (int row = 0; row < WX_ICON_SIZE; row++) {
        const uint8_t *line = &bits[row * WX_ICON_STRIDE];

        for (int col = 0; col < WX_ICON_SIZE; col++) {
            if (line[col >> 3] & (0x80 >> (col & 7))) {
                paint_pixel(fb, x + col, y + row, ink);
            }
        }
    }
}

static void draw_icon_centered(uint8_t *fb, int cx, int y, wx_icon_t which,
                               epd_color_t ink)
{
    draw_icon(fb, cx - WX_ICON_SIZE / 2, y, which, ink);
}

/* "Wednesday, July 12" from the day being described. */
static void format_date(const wx_report_t *report, char *buf, size_t len)
{
    struct tm local;

    if (!report->valid || localtime_r(&report->day_start, &local) == NULL) {
        snprintf(buf, len, "No forecast");
        return;
    }
    strftime(buf, len, "%A, %B %e", &local);

    /* %e pads single digits with a space; close it up. */
    char *gap = strstr(buf, "  ");
    if (gap != NULL) {
        memmove(gap, gap + 1, strlen(gap));
    }
}

static void format_clock(time_t when, bool valid, char *buf, size_t len)
{
    struct tm local;

    if (!valid || localtime_r(&when, &local) == NULL) {
        snprintf(buf, len, "--:--");
        return;
    }

    strftime(buf, len, "%I:%M", &local);
    if (buf[0] == '0') {
        memmove(buf, buf + 1, strlen(buf));
    }
}

static void format_temp(const wx_window_t *part, wx_units_t units, char *buf, size_t len)
{
    if (!part->temp_valid) {
        snprintf(buf, len, "--");
        return;
    }

    /* Tenths, rounded away from zero so -0.5 does not print as 0. */
    const int whole = (part->temp_x10 + (part->temp_x10 < 0 ? -5 : 5)) / 10;
    snprintf(buf, len, "%d%s", whole, wx_unit_symbol(units));
}

/*
 * Where a local time sits on the dial, in radians clockwise from the top.
 * Noon is 0, six in the evening a quarter turn, midnight a half.
 */
static float dial_angle(time_t when, bool valid, float fallback_hour)
{
    float hour = fallback_hour;
    struct tm local;

    if (valid && localtime_r(&when, &local) != NULL) {
        hour = (float)local.tm_hour + (float)local.tm_min / 60.0f;
    }

    float angle = (hour - 12.0f) * (2.0f * PI_F / 24.0f);
    while (angle < 0.0f) {
        angle += 2.0f * PI_F;
    }
    while (angle >= 2.0f * PI_F) {
        angle -= 2.0f * PI_F;
    }
    return angle;
}

/* A point `r` from the centre at `angle`, measured clockwise from the top. */
static void dial_point(float angle, int r, int *x, int *y)
{
    *x = CX + (int)lroundf((float)r * sinf(angle));
    *y = CY - (int)lroundf((float)r * cosf(angle));
}

/* True for the two sectors that fall after dark. */
static bool angle_is_night(float angle, float sunrise_a, float sunset_a)
{
    return (angle >= sunset_a) && (angle < sunrise_a);
}

/*
 * The dial: a black rim, then every pixel inside it inked according to which
 * part of the day it belongs to -- yellow while the sun is up, black once it
 * is down. Filling by testing each pixel's own angle rather than by stitching
 * polygons together means the wedges meet the rim exactly, whatever the angles.
 *
 * atan2f per pixel is not cheap, but this runs once per refresh and the refresh
 * that follows it takes twenty-five seconds.
 */
static void draw_frame(uint8_t *fb, float sunrise_a, float sunset_a)
{
    paint_fill_circle(fb, CX, CY, RING_R, EPD_BLACK);

    const int inner = RING_R - RING_T;

    for (int dy = -inner; dy <= inner; dy++) {
        for (int dx = -inner; dx <= inner; dx++) {
            if (dx * dx + dy * dy > inner * inner) {
                continue;
            }

            /* Clockwise from the top, matching dial_angle(). */
            float angle = atan2f((float)dx, (float)-dy);
            if (angle < 0.0f) {
                angle += 2.0f * PI_F;
            }

            paint_pixel(fb, CX + dx, CY + dy,
                        angle_is_night(angle, sunrise_a, sunset_a) ? EPD_BLACK
                                                                   : EPD_YELLOW);
        }
    }

    /*
     * Noon to midnight. Only the daylight half needs drawing in black: below
     * the centre it divides two black sectors, so there it has to be white to
     * exist at all.
     */
    paint_fill_rect(fb, CX - RING_T / 2, CY - RING_R, RING_T, RING_R, EPD_BLACK);
    paint_fill_rect(fb, CX - RING_T / 2, CY, RING_T, RING_R, EPD_WHITE);

    /*
     * Sunrise and sunset need no line at all: they separate a yellow sector
     * from a black one, and that edge is already the sharpest thing on the
     * screen. Drawing over it in either colour would only thicken one side.
     */
}

/*
 * The four parts, each centred on the middle of its own arc. Going clockwise
 * from the top the dial reads noon, sunset, midnight, sunrise, which fixes
 * afternoon to the upper right, evening lower right, night lower left and
 * morning upper left -- the mock-up's arrangement, but with the boundaries
 * where the sun actually puts them.
 */
static void draw_sectors(uint8_t *fb, const wx_report_t *report, float sunrise_a,
                         float sunset_a)
{
    const struct {
        wx_part_t part;
        float from, to;
        bool night;
    } SECTORS[] = {
        {WX_AFTERNOON, 0.0f,             sunset_a,          false},
        {WX_EVENING,   sunset_a,         PI_F,              true},
        {WX_NIGHT,     PI_F,             sunrise_a,         true},
        {WX_MORNING,   sunrise_a,        2.0f * PI_F,       false},
    };

    for (size_t i = 0; i < sizeof(SECTORS) / sizeof(SECTORS[0]); i++) {
        const wx_window_t *win = &report->part[SECTORS[i].part];
        const char *word = wx_cond_label(win->cond);
        const float middle = (SECTORS[i].from + SECTORS[i].to) / 2.0f;
        const float half_angle = (SECTORS[i].to - SECTORS[i].from) / 2.0f;

        char temp[12];
        format_temp(win, report->units, temp, sizeof(temp));

        /*
         * Measure this sector's own contents rather than assume the worst: a
         * short word like RAIN then sits comfortably in close, and only a long
         * one like OVERCAST is pushed out to where the wedge has widened
         * enough to hold it.
         */
        int half_w = WX_ICON_SIZE / 2;
        const int word_w = font_text_width(&FONT_LABEL, word);
        if (word_w / 2 > half_w) {
            half_w = word_w / 2;
        }
        half_w += CONTENT_PAD;

        /*
         * What decides whether a divider cuts the group is not the width of
         * the wedge at the group's centre but at the group's INNERMOST corner,
         * which sits a half-height closer to the hub and is the first thing a
         * divider reaches. Solving for that corner is the difference between a
         * layout that looks right and one that is only right on the centre
         * line.
         */
        const float spread = tanf(half_angle);
        const float needed = (spread > 0.05f)
                                 ? (float)half_w / spread + (float)CONTENT_HALF_H
                                 : (float)RING_R;

        /*
         * A short winter day leaves a wide evening and a deep summer one leaves
         * a sliver, and a sliver cannot hold an icon and a word both. When even
         * the outermost usable radius is too narrow, drop the icon: without it
         * the group is half as tall and can sit far enough out for the wedge to
         * have opened up. Losing the picture beats having a divider drawn
         * through the word, which is where the information actually is.
         */
        const bool with_icon = (needed <= (float)(CONTENT_R_MAX + CONTENT_SLACK));
        /* Without the icon the group is half as tall, so it can sit further out. */
        const int limit = with_icon ? CONTENT_R_MAX : (RING_R - TEXT_HALF_H);
        const float target = with_icon
                                 ? needed
                                 : (float)half_w / spread + (float)TEXT_HALF_H;
        const int radius =
            (int)lroundf(fmaxf((float)CONTENT_R_MIN, fminf(target, (float)limit)));

        int qx, qy;
        dial_point(middle, radius, &qx, &qy);

        /* Black on the yellow daylight sectors, white on the black night ones. */
        const epd_color_t ink = SECTORS[i].night ? EPD_WHITE : EPD_BLACK;

        if (with_icon) {
            draw_icon_centered(fb, qx, qy + ICON_DY,
                               weather_icon_for(win->cond, SECTORS[i].night), ink);
            font_draw_text_centered(fb, qx, qy + WORD_DY, &FONT_LABEL, word, ink);
            font_draw_text_centered(fb, qx, qy + TEMP_DY, &FONT_LABEL, temp, ink);
        } else {
            font_draw_text_centered(fb, qx, qy - TEXT_HALF_H, &FONT_LABEL, word, ink);
            font_draw_text_centered(fb, qx, qy - TEXT_HALF_H + 31, &FONT_LABEL, temp,
                                    ink);
        }
    }
}

static int clamp_int(int v, int low, int high)
{
    if (v < low) {
        return low;
    }
    return (v > high) ? high : v;
}

/*
 * Sunrise and sunset, sitting just beyond the rim on their own dividers so they
 * rise and fall with them. Only 74 px of panel is left either side of the
 * circle, so a near-horizontal divider would push the group off the edge; the
 * clamp pulls it back on. That costs a little of the alignment in exactly the
 * case where the divider is flat and the alignment is obvious anyway.
 */
static void draw_one_sun_time(uint8_t *fb, float angle, wx_icon_t icon, time_t when,
                              bool valid)
{
    int x, y;
    dial_point(angle, EDGE_R, &x, &y);

    /* (x, y) is the middle of the group; the icon sits above, the time below. */
    const int half_w = WX_ICON_SIZE / 2;
    const int half_h = EDGE_GROUP_H / 2;
    x = clamp_int(x, half_w + 2, VIEW_W - half_w - 2);
    y = clamp_int(y, half_h + 2, VIEW_H - half_h - 2);

    draw_icon_centered(fb, x, y - half_h, icon, EPD_BLACK);

    char text[16];
    format_clock(when, valid, text, sizeof(text));
    font_draw_text_centered(fb, x, y - half_h + WX_ICON_SIZE + 4, &FONT_LABEL, text,
                            EPD_BLACK);
}

static void draw_sun_times(uint8_t *fb, const wx_report_t *report, float sunrise_a,
                           float sunset_a)
{
    draw_one_sun_time(fb, sunrise_a, WX_ICON_SUNRISE, report->sunrise,
                      report->valid && report->sunrise_valid);
    draw_one_sun_time(fb, sunset_a, WX_ICON_SUNSET, report->sunset,
                      report->valid && report->sunset_valid);
}

void epd_weather_draw(uint8_t *fb, const wx_report_t *report)
{
    paint_clear(fb, EPD_WHITE);

    char line[64];
    format_date(report, line, sizeof(line));
    font_draw_text_centered(fb, CX, TITLE_Y, &FONT_TITLE, line, EPD_BLACK);

    /*
     * Sunrise belongs to the dial's left half and sunset to its right. Clamping
     * them there keeps the four sectors in order and non-negative however odd
     * the pair is -- a polar day, or a location whose sun is out of step with
     * the timezone this clock is set to.
     */
    float sunrise_a = dial_angle(report->sunrise, report->valid && report->sunrise_valid,
                                 6.0f);
    float sunset_a = dial_angle(report->sunset, report->valid && report->sunset_valid,
                                18.0f);
    sunrise_a = fmaxf(PI_F, fminf(sunrise_a, 2.0f * PI_F));
    sunset_a = fmaxf(0.0f, fminf(sunset_a, PI_F));

    draw_frame(fb, sunrise_a, sunset_a);
    draw_sectors(fb, report, sunrise_a, sunset_a);
    draw_sun_times(fb, report, sunrise_a, sunset_a);

    draw_icon_centered(fb, CX, MOON_ICON_Y, weather_icon_for_moon(report->moon_phase),
                       EPD_WHITE);
    font_draw_text_centered(fb, CX, MOON_TEXT_Y, &FONT_LABEL, "MOON PHASE", EPD_BLACK);
}

esp_err_t epd_weather_show(const wx_report_t *report)
{
    uint8_t *fb = epd_fb_alloc();
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_NO_MEM, TAG, "no room for a %u byte frame",
                        (unsigned)EPD_FB_SIZE);

    epd_weather_draw(fb, report);

    esp_err_t err = epd_wake();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "refreshing -- this takes about 25 seconds");
        err = epd_display(fb);
    }
    epd_fb_free(fb);

    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(epd_sleep(), TAG, "sleep");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "weather displayed, panel asleep");
    return ESP_OK;
}
