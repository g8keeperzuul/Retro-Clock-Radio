/*
 * weather_parse.h - One Call 3.0 JSON into plain arrays.
 *
 * Strictly a transcription: it pulls the fields out and does no interpreting.
 * Deciding what the weather actually is belongs to weather_aggregate.h, which
 * is separated precisely so it can be tested on a hand-built forecast with no
 * JSON in sight.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "weather_status.h"
#include "weather_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The API sends 48 hours and 8 days; the slack absorbs a generous API. */
#define WX_MAX_HOURLY 56
#define WX_MAX_DAILY   8

typedef struct {
    time_t   dt;
    int16_t  temp_x10;
    uint8_t  pop_pct;
    uint8_t  clouds_pct;
    uint16_t cond_id;
} wx_hour_t;

typedef struct {
    time_t   dt;              /* noon, local to the forecast location */
    time_t   sunrise, sunset; /* 0 when absent, as in a polar summer */
    float    moon_phase;
    uint16_t cond_id;
    int16_t  temp_morn_x10, temp_day_x10, temp_eve_x10, temp_night_x10;
    bool     temp_valid;
} wx_day_t;

/* About a kilobyte: heap or a static, never a task stack. */
typedef struct {
    int32_t   tz_offset_s;
    uint8_t   n_hours, n_days;
    wx_hour_t hour[WX_MAX_HOURLY];
    wx_day_t  day[WX_MAX_DAILY];
} wx_forecast_t;

/*
 * Parse `body` into `out`.
 *
 * WX_ERR_BAD_RESPONSE when the JSON is malformed or has neither array;
 * WX_ERR_REJECTED when the body is OpenWeather's own error shape, which is
 * worth telling apart because it means the key or the plan is wrong rather
 * than the network.
 */
wx_status_t weather_parse(const char *body, size_t len, wx_forecast_t *out);

#ifdef __cplusplus
}
#endif
