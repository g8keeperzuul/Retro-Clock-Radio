/*
 * refresh_gauge.h - A six-pixel countdown to the next panel refresh.
 *
 * The panel may not be refreshed more often than every 180 seconds, so a screen
 * change asked for by the knob can sit waiting for up to three minutes with
 * nothing on the glass to say so. The knob's own pixel flashes amber at the
 * moment of the turn, which says *that* you are waiting; this bar says *how
 * much longer*. It fills when the wait starts and empties as it runs down.
 *
 * A strip of WS2812s on one GPIO, driven by RMT. Six pixels is eighteen bytes,
 * which is small enough that hand-rolling it costs less than the managed
 * led_strip component would -- this project builds only from framework-espidf.
 *
 * NOT thread-safe, deliberately: every call comes from the screen manager's
 * task, which is the only thing that knows when a refresh is due.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pixels on the strip. Public because the caller has to divide a duration by it
 * to work out how many to light.
 */
#ifndef GAUGE_LED_COUNT
#define GAUGE_LED_COUNT 6
#endif

/*
 * Claim the RMT channel and blank the strip.
 *
 * A failure here is worth reporting but not worth stopping for -- there is no
 * way to tell a missing strip from a present one, so this only ever fails on a
 * bad configuration. After one, show() and clear() do nothing.
 */
esp_err_t refresh_gauge_start(void);

/*
 * Light the first `lit` pixels in one colour and darken the rest. `lit` is
 * clamped to the strip. The colour components are absolute 0..255 -- scaling
 * for brightness is the caller's business, since the caller is the one that
 * knows what the colour means.
 */
void refresh_gauge_show(uint8_t r, uint8_t g, uint8_t b, int lit);

/* Darken the whole strip. */
void refresh_gauge_clear(void);

#ifdef __cplusplus
}
#endif
