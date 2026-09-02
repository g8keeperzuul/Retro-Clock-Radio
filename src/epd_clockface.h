/*
 * epd_clockface.h - Analog clock face for the e-paper panel, laid out inside
 * the usable EPD_USABLE_WIDTH x EPD_USABLE_HEIGHT box.
 *
 * The dial only: numerals, chapter ring, minute track and the SLEEP marking.
 * No hands -- those belong to whatever draws the current time on top.
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
 * Bring up the panel, draw the dial, refresh, and put the panel back to sleep.
 * Allocates and releases its own frame buffer. Blocks for roughly half a minute
 * while the panel refreshes.
 */
esp_err_t epd_clockface_show(void);

#ifdef __cplusplus
}
#endif
