/*
 * epd_font.h - Bitmap text for the e-paper panel.
 *
 * The faces are cut from fonts/Aesthetic Rigelsia Free.ttf by
 * tools/make_font.py and live in the generated epd_font_data.c. Editing them
 * means editing that script and running it again, never touching the table.
 *
 * Each face is one fixed point size. A thin Deco face carries most of its
 * character in strokes two or three pixels wide, and scaling a bitmap that fine
 * destroys exactly the thing worth keeping -- so the sizes are chosen up front
 * and rasterised at the size they are drawn.
 */
#pragma once

#include <stdint.h>

#include "epd_5in83g.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One glyph. `bx`/`by` place the ink box relative to the pen, with `by`
 * measured from the baseline and negative above it -- that is what lets
 * ascenders, descenders and x-height letters share a line.
 */
typedef struct {
    uint8_t  w, h;        /* ink box, 0 x 0 for a character the face omits */
    int8_t   bx, by;      /* ink offset from the pen */
    uint8_t  advance;     /* how far the pen moves afterwards */
    uint16_t offset;      /* start of this glyph's rows in the face's blob */
} font_glyph_t;

/*
 * A face. `glyphs` is indexed by (character - first) over a contiguous range,
 * so characters the face was not built with are present but empty.
 */
typedef struct {
    uint8_t line_height;      /* ascent + descent */
    uint8_t ascent;           /* baseline offset from the top of the line */
    uint8_t space_advance;    /* the space character carries no ink */
    uint8_t first, count;
    const font_glyph_t *glyphs;
    const uint8_t *bits;      /* rows, MSB first, (w + 7) / 8 bytes each */
} font_face_t;

extern const font_face_t FONT_DIAL;    /* digits only, sized for the clock */
extern const font_face_t FONT_TITLE;   /* mixed case, for the weather heading */
extern const font_face_t FONT_LABEL;   /* caps and digits, for everything small */

/*
 * FONT_LABEL with one more pixel of weight. Not a style choice: a four-ink
 * e-paper cannot resolve a one-pixel stem and renders it grey, so a screen set
 * entirely in small type reads washed out unless the type is cut heavier. See
 * README.md "Why the small face has a heavy cut".
 */
extern const font_face_t FONT_LABEL_HEAVY;

/* Width the string occupies, in pixels. */
int font_text_width(const font_face_t *face, const char *text);

/*
 * Draw with the pen at (x, y), where y is the TOP of the line rather than the
 * baseline -- callers are positioning boxes on a panel, not setting type.
 */
void font_draw_text(uint8_t *fb, int x, int y, const font_face_t *face,
                    const char *text, epd_color_t color);

/* As font_draw_text, but centred horizontally on cx. */
void font_draw_text_centered(uint8_t *fb, int cx, int y, const font_face_t *face,
                             const char *text, epd_color_t color);

/* As font_draw_text, but ending at `right` rather than starting at x. */
void font_draw_text_right(uint8_t *fb, int right, int y, const font_face_t *face,
                          const char *text, epd_color_t color);

#ifdef __cplusplus
}
#endif
