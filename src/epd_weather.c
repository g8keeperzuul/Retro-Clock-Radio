/*
 * epd_weather.c - The daily weather screen. See the header.
 *
 * The mock-up is square; the usable box is 478 x 460, which is squarer still
 * but not square. The circle therefore shrinks to leave room for the sunrise
 * and sunset blocks either side of it, and everything else is pinned to the
 * circle rather than to the panel.
 */
#include "epd_weather.h"

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
 * Quadrant contents sit on a diagonal from the centre, clear of both dividing
 * lines: icon, then the condition, then the temperature. The offsets stack a
 * 124 px column and centre it, so the whole group stays inside the circle --
 * the binding constraint is the far bottom corner of a bottom quadrant.
 */
#define QUAD_OFFSET 58             /* centre of a quadrant's contents */
#define ICON_DY     (-62)          /* icon top, relative to that centre */
#define WORD_DY       2
#define TEMP_DY      33

/*
 * Sunrise and sunset live in the 74 px the circle leaves either side. The times
 * carry no AM or PM: there is no room for it, and the icon has already said
 * which end of the day this is.
 */
#define EDGE_ICON_Y (CY - 50)
#define EDGE_TEXT_Y (CY + 14)

/* The moon straddles the foot of the circle, as it does in the mock-up. */
#define MOON_ICON_Y 368
#define MOON_TEXT_Y 430

/* Draw a 1-bit icon with its top-left at (x, y). */
static void draw_icon(uint8_t *fb, int x, int y, wx_icon_t which)
{
    const uint8_t *bits = WEATHER_ICONS[which].bits;

    for (int row = 0; row < WX_ICON_SIZE; row++) {
        const uint8_t *line = &bits[row * WX_ICON_STRIDE];

        for (int col = 0; col < WX_ICON_SIZE; col++) {
            if (line[col >> 3] & (0x80 >> (col & 7))) {
                paint_pixel(fb, x + col, y + row, EPD_BLACK);
            }
        }
    }
}

static void draw_icon_centered(uint8_t *fb, int cx, int y, wx_icon_t which)
{
    draw_icon(fb, cx - WX_ICON_SIZE / 2, y, which);
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

/* The circle, and the two lines that quarter it. */
static void draw_frame(uint8_t *fb)
{
    paint_fill_circle(fb, CX, CY, RING_R, EPD_BLACK);
    paint_fill_circle(fb, CX, CY, RING_R - RING_T, EPD_WHITE);

    paint_fill_rect(fb, CX - RING_T / 2, CY - RING_R, RING_T, RING_R * 2, EPD_BLACK);
    paint_fill_rect(fb, CX - RING_R, CY - RING_T / 2, RING_R * 2, RING_T, EPD_BLACK);
}

/*
 * The four parts, clockwise from the top left. That order is not arbitrary: the
 * mock-up puts sunrise at nine o'clock, sunset at three and the moon at the
 * bottom, which fixes morning top-left, afternoon top-right, evening
 * bottom-right and night bottom-left.
 */
static void draw_quadrants(uint8_t *fb, const wx_report_t *report)
{
    static const struct {
        wx_part_t part;
        int dx, dy;
    } PLACES[] = {
        {WX_MORNING,   -1, -1},
        {WX_AFTERNOON,  1, -1},
        {WX_EVENING,    1,  1},
        {WX_NIGHT,     -1,  1},
    };

    for (size_t i = 0; i < sizeof(PLACES) / sizeof(PLACES[0]); i++) {
        const wx_window_t *win = &report->part[PLACES[i].part];
        const int qx = CX + PLACES[i].dx * QUAD_OFFSET;
        const int qy = CY + PLACES[i].dy * QUAD_OFFSET;

        /* Evening and night take the night form of the icon. */
        const bool night = (PLACES[i].part == WX_EVENING || PLACES[i].part == WX_NIGHT);

        draw_icon_centered(fb, qx, qy + ICON_DY, weather_icon_for(win->cond, night));
        font_draw_text_centered(fb, qx, qy + WORD_DY, &FONT_LABEL,
                                wx_cond_label(win->cond), EPD_BLACK);

        char temp[12];
        format_temp(win, report->units, temp, sizeof(temp));
        font_draw_text_centered(fb, qx, qy + TEMP_DY, &FONT_LABEL, temp, EPD_BLACK);
    }
}

/* Sunrise on the left and sunset on the right, on the circle's own axis. */
static void draw_sun_times(uint8_t *fb, const wx_report_t *report)
{
    const int left = (CX - RING_R) / 2;
    const int right = CX + RING_R + (VIEW_W - CX - RING_R) / 2;
    char when[16];

    draw_icon_centered(fb, left, EDGE_ICON_Y, WX_ICON_SUNRISE);
    format_clock(report->sunrise, report->valid && report->sunrise_valid,
                 when, sizeof(when));
    font_draw_text_centered(fb, left, EDGE_TEXT_Y, &FONT_LABEL, when, EPD_BLACK);

    draw_icon_centered(fb, right, EDGE_ICON_Y, WX_ICON_SUNSET);
    format_clock(report->sunset, report->valid && report->sunset_valid,
                 when, sizeof(when));
    font_draw_text_centered(fb, right, EDGE_TEXT_Y, &FONT_LABEL, when, EPD_BLACK);
}

void epd_weather_draw(uint8_t *fb, const wx_report_t *report)
{
    paint_clear(fb, EPD_WHITE);

    char line[64];
    format_date(report, line, sizeof(line));
    font_draw_text_centered(fb, CX, TITLE_Y, &FONT_TITLE, line, EPD_BLACK);

    draw_frame(fb);
    draw_quadrants(fb, report);
    draw_sun_times(fb, report);

    draw_icon_centered(fb, CX, MOON_ICON_Y, weather_icon_for_moon(report->moon_phase));
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
