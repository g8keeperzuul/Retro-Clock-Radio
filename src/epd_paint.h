/*
 * epd_paint.h - Drawing primitives for the 2-bit-per-pixel e-paper frame buffer.
 *
 * A deliberately small stand-in for Waveshare's GUI_Paint: enough shapes and
 * text to compose a status screen, nothing that needs a file system or a font
 * pack.
 *
 * Coordinates are relative to the usable area, not the panel: (0,0) is its
 * top-left corner, x runs to EPD_USABLE_WIDTH - 1 and y to
 * EPD_USABLE_HEIGHT - 1. The offset into the frame buffer is applied here, and
 * everything clips silently to the box, so nothing can stray into the dead
 * margin along the top and right of the panel.
 */
#pragma once

#include <stdint.h>

#include "epd_5in83g.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in font: a 5x7 cell drawn at 1 px per unit, scaled by an integer. */
#define PAINT_FONT_W 5
#define PAINT_FONT_H 7

/* Advance between character origins, including the one-unit inter-glyph gap. */
#define PAINT_CHAR_ADVANCE(scale) ((PAINT_FONT_W + 1) * (scale))

/* Floods the entire buffer, dead margins included, so no stale ink is shipped. */
void paint_clear(uint8_t *fb, epd_color_t color);
void paint_pixel(uint8_t *fb, int x, int y, epd_color_t color);

/* Axis-aligned rectangle at (x,y), w x h. paint_rect draws the outline only. */
void paint_fill_rect(uint8_t *fb, int x, int y, int w, int h, epd_color_t color);
void paint_rect(uint8_t *fb, int x, int y, int w, int h, int thickness, epd_color_t color);

void paint_fill_circle(uint8_t *fb, int cx, int cy, int r, epd_color_t color);

/*
 * A straight line of the given thickness, at any angle. Built from two
 * triangles, that being the widest thing this layer can fill; the ends are
 * square, so a radial line meets a circle cleanly.
 */
void paint_line(uint8_t *fb, int x0, int y0, int x1, int y1, int thickness,
                epd_color_t color);
void paint_fill_triangle(uint8_t *fb, int x0, int y0, int x1, int y1, int x2, int y2,
                         epd_color_t color);

/*
 * Draw a 1-bit bitmap: one word per row, leftmost pixel in bit 0, up to 32 px
 * wide. Clear bits are left alone, so a glyph can be laid over what is already
 * there -- which is how the numerals get their outline.
 */
void paint_bitmap(uint8_t *fb, int x, int y, const uint32_t *rows, int w, int h,
                  epd_color_t color);

/* Width in pixels the given string occupies, excluding the trailing gap. */
int paint_text_width(const char *text, int scale);

/* Draw text with its top-left corner at (x,y). Unmapped bytes render as blanks. */
void paint_text(uint8_t *fb, int x, int y, const char *text, int scale, epd_color_t color);

/* As paint_text, but centred on cx. */
void paint_text_centered(uint8_t *fb, int cx, int y, const char *text, int scale,
                         epd_color_t color);

/*
 * One character per line, running down the screen -- used for the edge labels,
 * where a rotated font would be overkill.
 */
void paint_text_stacked(uint8_t *fb, int x, int y, const char *text, int scale,
                        epd_color_t color);

/* Height paint_text_stacked() occupies, so callers can centre it vertically. */
int paint_text_stacked_height(const char *text, int scale);

#ifdef __cplusplus
}
#endif
