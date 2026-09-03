/*
 * clock_display.h - Keeps the dial showing the time, five minutes at a time.
 *
 * A background task redraws the hands whenever the displayed five-minute mark
 * changes. Five minutes is not a stylistic choice: a full refresh of this panel
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
 * Start the task. Returns once it is running, not once anything is on the
 * glass -- the first refresh takes the better part of half a minute.
 *
 * Safe to call with the real-time clock unset: the bare dial goes up, and the
 * hands appear once NTP lands.
 */
esp_err_t clock_display_start(void);

#ifdef __cplusplus
}
#endif
