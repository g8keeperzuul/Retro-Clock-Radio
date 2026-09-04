/*
 * weather_display.h - Keeps the weather screen current.
 *
 * A background task polls the forecast, reduces it to four parts of a day and
 * redraws the panel when what would be drawn has actually changed. Polling and
 * refreshing are deliberately separate rates: the forecast is fetched every half
 * hour, but an unchanged forecast costs no panel refresh, and the panel is rated
 * for a finite number of those.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the task. Returns once it is running, not once anything is on the
 * glass -- the first refresh takes the better part of half a minute.
 *
 * Safe to call with the clock unset or the network down: the task waits and
 * retries, and puts up an empty dial in the meantime.
 */
esp_err_t weather_display_start(void);

#ifdef __cplusplus
}
#endif
