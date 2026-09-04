/*
 * weather_aggregate.h - Forty-eight hours of forecast into four parts of a day.
 *
 * This exists because a One Call `daily` entry carries exactly one condition
 * for the whole day; only its temperatures are split into morning, day, evening
 * and night. The four conditions the screen wants have to be worked out from
 * the hourly array.
 *
 * The day boundaries come in as arguments rather than being read from the
 * clock, which is what makes the whole thing testable: a test can pose "the
 * device booted at eight in the evening" or "this is the short day in spring"
 * by passing three different numbers against one captured forecast.
 */
#pragma once

#include "weather_parse.h"
#include "weather_status.h"
#include "weather_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    time_t     day_start, noon, day_end;   /* unix UTC of local midnight, noon, midnight */
    uint8_t    pop_threshold_pct;          /* below this, "light rain" is just cloud */
    uint8_t    strong_pop_pct;             /* at or above this, one hour is enough */
    wx_units_t units;
} wx_agg_cfg_t;

void weather_agg_default_cfg(wx_agg_cfg_t *cfg);

/*
 * Local midnight, noon and the following midnight around `now`.
 *
 * The only function here that consults the timezone. Noon is derived with
 * mktime rather than by adding twelve hours, because a day that springs
 * forward is twenty-three hours long and noon is not its midpoint.
 */
wx_status_t weather_day_bounds(time_t now, time_t *day_start, time_t *noon,
                               time_t *day_end);

/*
 * Reduce `fc` to a report. `prev` is the last report published, or NULL --
 * windows that have already passed have no hourly samples left to aggregate,
 * so an earlier poll from the same day is the best source for them.
 *
 * Pure: no clock, no timezone, no allocation, no logging.
 */
wx_status_t weather_aggregate(const wx_forecast_t *fc, const wx_agg_cfg_t *cfg,
                              const wx_report_t *prev, wx_report_t *out);

#ifdef __cplusplus
}
#endif
