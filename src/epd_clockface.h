/*
 * epd_clockface.h - Analog clock face for the e-paper panel, laid out inside
 * the usable EPD_USABLE_WIDTH x EPD_USABLE_HEIGHT box.
 *
 * The dial and, optionally, the hands. epd_clockface_draw() is the dial alone;
 * epd_clockface_draw_hands() lays the hour and minute hands over it.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compose the dial into an existing EPD_FB_SIZE frame buffer. Split out from
 * epd_clockface_show() so the drawing can be rendered and inspected off the
 * target.
 */
void epd_clockface_draw(uint8_t *fb);

/*
 * Lay the hour and minute hands over a dial already drawn into fb. `hour` is
 * 0-23 and `minute` 0-59; the hour hand carries between the numerals as the
 * minutes advance, the way a real movement does.
 */
void epd_clockface_draw_hands(uint8_t *fb, int hour, int minute);

/*
 * Bring up the panel, draw the dial, refresh, and put the panel back to sleep.
 * Allocates and releases its own frame buffer. Blocks for roughly half a minute
 * while the panel refreshes.
 */
esp_err_t epd_clockface_show(void);

/* As epd_clockface_show(), with the hands set to the given time. */
esp_err_t epd_clockface_show_time(int hour, int minute);

#ifdef __cplusplus
}
#endif
