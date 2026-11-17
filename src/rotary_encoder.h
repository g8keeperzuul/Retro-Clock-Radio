/*
 * rotary_encoder.h - The Adafruit STEMMA QT rotary encoder breakout.
 *
 * The breakout is a seesaw microcontroller, not a bare encoder: it decodes the
 * quadrature itself and holds a running count, so the host only has to ask what
 * has changed since last time. That is what makes it survivable on this board,
 * where a panel refresh blocks for twenty-five seconds -- clicks made during one
 * are still there afterwards rather than lost to a missed edge.
 *
 * The QT cable carries four wires (SDA, SCL, 3V3, GND), so the breakout's INT
 * output is not connected and this is a polled driver. See README.md "Wiring".
 *
 * The knob is also a push button, wired to one of the seesaw's own GPIOs and
 * read on the same poll as the count.
 *
 * The breakout also carries a NeoPixel under the knob, on the same seesaw and
 * therefore the same I2C handle -- which is why this module owns it. It is used
 * for one thing: telling the hand on the knob what became of the turn it just
 * made. The consumer says which of the three it was and the driver picks the
 * colour, so nothing outside here has to know a NeoPixel exists.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What became of a turn. The knob lights up accordingly -- see FLASH[] in the
 * .c file for which colour is which.
 */
typedef enum {
    ENC_ACCEPTED,   /* it will be acted on now */
    ENC_DEFERRED,   /* accepted, but the panel is not allowed to redraw yet */
    ENC_NOCHANGE,   /* registered, but the dial has not left the screen it is on */
    ENC_IGNORED,    /* dropped: the panel is mid-refresh */
} enc_verdict_t;

/*
 * Called from the encoder's own task with the number of detents turned since
 * the last report: positive clockwise, negative anticlockwise, never zero. It
 * must not block for long -- the next poll is queued behind it -- and it
 * returns what it did with the turn, which the driver flashes on the knob.
 */
typedef enc_verdict_t (*rotary_encoder_cb_t)(int detents);

/*
 * Called from the encoder's own task each time the knob is pushed down -- once
 * per press, on the way down, however long it is held. Returns whether the
 * press did anything: the knob flashes green if it did and stays dark if not,
 * so a press with nothing to do looks exactly as it always has. Same rule as a
 * turn about not blocking for long.
 */
typedef bool (*rotary_encoder_press_cb_t)(void);

/*
 * Probe the breakout and start polling it. `on_press` may be NULL, which
 * leaves the button unread.
 *
 * Returns ESP_ERR_NOT_FOUND when nothing answers at ENC_I2C_ADDR, which the
 * caller should treat as "no knob fitted" rather than as a failure: everything
 * else on this board works without one.
 */
esp_err_t rotary_encoder_start(rotary_encoder_cb_t on_turn, rotary_encoder_press_cb_t on_press);

#ifdef __cplusplus
}
#endif
