/*
 * epd_clockface.c - Analog clock face for the e-paper panel. See the header.
 *
 * Modelled on the dial in docs/pictures/clock-face.png: outlined Art Deco
 * numerals (epd_deco_font.h) standing outside a two-line chapter ring, an inner minute track with wedges
 * on the fives, cross lines running out to the edges of the plate, and SLEEP
 * marked under the ring.
 *
 * That dial carries only eight numerals. Its 12 and 6 sit behind the winder
 * knobs and its 3 and 9 were never printed -- the cross line and its dot stand
 * in for them. Nothing hides 12 and 6 here, so both are drawn; 3 and 9 keep the
 * original's line-and-dot treatment.
 */
#include "epd_clockface.h"

#include <math.h>
#include <stdio.h>

#include "epd_5in83g.h"
#include "epd_deco_font.h"
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

#define NUM_OUTLINE 1    /* how far the black outline stands off the fill */
#define NUM_GAP     5    /* between the figures of a two-figure numeral */
#define SLEEP_SCALE 2    /* SLEEP keeps the plain built-in font */

/* Where SLEEP sits below the ring, and where the 12 o'clock line stops short. */
#define SLEEP_Y (CY + 146)

#define NUM_HALF_H (DECO_GLYPH_H / 2)

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
 * The cross lines. Left and right run clear out to the edge of the plate, which
 * is what stands in for the missing 3 and 9. The vertical pair cannot: a
 * numeral now sits at each end of it, so those two stop short -- above SLEEP at
 * the bottom, under the 12 at the top.
 */
static void draw_cross_lines(uint8_t *fb)
{
    const int edge = 6;
    const int t = 2;

    paint_fill_rect(fb, edge, CY - t / 2, CX - R_QUARTER - edge, t, EPD_BLACK);
    paint_fill_rect(fb, CX + R_QUARTER, CY - t / 2, VIEW_W - edge - (CX + R_QUARTER), t, EPD_BLACK);

    const int top_from = CY - R_NUMERAL + NUM_HALF_H + 8;
    paint_fill_rect(fb, CX - t / 2, top_from, t, (CY - R_QUARTER) - top_from, EPD_BLACK);
    //paint_fill_rect(fb, CX - t / 2, CY + R_QUARTER, t, (SLEEP_Y - 6) - (CY + R_QUARTER), EPD_BLACK);
    paint_fill_rect(fb, CX - t / 2, CY + R_QUARTER, t, SLEEP_Y - (CY + R_QUARTER), EPD_BLACK);
}

/* Set width of a numeral, figures proportionally spaced. */
static int numeral_width(const char *text)
{
    int width = -NUM_GAP;

    for (const char *p = text; *p != '\0'; p++) {
        width += DECO_DIGITS[*p - '0'].w + NUM_GAP;
    }
    return width;
}

/*
 * Hour numerals in the Deco set, yellow with a black outline -- the nearest the
 * four inks come to the original's dark-edged tan. The outline is eight offset
 * copies of the glyph rather than a real stroke: same look at one pixel, and it
 * costs nothing but time.
 */
static void draw_numeral(uint8_t *fb, int cx, int cy, const char *text)
{
    const int y = cy - NUM_HALF_H;
    int x = cx - numeral_width(text) / 2;

    for (const char *p = text; *p != '\0'; p++) {
        const deco_glyph_t *glyph = &DECO_DIGITS[*p - '0'];

        for (int dy = -NUM_OUTLINE; dy <= NUM_OUTLINE; dy++) {
            for (int dx = -NUM_OUTLINE; dx <= NUM_OUTLINE; dx++) {
                if (dx != 0 || dy != 0) {
                    paint_bitmap(fb, x + dx, y + dy, glyph->rows, glyph->w, DECO_GLYPH_H,
                                 EPD_BLACK);
                }
            }
        }
        paint_bitmap(fb, x, y, glyph->rows, glyph->w, DECO_GLYPH_H, EPD_YELLOW);

        x += glyph->w + NUM_GAP;
    }
}

static void draw_numerals(uint8_t *fb)
{
    for (int hour = 1; hour <= 12; hour++) {
        /* 3 and 9 are the cross line's business, as they are on the original. */
        /*
        if (hour == 3 || hour == 9) {
            continue;
        }
        */

        int x, y;
        polar(hour * 5.0f, R_NUMERAL, &x, &y);

        char label[3];
        snprintf(label, sizeof(label), "%d", hour);
        draw_numeral(fb, x, y, label);
    }
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

esp_err_t epd_clockface_show(void)
{
    uint8_t *fb = epd_fb_alloc();
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_NO_MEM, TAG, "no room for a %u byte frame",
                        (unsigned)EPD_FB_SIZE);

    epd_clockface_draw(fb);

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
