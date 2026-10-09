/*
 * epd_forecast.c - Today and tomorrow, stacked. See the header.
 */
#include "epd_forecast.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "epd_font.h"
#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather_icons.h"

static const char *TAG = "epd-forecast";

/*
 * Every word on this screen is small type, and small type on this panel needs
 * the heavier cut -- a one-pixel stem is a grey line however black the frame
 * buffer says it is. One alias rather than forty call sites, so the whole
 * screen can be moved back to FONT_LABEL by changing this line.
 */
#define FACE FONT_LABEL_HEAVY

#define VIEW_W EPD_USABLE_WIDTH    /* 478 */
#define VIEW_H EPD_USABLE_HEIGHT   /* 460 */

/* Two equal halves, each framed so the eye does not have to find the divide. */
#define HALF_H (VIEW_H / 2)        /* 230 */
#define FRAME_T 3

/*
 * The five columns, and where their contents sit inside a half.
 *
 * Vertical space is spent rather than guessed: a half is 230 tall, the frame
 * takes 3 at each end, and what is left carries a 48 px title row, a 48 px icon
 * row and two 29 px lines of figures. That totals 154, leaving 70 for five
 * equal gaps -- the two margins and the three divisions -- which is where the
 * 14 below comes from. Change a row height and this has to be redone.
 */
#define GAP        14
#define ROW_TITLE  (FRAME_T + GAP)                      /* 17 */
#define ROW_ICONS  (ROW_TITLE + WX_ICON_SIZE + GAP)     /* 79 */
#define ROW_VALUE  (ROW_ICONS + WX_ICON_SIZE + GAP)     /* 141 */
#define ROW_UNDER  (ROW_VALUE + 29 + GAP)               /* 184 */

#define COLUMNS 5
#define COL_W   (VIEW_W / COLUMNS)                      /* 95 */

/* Which column each measure occupies, left to right as in the template. */
#define COL_TEMP   0
#define COL_RH     1
#define COL_PRECIP 2
#define COL_WIND   3
#define COL_CLOUD  4

static int column_centre(int col)
{
    return col * COL_W + COL_W / 2;
}

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

/*
 * "OCT 7". Not strftime("%b %-d"): %-d is a GNU extension the newlib on this
 * part does not carry, and the month has to be upper case anyway because that
 * is the only case the small face was cut with.
 */
static void format_date(char *out, size_t n, time_t day_start)
{
    static const char *MONTHS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    struct tm local;

    localtime_r(&day_start, &local);
    snprintf(out, n, "%s %d", MONTHS[local.tm_mon % 12], local.tm_mday);
}

/* A tenth-degree temperature as the panel shows it: "20" or "-3". */
static void format_temp(char *out, size_t n, int16_t x10)
{
    snprintf(out, n, "%d", (x10 >= 0) ? (x10 + 5) / 10 : -((-x10 + 5) / 10));
}

/*
 * A small triangle, for the arrows that mark the day's high and low. Drawn
 * rather than set: the face has no such glyph, and two filled triangles are
 * cheaper than cutting one.
 */
static void draw_arrow(uint8_t *fb, int x, int y, int w, int h, bool up)
{
    if (up) {
        paint_fill_triangle(fb, x + w / 2, y, x + w, y + h, x, y + h, EPD_BLACK);
    } else {
        paint_fill_triangle(fb, x, y, x + w, y, x + w / 2, y + h, EPD_BLACK);
    }
}

#define ARROW_W 20
#define ARROW_H 16
#define ARROW_GAP 4

/*
 * The degree ring, drawn rather than set. The face was cut from ASCII only and
 * the degree sign is not in it; a ring and a disc of the background colour cost
 * two calls and look better than the alternatives of "17 C" or a raised "o".
 */
#define DEG_R 5
#define DEG_W (DEG_R * 2 + 3)

static void draw_degrees(uint8_t *fb, int x, int y)
{
    /* Sat against the top of the digits, where a degree sign belongs. */
    const int cy = y + DEG_R + 2;

    paint_fill_circle(fb, x + DEG_R, cy, DEG_R, EPD_BLACK);
    paint_fill_circle(fb, x + DEG_R, cy, DEG_R - 2, EPD_WHITE);
}

/* An arrow, a temperature and its unit, together centred on `cx`. */
static void draw_temp_line(uint8_t *fb, int cx, int y, int16_t x10, bool up)
{
    char text[12];
    format_temp(text, sizeof(text), x10);

    const int tw = font_text_width(&FACE, text);
    const int cw = font_text_width(&FACE, "C");
    const int total = ARROW_W + ARROW_GAP + tw + DEG_W + cw;
    int x = cx - total / 2;

    draw_arrow(fb, x, y + (FACE.line_height - ARROW_H) / 2, ARROW_W, ARROW_H, up);
    x += ARROW_W + ARROW_GAP;

    font_draw_text(fb, x, y, &FACE, text, EPD_BLACK);
    x += tw;

    draw_degrees(fb, x, y);
    font_draw_text(fb, x + DEG_W, y, &FACE, "C", EPD_BLACK);
}

/*
 * The heading: date, the day's condition glyph, and what that condition is
 * called. The three are measured and then centred as one group, so the icon
 * stays beside its words however long they are.
 */
static void draw_heading(uint8_t *fb, int y, const wx_daysum_t *day)
{
    char date[16];
    format_date(date, sizeof(date), day->day_start);

    char head[20];
    snprintf(head, sizeof(head), "%s:", date);

    const char *words = wx_cond_description(day->cond_id);
    const int gap = 10;
    const int budget = VIEW_W - 2 * (FRAME_T + 4);

    const int head_w = font_text_width(&FACE, head);
    int words_w = font_text_width(&FACE, words);

    /*
     * Five of OpenWeather's fifty-five phrasings are too long to sit beside
     * both a date and an icon -- "THUNDERSTORM WITH HEAVY DRIZZLE" is the
     * worst. What gives way is the icon, not the words: the glyph says nothing
     * the words do not, so dropping it loses nothing, where shortening the
     * words to fit the glyph would lose the difference between drizzle and
     * hail. Only an id outside the table falls all the way back to the
     * project's own coarser vocabulary.
     */
    bool with_icon = (head_w + gap + WX_ICON_SIZE + gap + words_w) <= budget;

    if (!with_icon && head_w + gap + words_w > budget) {
        words = wx_cond_label(wx_cond_from_id(day->cond_id));
        words_w = font_text_width(&FACE, words);
        with_icon = (head_w + gap + WX_ICON_SIZE + gap + words_w) <= budget;
    }

    const int total = head_w + gap + words_w +
                      (with_icon ? WX_ICON_SIZE + gap : 0);

    int x = (VIEW_W - total) / 2;
    if (x < FRAME_T + 4) {
        x = FRAME_T + 4;
    }

    /* The words sit on the icon's optical centre, not its top. */
    const int text_y = y + (WX_ICON_SIZE - FACE.line_height) / 2;

    font_draw_text(fb, x, text_y, &FACE, head, EPD_BLACK);
    x += head_w + gap;

    if (with_icon) {
        draw_icon(fb, x, y, weather_icon_for(wx_cond_from_id(day->cond_id), false));
        x += WX_ICON_SIZE + gap;
    }

    font_draw_text(fb, x, text_y, &FACE, words, EPD_BLACK);
}

static void draw_half(uint8_t *fb, int y0, const wx_daysum_t *day)
{
    paint_rect(fb, 0, y0, VIEW_W, HALF_H, FRAME_T, EPD_BLACK);

    if (day == NULL || !day->valid) {
        font_draw_text_centered(fb, VIEW_W / 2,
                                y0 + (HALF_H - FACE.line_height) / 2,
                                &FACE, "NO FORECAST YET", EPD_BLACK);
        return;
    }

    draw_heading(fb, y0 + ROW_TITLE, day);

    static const wx_icon_t MEASURES[COLUMNS] = {
        [COL_TEMP]   = WX_ICON_THERMOMETER,
        [COL_RH]     = WX_ICON_HUMIDITY,
        [COL_PRECIP] = WX_ICON_UMBRELLA,
        [COL_WIND]   = WX_ICON_WIND,
        [COL_CLOUD]  = WX_ICON_CLOUDCOVER,
    };

    for (int col = 0; col < COLUMNS; col++) {
        draw_icon_centered(fb, column_centre(col), y0 + ROW_ICONS, MEASURES[col]);
    }

    char text[16];
    const int value_y = y0 + ROW_VALUE;
    const int under_y = y0 + ROW_UNDER;

    /* Temperature: the high on the value line, the low beneath it. */
    draw_temp_line(fb, column_centre(COL_TEMP), value_y, day->temp_max_x10, true);
    draw_temp_line(fb, column_centre(COL_TEMP), under_y, day->temp_min_x10, false);

    snprintf(text, sizeof(text), "%u%%", (unsigned)day->humidity_pct);
    font_draw_text_centered(fb, column_centre(COL_RH), value_y, &FACE, text,
                            EPD_BLACK);

    snprintf(text, sizeof(text), "%u%%", (unsigned)day->pop_pct);
    font_draw_text_centered(fb, column_centre(COL_PRECIP), value_y, &FACE, text,
                            EPD_BLACK);

    /*
     * The amount only appears when there is one. A standing "0.0 MM" under a
     * dry day is a line of type that says nothing.
     */
    if (day->precip_x100 > 0) {
        snprintf(text, sizeof(text), "%u.%u MM", (unsigned)(day->precip_x100 / 100),
                 (unsigned)((day->precip_x100 % 100) / 10));
        font_draw_text_centered(fb, column_centre(COL_PRECIP), under_y, &FACE,
                                text, EPD_BLACK);
    }

    /* The API gives metres per second; kilometres per hour is what is read. */
    snprintf(text, sizeof(text), "%u", (unsigned)((day->gust_x10 * 36 + 50) / 100));
    font_draw_text_centered(fb, column_centre(COL_WIND), value_y, &FACE, text,
                            EPD_BLACK);
    font_draw_text_centered(fb, column_centre(COL_WIND), under_y, &FACE, "KM/H",
                            EPD_BLACK);

    snprintf(text, sizeof(text), "%u%%", (unsigned)day->clouds_pct);
    font_draw_text_centered(fb, column_centre(COL_CLOUD), value_y, &FACE, text,
                            EPD_BLACK);
}

void epd_forecast_draw(uint8_t *fb, const wx_daysum_t *today,
                       const wx_daysum_t *tomorrow)
{
    paint_clear(fb, EPD_WHITE);

    draw_half(fb, 0, today);
    draw_half(fb, HALF_H, tomorrow);
}

esp_err_t epd_forecast_show(const wx_daysum_t *today, const wx_daysum_t *tomorrow)
{
    uint8_t *fb = epd_fb_alloc();
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_NO_MEM, TAG, "no room for a %u byte frame",
                        (unsigned)EPD_FB_SIZE);

    epd_forecast_draw(fb, today, tomorrow);

    esp_err_t err = epd_wake();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "refreshing -- this takes about 25 seconds");
        err = epd_display(fb);
    }
    epd_fb_free(fb);

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Not optional, and not merely about power. epd_display() leaves the panel
     * driving; the POWER_OFF inside epd_sleep() is what stops it. Left
     * energised after the waveform has finished, the panel goes on pushing
     * pigment and the image it just built visibly washes out -- high contrast
     * at the end of the refresh, grey a moment later.
     */
    ESP_RETURN_ON_ERROR(epd_sleep(), TAG, "sleep");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "forecast displayed, panel asleep");
    return ESP_OK;
}
