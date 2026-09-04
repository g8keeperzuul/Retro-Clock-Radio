/*
 * weather_quota.h - The call budget for the weather API, kept in NVS.
 *
 * The free One Call tier allows 1000 calls a day. A half-hourly poll needs 48,
 * so the cap is not the thing to worry about -- a crash loop is. Each restart
 * would otherwise make a call, and a board stuck rebooting would spend the
 * day's allowance in minutes. Hence three brakes rather than one, all of them
 * persistent, because anything held only in RAM resets with the fault.
 */
#pragma once

#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t weather_quota_init(void);

/*
 * Spend one call. Call this immediately before opening the socket, never
 * after: a crash inside the handshake must still cost budget, or a crash loop
 * is unbounded. There is deliberately no refund on failure, since a request
 * that failed locally may well have reached the server.
 *
 *   ESP_OK                 go ahead
 *   ESP_ERR_INVALID_STATE  the clock is unset, so there is no safe day to count against
 *   ESP_ERR_TIMEOUT        too soon after the last attempt
 *   ESP_ERR_NOT_ALLOWED    today's allowance is spent
 */
esp_err_t weather_quota_reserve(void);

uint16_t weather_quota_used_today(void);

#ifdef __cplusplus
}
#endif
