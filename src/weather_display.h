/*
 * weather_display.h - Keeps the weather screen current.
 *
 * A background task polls the forecast, reduces it to four parts of a day and
 * redraws the panel when what would be drawn has actually changed. Polling and
 * refreshing are deliberately separate rates: the forecast is fetched every half
 * hour, but an unchanged forecast costs no panel refresh, and the panel is rated
 * for a finite number of those.
 *
 * The refresh itself belongs to screen_manager.c, which decides whether the
 * weather is the screen showing and when the panel may next be written to.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the task. It draws nothing itself: it fetches, and tells the screen
 * manager when the forecast has actually changed, which redraws only if the
 * weather is the screen showing.
 *
 * Safe to call with the clock unset or the network down: the task waits and
 * retries, and an empty dial goes up in the meantime.
 */
esp_err_t weather_display_start(void);

/*
 * Put the latest forecast on the panel. Blocks for the length of a refresh,
 * about twenty-five seconds. Called by the screen manager, which serialises
 * the panel; nothing else should call it.
 *
 * Two renderings of the same fetch: _draw is the dial, _draw_forecast is the
 * two-day table. One poll feeds both, so the second screen costs no API calls.
 */
esp_err_t weather_display_draw(void);
esp_err_t weather_display_draw_forecast(void);

#ifdef __cplusplus
}
#endif
