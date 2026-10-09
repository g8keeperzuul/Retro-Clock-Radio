/*
 * epd_clockface.c - Analog clock face for the e-paper panel. See the header.
 *
 * Modelled on the dial in docs/pictures/clock-face.png: Art Deco numerals
 * (epd_font.h) standing outside a two-line chapter ring, an inner minute track
 * with wedges on the fives, a vertical cross line above and below the ring, and
 * SLEEP marked under it.
 *
 * That dial carries only eight numerals: its 12 and 6 sit behind the winder
 * knobs, and its 3 and 9 were never printed -- horizontal cross lines running
 * out to the edges of the plate stand in for them. Nothing hides anything here,
 * so all twelve are drawn, and the horizontal arms are left off: with a 3 and a
 * 9 on the plate they have nothing to stand in for and would strike through
 * both.
 */
#include "epd_clockface.h"

#include <math.h>
#include <stdio.h>

#include "epd_5in83g.h"
#include "epd_font.h"
#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd-clock";

/* The dial fills the usable box; the box is 18 px wider than it is tall. */
#define VIEW_W EPD_USABLE_WIDTH
#define VIEW_H EPD_USABLE_HEIGHT
#define CX     (VIEW_W / 2)
#define CY     (VIEW_H / 2)

/*
 * Radii, all from the centre outwards. Proportions follow the photographed
 * dial, scaled to the half-height -- the tighter of the two axes, and what
 * decides whether the 12 and the 6 clear the edge of the box.
 */
#define R_NUMERAL  196   /* centre of the hour numerals */
#define R_RING_OUT 138   /* outer line of the chapter ring */
#define R_RING_IN  120   /* inner line of the chapter ring */
#define R_MIN_OUT  112   /* outer end of the minute track */
#define R_MIN_IN   102   /* inner end of a plain minute tick */
#define R_FIVE_IN   92   /* apex of a five-minute wedge */
#define R_QUARTER  106   /* the dots that stand at 12, 3, 6 and 9 */

#define RING_LINE  2     /* thickness of each chapter-ring circle */
#define HUB_R      5

/*
 * The hands, shaped like the pair in the photograph: a broad flat base at the
 * boss tapering to a narrow flat tip, no point and no tail. Half-widths, so
 * HOUR_W0 of 6.5 is a 13 px base. The minute hand reaches into the minute
 * track; the hour hand stops around two thirds of the way out.
 */
#define HAND_BASE_R  8     /* both hands start clear of the boss */
#define HOUR_R      72
#define HOUR_W0      6.5f
#define HOUR_W1      2.5f
#define MINUTE_R   106
#define MINUTE_W0    6.0f
#define MINUTE_W1    2.0f

#define SLEEP_SCALE 2    /* SLEEP keeps the plain built-in font */

/* Where SLEEP sits below the ring, and where the 12 o'clock line stops short. */
#define SLEEP_Y (CY + 146)

#define NUM_HALF_H (FONT_DIAL.line_height / 2)

/* Radians per minute mark, spelled out rather than leaning on M_PI. */
#define RAD_PER_MINUTE (2.0f * 3.14159265f / 60.0f)

/* Point `r` px from the centre at the angle of `minute` on a 60-minute dial. */
static void polar(float minute, int r, int *x, int *y)
{
    const float a = minute * RAD_PER_MINUTE;
    *x = CX + (int)lroundf((float)r * sinf(a));
    *y = CY - (int)lroundf((float)r * cosf(a));
}

/*
 * A radial spoke as a quadrilateral: half-width w0 where it meets r0, w1 where
 * it meets r1. Equal widths give a tick, a zero inner width gives a wedge.
 * Drawn as two triangles because that is the widest primitive paint offers.
 */
static void draw_spoke(uint8_t *fb, float minute, int r0, int r1, float w0, float w1,
                       epd_color_t color)
{
    const float a = minute * RAD_PER_MINUTE;
    const float dx = sinf(a), dy = -cosf(a);   /* outward */
    const float px = cosf(a), py = sinf(a);    /* across  */

    const int ax0 = CX + (int)lroundf(dx * r0 + px * w0);
    const int ay0 = CY + (int)lroundf(dy * r0 + py * w0);
    const int bx0 = CX + (int)lroundf(dx * r0 - px * w0);
    const int by0 = CY + (int)lroundf(dy * r0 - py * w0);
    const int ax1 = CX + (int)lroundf(dx * r1 + px * w1);
    const int ay1 = CY + (int)lroundf(dy * r1 + py * w1);
    const int bx1 = CX + (int)lroundf(dx * r1 - px * w1);
    const int by1 = CY + (int)lroundf(dy * r1 - py * w1);

    paint_fill_triangle(fb, ax0, ay0, ax1, ay1, bx1, by1, color);
    paint_fill_triangle(fb, ax0, ay0, bx1, by1, bx0, by0, color);
}

/* A circle drawn as a line: the outline the paint layer does not provide. */
static void draw_circle_outline(uint8_t *fb, int r, int thickness, epd_color_t color)
{
    paint_fill_circle(fb, CX, CY, r, color);
    paint_fill_circle(fb, CX, CY, r - thickness, EPD_WHITE);
}

/*
 * The chapter ring: two concentric lines with a radial tick across the band at
 * every hour, exactly as the original is engraved.
 */
static void draw_chapter_ring(uint8_t *fb)
{
    draw_circle_outline(fb, R_RING_OUT, RING_LINE, EPD_BLACK);
    draw_circle_outline(fb, R_RING_IN, RING_LINE, EPD_BLACK);

    for (int hour = 0; hour < 12; hour++) {
        draw_spoke(fb, hour * 5.0f, R_RING_IN, R_RING_OUT, 1.5f, 1.5f, EPD_BLACK);
    }
}

/*
 * The minute track inside the ring: a hairline at every minute, a wedge on the
 * fives, and a dot where a quarter falls -- the quarters carry the cross lines
 * instead of a wedge.
 */
static void draw_minute_track(uint8_t *fb)
{
    for (int m = 0; m < 60; m++) {
        if (m % 15 == 0) {
            int x, y;
            polar((float)m, R_QUARTER, &x, &y);
            paint_fill_circle(fb, x, y, 4, EPD_BLACK);
        } else if (m % 5 == 0) {
            draw_spoke(fb, (float)m, R_MIN_OUT, R_FIVE_IN, 4.0f, 0.0f, EPD_RED);
        } else {
            draw_spoke(fb, (float)m, R_MIN_OUT, R_MIN_IN, 1.0f, 1.0f, EPD_BLACK);
        }
    }
}

/*
 * The vertical cross line, above and below the ring.
 *
 * The original dial has a horizontal pair as well, running out to the edges of
 * the plate: on that face they stand in for a 3 and a 9 that were never
 * printed. Both numerals are drawn here, so the arms have nothing left to say
 * and would only strike through them.
 *
 * The vertical line cannot run to the edges either, for the same reason -- a
 * numeral sits at each end of it -- so it stops short at both: above SLEEP at
 * the bottom, under the 12 at the top.
 */
static void draw_cross_lines(uint8_t *fb)
{
    const int t = 2;

    const int top_from = CY - R_NUMERAL + NUM_HALF_H + 8;
    paint_fill_rect(fb, CX - t / 2, top_from, t, (CY - R_QUARTER) - top_from, EPD_BLACK);
    //paint_fill_rect(fb, CX - t / 2, CY + R_QUARTER, t, (SLEEP_Y - 6) - (CY + R_QUARTER), EPD_BLACK);
    paint_fill_rect(fb, CX - t / 2, CY + R_QUARTER, t, SLEEP_Y - (CY + R_QUARTER), EPD_BLACK);
}

/*
 * Hour numerals, solid black. The dial used to set them in yellow behind a
 * black outline, which suited the blocky face it had then; Rigelsia's stems are
 * three pixels wide and an outline would leave nothing of them.
 */
static void draw_numeral(uint8_t *fb, int cx, int cy, const char *text)
{
    font_draw_text_centered(fb, cx, cy - NUM_HALF_H, &FONT_DIAL, text, EPD_BLACK);
}

static void draw_numerals(uint8_t *fb)
{
    for (int hour = 1; hour <= 12; hour++) {
        int x, y;
        polar(hour * 5.0f, R_NUMERAL, &x, &y);

        char label[3];
        snprintf(label, sizeof(label), "%d", hour);
        draw_numeral(fb, x, y, label);
    }
}

void epd_clockface_draw_hands(uint8_t *fb, int hour, int minute)
{
    /*
     * The hour hand sits between numerals in proportion to the minutes: at
     * half past, it is halfway to the next hour. Twelve hours is sixty minute
     * marks, so an hour is five of them and a minute is a twelfth of one.
     */
    const float hour_pos = (float)(hour % 12) * 5.0f + (float)minute / 12.0f;

    draw_spoke(fb, hour_pos, HAND_BASE_R, HOUR_R, HOUR_W0, HOUR_W1, EPD_BLACK);
    draw_spoke(fb, (float)minute, HAND_BASE_R, MINUTE_R, MINUTE_W0, MINUTE_W1, EPD_BLACK);

    /* The boss goes on last, covering both bases as it does on the original. */
    paint_fill_circle(fb, CX, CY, HUB_R, EPD_BLACK);
}

void epd_clockface_draw(uint8_t *fb)
{
    /* Clears the dead margins too; everything after this clips to the box. */
    paint_clear(fb, EPD_WHITE);

    draw_chapter_ring(fb);
    draw_minute_track(fb);
    draw_cross_lines(fb);
    draw_numerals(fb);

    //paint_text_centered(fb, CX, SLEEP_Y, "SLEEP", SLEEP_SCALE, EPD_BLACK);

    /* The hub the hands would turn on, drawn even though no hands are. */
    paint_fill_circle(fb, CX, CY, HUB_R, EPD_BLACK);
}

/* Shared by both entry points; `hour` below zero means draw the bare dial. */
static esp_err_t clockface_show(int hour, int minute)
{
    uint8_t *fb = epd_fb_alloc();
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_NO_MEM, TAG, "no room for a %u byte frame",
                        (unsigned)EPD_FB_SIZE);

    epd_clockface_draw(fb);
    if (hour >= 0) {
        epd_clockface_draw_hands(fb, hour, minute);
    }

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

    ESP_LOGI(TAG, "clock face displayed, panel asleep");
    return ESP_OK;
}

esp_err_t epd_clockface_show(void)
{
    return clockface_show(-1, 0);
}

esp_err_t epd_clockface_show_time(int hour, int minute)
{
    ESP_LOGI(TAG, "showing %02d:%02d", hour, minute);
    return clockface_show(hour, minute);
}
