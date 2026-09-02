/*
 * epd_pattern.h - Four-colour e-paper bring-up test pattern.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compose the test pattern into an existing EPD_FB_SIZE frame buffer. Split out
 * from epd_pattern_show() so the drawing can be rendered and inspected off the
 * target.
 */
void epd_pattern_draw(uint8_t *fb);

/*
 * Bring up the panel, draw the test pattern, refresh, and put the panel back to
 * sleep. Allocates and releases its own frame buffer. Blocks for roughly half a
 * minute while the panel refreshes.
 */
esp_err_t epd_pattern_show(void);

#ifdef __cplusplus
}
#endif
