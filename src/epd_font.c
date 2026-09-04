/*
 * epd_font.c - Bitmap text for the e-paper panel. See the header.
 */
#include "epd_font.h"

#include "epd_paint.h"

/* The glyph for `c`, or NULL when the face has no ink for it. */
static const font_glyph_t *glyph_of(const font_face_t *face, char c)
{
    const unsigned char u = (unsigned char)c;

    if (u < face->first || u >= face->first + face->count) {
        return NULL;
    }

    const font_glyph_t *g = &face->glyphs[u - face->first];
    return (g->w == 0 || g->h == 0) ? NULL : g;
}

/* Pen movement for `c`, whether or not it leaves ink. */
static int advance_of(const font_face_t *face, char c)
{
    if (c == ' ') {
        return face->space_advance;
    }

    const font_glyph_t *g = glyph_of(face, c);
    return (g != NULL) ? g->advance : 0;
}

int font_text_width(const font_face_t *face, const char *text)
{
    int width = 0;

    for (const char *p = text; *p != '\0'; p++) {
        width += advance_of(face, *p);
    }
    return width;
}

static void draw_glyph(uint8_t *fb, int pen_x, int baseline, const font_face_t *face,
                       const font_glyph_t *g, epd_color_t color)
{
    const int stride = (g->w + 7) / 8;
    const uint8_t *rows = &face->bits[g->offset];

    for (int row = 0; row < g->h; row++) {
        const uint8_t *bits = &rows[row * stride];
        const int y = baseline + g->by + row;

        for (int col = 0; col < g->w; col++) {
            if (bits[col >> 3] & (0x80 >> (col & 7))) {
                paint_pixel(fb, pen_x + g->bx + col, y, color);
            }
        }
    }
}

void font_draw_text(uint8_t *fb, int x, int y, const font_face_t *face,
                    const char *text, epd_color_t color)
{
    /* Callers give the top of the line; the glyphs need the baseline. */
    const int baseline = y + face->ascent;

    for (const char *p = text; *p != '\0'; p++) {
        const font_glyph_t *g = glyph_of(face, *p);

        if (g != NULL) {
            draw_glyph(fb, x, baseline, face, g, color);
        }
        x += advance_of(face, *p);
    }
}

void font_draw_text_centered(uint8_t *fb, int cx, int y, const font_face_t *face,
                             const char *text, epd_color_t color)
{
    font_draw_text(fb, cx - font_text_width(face, text) / 2, y, face, text, color);
}

void font_draw_text_right(uint8_t *fb, int right, int y, const font_face_t *face,
                          const char *text, epd_color_t color)
{
    font_draw_text(fb, right - font_text_width(face, text), y, face, text, color);
}
