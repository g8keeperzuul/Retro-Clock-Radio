/*
 * epd_pattern.c - Four-colour e-paper bring-up test pattern.
 *
 * Everything is laid out inside the usable box, in the box's own coordinates,
 * so the pattern doubles as a check that the visible window really is where the
 * firmware thinks it is: the frame is flush to all four of its sides, and ink
 * spilling past the right-hand or bottom one -- or a strip of white above the
 * top one -- would mean the crop is wrong.
 *
 * The pattern answers three questions at a glance:
 *
 *   1. Do all four inks fire?      Labelled swatches, one per colour code.
 *   2. Which way up is the panel?  Corner markers of four different shapes, an
 *                                  arrow pointing at the top edge, and every
 *                                  edge named. Reading the labels also catches
 *                                  a mirrored scan direction, which a purely
 *                                  symmetric pattern would hide.
 *   3. Is the geometry right?      The frame plus edge ticks: a row-stride
 *                                  error skews them, an off-by-one in the
 *                                  resolution registers clips them.
 */
#include "epd_pattern.h"

#include "epd_5in83g.h"
#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd-pattern";

/* The pattern's own canvas: the usable box, smaller than the panel on two sides. */
#define VIEW_W   EPD_USABLE_WIDTH    /* 478 */
#define VIEW_H   EPD_USABLE_HEIGHT   /* 460 */
#define CENTER_X (VIEW_W / 2)

#define BORDER    4    /* outer frame thickness */
#define MARK_SIZE 48   /* corner marker square */
#define MARK_PAD  12   /* gap from the usable edge to the marker */

#define MARK_RIGHT (VIEW_W - MARK_PAD - MARK_SIZE)  /* 418 */
#define MARK_BOT   (VIEW_H - MARK_PAD - MARK_SIZE)  /* 400 */

/* Text sizes: 3 for the edge names and the headline, 2 for everything else. */
#define EDGE_SCALE 3
#define NOTE_SCALE 2

static void draw_text_right(uint8_t *fb, int right, int y, const char *text, int scale,
                            epd_color_t color)
{
    paint_text(fb, right - paint_text_width(text, scale), y, text, scale, color);
}

/*
 * Each corner gets a different shape, so orientation survives even if the
 * labels are too small to read from across the room.
 */
static void draw_corner_markers(uint8_t *fb)
{
    /* Top-left: solid red square. */
    paint_fill_rect(fb, MARK_PAD, MARK_PAD, MARK_SIZE, MARK_SIZE, EPD_RED);
    paint_rect(fb, MARK_PAD, MARK_PAD, MARK_SIZE, MARK_SIZE, 3, EPD_BLACK);

    /* Top-right: yellow disc, ringed in black so it reads against the white. */
    paint_fill_circle(fb, MARK_RIGHT + MARK_SIZE / 2, MARK_PAD + MARK_SIZE / 2,
                      MARK_SIZE / 2, EPD_BLACK);
    paint_fill_circle(fb, MARK_RIGHT + MARK_SIZE / 2, MARK_PAD + MARK_SIZE / 2,
                      MARK_SIZE / 2 - 4, EPD_YELLOW);

    /* Bottom-left: black triangle, right angle tucked into the corner. */
    paint_fill_triangle(fb, MARK_PAD, MARK_BOT,
                        MARK_PAD, MARK_BOT + MARK_SIZE,
                        MARK_PAD + MARK_SIZE, MARK_BOT + MARK_SIZE, EPD_BLACK);

    /* Bottom-right: all four inks in one square, in colour-code order. */
    const int half = MARK_SIZE / 2;
    paint_fill_rect(fb, MARK_RIGHT, MARK_BOT, half, half, EPD_BLACK);
    paint_fill_rect(fb, MARK_RIGHT + half, MARK_BOT, half, half, EPD_WHITE);
    paint_fill_rect(fb, MARK_RIGHT, MARK_BOT + half, half, half, EPD_YELLOW);
    paint_fill_rect(fb, MARK_RIGHT + half, MARK_BOT + half, half, half, EPD_RED);
    paint_rect(fb, MARK_RIGHT, MARK_BOT, MARK_SIZE, MARK_SIZE, 3, EPD_BLACK);

    /*
     * Corner names with the usable coordinate each marker sits against -- box
     * coordinates, so the far corner is (477,459) and not the panel's
     * (647,479). That is the whole point of the exercise: a marker sitting
     * anywhere but hard against the label's coordinate means the crop is off.
     */
    paint_text(fb, MARK_PAD, 66, "TL (0,0)", NOTE_SCALE, EPD_BLACK);
    draw_text_right(fb, VIEW_W - MARK_PAD, 66, "(477,0) TR", NOTE_SCALE, EPD_BLACK);

    /* The bottom pair tucks between the markers -- the rows below are full. */
    paint_text(fb, MARK_PAD + MARK_SIZE + 8, 404, "BL (0,459)", NOTE_SCALE, EPD_BLACK);
    draw_text_right(fb, MARK_RIGHT - 8, 404, "(477,459) BR", NOTE_SCALE, EPD_BLACK);
}

/* Name every edge, so a rotated or mirrored scan is obvious from the text. */
static void draw_edge_labels(uint8_t *fb)
{
    paint_text_centered(fb, CENTER_X, 20, "TOP EDGE", EDGE_SCALE, EPD_BLACK);
    paint_text_centered(fb, CENTER_X, 424, "BOTTOM EDGE", EDGE_SCALE, EPD_BLACK);

    const int left_h = paint_text_stacked_height("LEFT", EDGE_SCALE);
    const int right_h = paint_text_stacked_height("RIGHT", EDGE_SCALE);
    const int glyph_w = PAINT_FONT_W * EDGE_SCALE;

    paint_text_stacked(fb, 14, (VIEW_H - left_h) / 2, "LEFT", EDGE_SCALE, EPD_BLACK);
    paint_text_stacked(fb, VIEW_W - 14 - glyph_w, (VIEW_H - right_h) / 2, "RIGHT",
                       EDGE_SCALE, EPD_BLACK);
}

/*
 * Ticks along the inside of the frame: TICKS_X of them spanning corner to
 * corner across, TICKS_Y down. Spacing is divided out of the box rather than
 * fixed, so the run always starts and ends hard against the frame whatever the
 * box measures -- and unevenness, or a run that stops short, is the row-stride
 * or resolution-register error it looks like.
 */
#define TICKS_X 14
#define TICKS_Y 11

/* Position of tick i of n, spanning the frame's inner edges over `span` px. */
static int tick_at(int i, int n, int span)
{
    return BORDER + i * (span - 2 * BORDER - 2) / (n - 1);
}

static void draw_edge_ticks(uint8_t *fb)
{
    for (int i = 0; i < TICKS_X; i++) {
        int x = tick_at(i, TICKS_X, VIEW_W);
        paint_fill_rect(fb, x, BORDER, 2, 8, EPD_BLACK);
        paint_fill_rect(fb, x, VIEW_H - BORDER - 8, 2, 8, EPD_BLACK);
    }
    for (int i = 0; i < TICKS_Y; i++) {
        int y = tick_at(i, TICKS_Y, VIEW_H);
        paint_fill_rect(fb, BORDER, y, 8, 2, EPD_BLACK);
        paint_fill_rect(fb, VIEW_W - BORDER - 8, y, 8, 2, EPD_BLACK);
    }
}

/* One labelled swatch per colour code, in the order the panel numbers them. */
static void draw_swatches(uint8_t *fb)
{
    static const struct {
        epd_color_t color;
        const char *name;
        const char *code;
    } SWATCHES[] = {
        {EPD_BLACK,  "BLACK",  "0"},
        {EPD_WHITE,  "WHITE",  "1"},
        {EPD_YELLOW, "YELLOW", "2"},
        {EPD_RED,    "RED",    "3"},
    };

    const int count = sizeof(SWATCHES) / sizeof(SWATCHES[0]);
    const int w = 84;
    const int h = 72;
    const int gap = 12;
    const int y = 108;
    const int x0 = (VIEW_W - (count * w + (count - 1) * gap)) / 2;

    for (int i = 0; i < count; i++) {
        int x = x0 + i * (w + gap);

        paint_fill_rect(fb, x, y, w, h, SWATCHES[i].color);
        /* The outline is what makes the white swatch a swatch and not a gap. */
        paint_rect(fb, x, y, w, h, 3, EPD_BLACK);

        paint_text_centered(fb, x + w / 2, y + h + 6, SWATCHES[i].name, NOTE_SCALE, EPD_BLACK);
        paint_text_centered(fb, x + w / 2, y + h + 22, SWATCHES[i].code, NOTE_SCALE, EPD_BLACK);
    }
}

/*
 * A red arrow pointing at the top edge, drawn as a slightly larger black arrow
 * with the red one laid over it -- cheaper than outlining, same result.
 */
static void draw_up_arrow(uint8_t *fb)
{
    const int apex_y = 224;
    const int tail_y = 324;

    /* Black underlay. */
    paint_fill_triangle(fb, CENTER_X, apex_y,
                        CENTER_X - 44, apex_y + 56,
                        CENTER_X + 44, apex_y + 56, EPD_BLACK);
    paint_fill_rect(fb, CENTER_X - 17, apex_y + 56, 34, tail_y - (apex_y + 56), EPD_BLACK);

    /* Red arrow, inset far enough to leave the underlay showing as an outline. */
    paint_fill_triangle(fb, CENTER_X, apex_y + 8,
                        CENTER_X - 36, apex_y + 52,
                        CENTER_X + 36, apex_y + 52, EPD_RED);
    paint_fill_rect(fb, CENTER_X - 13, apex_y + 52, 26, tail_y - 6 - (apex_y + 52), EPD_RED);
}

/*
 * Captions under the arrow rather than beside it: 468 px leaves no room for
 * two text columns flanking the arrow at a legible size.
 */
static void draw_captions(uint8_t *fb)
{
    paint_text_centered(fb, CENTER_X, 334, "ARROW POINTS", EDGE_SCALE, EPD_BLACK);
    paint_text_centered(fb, CENTER_X, 358, "TO THE TOP", EDGE_SCALE, EPD_BLACK);

    /*
     * Panel size, box size and where the box sits, so a glance says which
     * geometry this drawing was built for.
     */
    paint_text_centered(fb, CENTER_X, 384, "648 X 480 PANEL - 478 X 460 AT (0,20)",
                        NOTE_SCALE, EPD_BLACK);
}

void epd_pattern_draw(uint8_t *fb)
{
    /* Clears the dead margins too; everything after this clips to the box. */
    paint_clear(fb, EPD_WHITE);
    paint_rect(fb, 0, 0, VIEW_W, VIEW_H, BORDER, EPD_BLACK);

    draw_edge_ticks(fb);
    draw_corner_markers(fb);
    draw_edge_labels(fb);

    paint_text_centered(fb, CENTER_X, 88, "RETRO CLOCK-RADIO 5.83 IN (G)", NOTE_SCALE,
                        EPD_BLACK);

    draw_swatches(fb);
    draw_up_arrow(fb);
    draw_captions(fb);
}

esp_err_t epd_pattern_show(void)
{
    uint8_t *fb = epd_fb_alloc();
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_NO_MEM, TAG, "no room for a %u byte frame",
                        (unsigned)EPD_FB_SIZE);

    epd_pattern_draw(fb);

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
     * The image stays on the glass once the panel is asleep; the settling delay
     * is what the datasheet asks for before power could be cut.
     */
    ESP_RETURN_ON_ERROR(epd_sleep(), TAG, "sleep");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "test pattern displayed, panel asleep");
    return ESP_OK;
}
