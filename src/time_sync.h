/*
 * time_sync.h - Set the real-time clock from an NTP server.
 *
 * The NTP server, timezone and timeouts are compile-time parameters supplied by
 * platformio.ini (build_flags). See that file for the full list.
 *
 * Requires a working network connection; call after wifi_sta_connect().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Apply the configured timezone and start the SNTP client against NTP_SERVER.
 * Returns immediately; the first sync arrives asynchronously. Thereafter the
 * client re-syncs every NTP_SYNC_INTERVAL_MS on its own.
 */
esp_err_t time_sync_start(void);

/*
 * Block until the real-time clock has been set, at most NTP_SYNC_TIMEOUT_MS.
 * Returns ESP_OK once the clock is set, ESP_ERR_TIMEOUT otherwise. A timeout is
 * not fatal: the client keeps retrying in the background.
 */
esp_err_t time_sync_wait(void);

/* True once the real-time clock holds a plausible wall-clock time. */
bool time_sync_is_valid(void);

/*
 * Write the current local time into buf as "Mon 2026-08-31 22:49:58 EDT".
 * Returns the number of characters written, or 0 if buf was too small.
 */
size_t time_sync_format_local(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
