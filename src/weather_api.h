/*
 * weather_api.h - Fetching the One Call response.
 *
 * The caller owns the body and frees it with weather_api_release(). Nothing
 * here knows what JSON is; that is weather_parse.h's job.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fetch the forecast. On ESP_OK *body is a NUL-terminated response of *len
 * bytes, allocated from PSRAM where there is any.
 *
 * With WEATHER_MOCK set this returns a canned payload and touches neither the
 * network nor the call budget.
 */
esp_err_t weather_api_fetch(char **body, size_t *len);

void weather_api_release(char *body);

#ifdef __cplusplus
}
#endif
