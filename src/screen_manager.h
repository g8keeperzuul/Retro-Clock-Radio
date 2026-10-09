/*
 * screen_manager.h - Whose turn it is to own the glass.
 *
 * Everything that draws on the panel goes through here, for two reasons. The
 * obvious one is that the screens have to take turns. The less obvious one is
 * that a refresh of this panel blocks for twenty-five seconds and its datasheet
 * asks for a hundred and eighty between them, so "when may anything be drawn at
 * all" is a single global question and belongs in a single place.
 *
 * The screens are a ring, and SCREENS[] in the .c file is the order the knob
 * walks: clockwise moves forward, anticlockwise back, and both wrap. The
 * bring-up test pattern is deliberately not in it -- epd_pattern_show() is still
 * there to be called by hand from app_main() when a panel needs proving.
 *
 * Two rules the knob has to obey, both forced by that twenty-five seconds:
 *
 *   - A turn is not acted on immediately. The ring settles for SCREEN_SETTLE_MS
 *     first, so spinning past three screens costs one refresh rather than three,
 *     and lands on the one the hand stopped at.
 *   - A turn made while the panel is mid-refresh is discarded, not queued.
 *     Queueing it would move the screen half a minute after the knob stopped,
 *     and a knob idly spun through a refresh would leave a run of them waiting.
 */
#pragma once

#include "esp_err.h"
#include "rotary_encoder.h"   /* enc_verdict_t: what a turn came to */

#ifdef __cplusplus
extern "C" {
#endif

/* The ring, in the order the knob walks it. */
typedef enum {
    SCREEN_CLOCK,
    SCREEN_WEATHER,
    SCREEN_FORECAST,
    SCREEN_COUNT,
} screen_id_t;

/*
 * Start the task and put `initial` on the glass. The screens' own tasks may be
 * started before or after this; nothing is drawn until this call is made.
 */
esp_err_t screen_manager_start(screen_id_t initial);

/*
 * A screen announcing that what it would draw has changed. Acted on only if it
 * is the screen showing; otherwise it is remembered, and drawn when the knob
 * next arrives there. Safe to call from any task.
 */
void screen_manager_content_changed(screen_id_t screen);

/*
 * Move `detents` places around the ring: positive forward, negative back.
 * Safe to call from any task. Ignored outright during a refresh.
 *
 * Shaped as a rotary_encoder_cb_t so it can be handed straight to
 * rotary_encoder_start(). The verdict it returns is what lights the knob.
 */
enc_verdict_t screen_manager_step(int detents);

/* Which screen the knob is currently pointing at. */
screen_id_t screen_manager_active(void);

#ifdef __cplusplus
}
#endif
