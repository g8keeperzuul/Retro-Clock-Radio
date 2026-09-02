/*
 * epd_pattern.c - Four-colour e-paper bring-up test pattern.
 *
 * The pattern answers three questions at a glance:
 *
 *   1. Do all four inks fire?      Labelled swatches, one per colour code.
 *   2. Which way up is the panel?  Corner markers of four different shapes, an
 *                                  arrow pointing at the top edge, and every
 *                                  edge named. Reading the labels also catches
 *                                  a mirrored scan direction, which a purely
 *                                  symmetric pattern would hide.
 *   3. Is the geometry right?      A frame flush to all four sides plus edge
 *                                  ticks: a row-stride error skews them, an
 *                                  off-by-one in the resolution registers
 *                                  clips them.
 */
#include "epd_pattern.h"

#include "epd_5in83g.h"
#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd-pattern";

#define CENTER_X (EPD_WIDTH / 2)

#define BORDER    4    /* outer frame thickness */
#define MARK_SIZE 56   /* corner marker square */
#define MARK_PAD  14   /* gap from the panel edge to the marker */

#define MARK_RIGHT (EPD_WIDTH - MARK_PAD - MARK_SIZE)   /* 578 */
#define MARK_BOT   (EPD_HEIGHT - MARK_PAD - MARK_SIZE)  /* 410 */

/* Text sizes: 4 for edge names, 3 for the caption columns, 2 for annotations. */
#define EDGE_SCALE 4
#define BODY_SCALE 3
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

    /* Corner names, with the pixel coordinate each marker sits against. */
    paint_text(fb, MARK_PAD, 76, "TL (0,0)", NOTE_SCALE, EPD_BLACK);
    draw_text_right(fb, EPD_WIDTH - MARK_PAD, 76, "(647,0) TR", NOTE_SCALE, EPD_BLACK);
    paint_text(fb, MARK_PAD, 390, "BL (0,479)", NOTE_SCALE, EPD_BLACK);
    draw_text_right(fb, EPD_WIDTH - MARK_PAD, 390, "(647,479) BR", NOTE_SCALE, EPD_BLACK);
}

/* Name every edge, so a rotated or mirrored scan is obvious from the text. */
static void draw_edge_labels(uint8_t *fb)
{
    paint_text_centered(fb, CENTER_X, 26, "TOP EDGE", EDGE_SCALE, EPD_BLACK);
    paint_text_centered(fb, CENTER_X, 414, "BOTTOM EDGE", EDGE_SCALE, EPD_BLACK);

    const int left_h = paint_text_stacked_height("LEFT", EDGE_SCALE);
    const int right_h = paint_text_stacked_height("RIGHT", EDGE_SCALE);
    const int glyph_w = PAINT_FONT_W * EDGE_SCALE;

    paint_text_stacked(fb, 18, (EPD_HEIGHT - left_h) / 2, "LEFT", EDGE_SCALE, EPD_BLACK);
    paint_text_stacked(fb, EPD_WIDTH - 18 - glyph_w, (EPD_HEIGHT - right_h) / 2, "RIGHT",
                       EDGE_SCALE, EPD_BLACK);
}

/*
 * Ticks just inside the frame, every 72 px across and 60 px down. Evenly spaced
 * and reaching both ends means the full 648x480 is being addressed with the
 * expected row stride.
 */
static void draw_edge_ticks(uint8_t *fb)
{
    for (int x = 0; x < EPD_WIDTH; x += 72) {
        paint_fill_rect(fb, x, BORDER, 2, 8, EPD_BLACK);
        paint_fill_rect(fb, x, EPD_HEIGHT - BORDER - 8, 2, 8, EPD_BLACK);
    }
    for (int y = 0; y < EPD_HEIGHT; y += 60) {
        paint_fill_rect(fb, BORDER, y, 8, 2, EPD_BLACK);
        paint_fill_rect(fb, EPD_WIDTH - BORDER - 8, y, 8, 2, EPD_BLACK);
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
    const int w = 120;
    const int h = 96;
    const int gap = 16;
    const int y = 112;
    const int x0 = (EPD_WIDTH - (count * w + (count - 1) * gap)) / 2;

    for (int i = 0; i < count; i++) {
        int x = x0 + i * (w + gap);

        paint_fill_rect(fb, x, y, w, h, SWATCHES[i].color);
        /* The outline is what makes the white swatch a swatch and not a gap. */
        paint_rect(fb, x, y, w, h, 3, EPD_BLACK);

        paint_text_centered(fb, x + w / 2, y + h + 6, SWATCHES[i].name, NOTE_SCALE, EPD_BLACK);
        paint_text_centered(fb, x + w / 2, y + h + 24, SWATCHES[i].code, NOTE_SCALE, EPD_BLACK);
    }
}

/*
 * A red arrow pointing at the top edge, drawn as a slightly larger black arrow
 * with the red one laid over it -- cheaper than outlining, same result.
 */
static void draw_up_arrow(uint8_t *fb)
{
    const int apex_y = 258;
    const int tail_y = 386;

    /* Black underlay. */
    paint_fill_triangle(fb, CENTER_X, apex_y,
                        CENTER_X - 48, apex_y + 64,
                        CENTER_X + 48, apex_y + 64, EPD_BLACK);
    paint_fill_rect(fb, CENTER_X - 19, apex_y + 64, 38, tail_y - (apex_y + 64), EPD_BLACK);

    /* Red arrow, inset far enough to leave the underlay showing as an outline. */
    paint_fill_triangle(fb, CENTER_X, apex_y + 8,
                        CENTER_X - 40, apex_y + 60,
                        CENTER_X + 40, apex_y + 60, EPD_RED);
    paint_fill_rect(fb, CENTER_X - 15, apex_y + 60, 30, tail_y - 6 - (apex_y + 60), EPD_RED);
}

/* Captions flanking the arrow: panel identity on the left, what it means right. */
static void draw_captions(uint8_t *fb)
{
    static const char *LEFT[] = {"5.83 IN (G)", "648 X 480", "4 COLOUR"};
    static const char *RIGHT[] = {"ARROW POINTS", "TO THE TOP", "OF THE PANEL"};

    const int line_h = 30;
    const int y0 = 280;

    for (int i = 0; i < 3; i++) {
        draw_text_right(fb, CENTER_X - 66, y0 + i * line_h, LEFT[i], BODY_SCALE, EPD_BLACK);
        paint_text(fb, CENTER_X + 66, y0 + i * line_h, RIGHT[i], BODY_SCALE, EPD_BLACK);
    }
}

void epd_pattern_draw(uint8_t *fb)
{
    paint_clear(fb, EPD_WHITE);
    paint_rect(fb, 0, 0, EPD_WIDTH, EPD_HEIGHT, BORDER, EPD_BLACK);

    draw_edge_ticks(fb);
    draw_corner_markers(fb);
    draw_edge_labels(fb);
    draw_swatches(fb);
    draw_up_arrow(fb);
    draw_captions(fb);

    paint_text_centered(fb, CENTER_X, 446, "RETRO CLOCK-RADIO DISPLAY TEST", NOTE_SCALE,
                        EPD_BLACK);
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
