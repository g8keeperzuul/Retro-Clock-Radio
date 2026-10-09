/*
 * weather_daily.h - Three-hourly samples reduced to a day apiece.
 *
 * The two-day forecast screen wants one row of figures per calendar day. The
 * paid daily endpoint would hand those over ready-made; this derives the same
 * numbers from the free three-hourly forecast, which carries every field the
 * screen shows -- temp_min/max, humidity, pop, wind.gust, clouds.all and
 * rain/snow -- just eight times a day instead of once.
 *
 * Pure: no JSON, no clock, no ESP. The day boundaries come in as arguments, so
 * a test poses "booted at eight in the evening" by passing different numbers.
 *
 * One honest limitation, and it is the endpoint's rather than this code's: the
 * forecast begins at the current hour and holds no history, so TODAY narrows
 * as the day goes by. By late evening it describes the hours that are left,
 * and `samples` says how many it was able to use.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "weather_parse.h"
#include "weather_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One day's worth of the two-day screen. */
typedef struct {
    time_t   day_start;      /* local midnight of the day described */
    uint8_t  samples;        /* three-hourly entries this was reduced from */

    uint16_t cond_id;        /* the day's representative OpenWeather id */
    int16_t  temp_min_x10, temp_max_x10;
    uint8_t  humidity_pct;   /* mean over the day */
    uint8_t  pop_pct;        /* the worst hour, not the mean */
    uint16_t gust_x10;       /* the strongest gust, in m/s as the API sends it */
    uint8_t  clouds_pct;     /* mean over the day */
    uint16_t precip_x100;    /* total millimetres over the day */

    bool     valid;
} wx_daysum_t;

/*
 * Reduce every sample falling in [day_start, day_end) into `out`.
 *
 * WX_ERR_NO_DATA when the window caught no samples at all, which is ordinary
 * rather than exceptional -- it is what "today" looks like just before
 * midnight. `out` is zeroed and left invalid in that case.
 */
wx_status_t weather_day_summary(const wx_forecast_t *fc, time_t day_start,
                                time_t day_end, wx_daysum_t *out);

/* Whether two summaries would put the same pixels on the panel. */
bool wx_daysum_equal(const wx_daysum_t *a, const wx_daysum_t *b);

/*
 * OpenWeather's own wording for a condition id -- "broken clouds", in capitals
 * because that is the only case the panel's small face was cut with.
 *
 * A table rather than the description string from the response: the screen
 * shows one condition per day but a day is eight samples, so the string would
 * have to be carried on every one of them to be sure of having the right one.
 * The ids are a fixed, documented list, so the lookup cannot drift.
 */
const char *wx_cond_description(uint16_t cond_id);

#ifdef __cplusplus
}
#endif
