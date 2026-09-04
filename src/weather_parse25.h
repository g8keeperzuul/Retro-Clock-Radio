/*
 * weather_parse25.h - The free 2.5 forecast, parsed into the same shape One
 * Call produces so everything downstream is unchanged.
 *
 * This is the source an ordinary OpenWeather key can reach. One Call needs its
 * own paid-tier subscription; weather_parse.h handles that response and stays
 * built and tested for whenever it is available.
 */
#pragma once

#include "weather_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a data/2.5/forecast response. See weather_parse() for the codes. */
wx_status_t weather_parse25(const char *body, size_t len, wx_forecast_t *out);

#ifdef __cplusplus
}
#endif
