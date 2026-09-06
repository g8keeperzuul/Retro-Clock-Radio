/*
 * clock_display.h - Keeps the dial showing the time, five minutes at a time.
 *
 * A background task notices whenever the displayed five-minute mark changes and
 * asks screen_manager.c to redraw, which it does only if the clock face is the
 * screen showing. Five minutes is not a stylistic choice: a full refresh of this panel
 * takes about 25 seconds and its datasheet asks for at least 180 seconds
 * between updates, so a moving minute hand is not on offer. See README.md.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The hands move in steps this many minutes apart. */
#define CLOCK_DISPLAY_STEP_MINUTES 5

/*
 * Start the task. It draws nothing itself: it watches the clock and tells the
 * screen manager when the hands would move, which redraws only if the clock
 * face is the screen showing.
 *
 * Safe to call with the real-time clock unset: the bare dial goes up, and the
 * hands appear once NTP lands.
 */
esp_err_t clock_display_start(void);

/*
 * Put the dial on the panel, hands at the nearest five-minute mark to now.
 * Blocks for the length of a refresh, about twenty-five seconds. Called by the
 * screen manager, which serialises the panel; nothing else should call it.
 */
esp_err_t clock_display_draw(void);

#ifdef __cplusplus
}
#endif
