/*
 * wifi_sta.h - Wi-Fi station bootstrap for the retro clock-radio.
 *
 * Credentials and tuning knobs are compile-time parameters supplied by
 * platformio.ini (build_flags). See that file for the full list.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up NVS, the TCP/IP stack, the default event loop and the Wi-Fi driver
 * in station mode, then start the driver. Does not associate; call
 * wifi_sta_connect() for that. Safe to call once, early in app_main().
 */
esp_err_t wifi_sta_init(void);

/*
 * Associate with WIFI_SSID and block until DHCP hands us an IPv4 address.
 *
 * Retries with exponential backoff between WIFI_CONNECT_BACKOFF_MIN_MS and
 * WIFI_CONNECT_BACKOFF_MAX_MS, giving up after WIFI_CONNECT_MAX_RETRY attempts
 * (0 = never give up).
 *
 * Returns ESP_OK once an address is assigned, ESP_ERR_TIMEOUT if the attempt
 * budget is exhausted. After the first success, drops are re-dialled
 * automatically in the background.
 */
esp_err_t wifi_sta_connect(void);

/* True while the station holds an IPv4 address. */
bool wifi_sta_is_connected(void);

/* Reason code from the most recent disconnect (wifi_err_reason_t), 0 if none. */
uint8_t wifi_sta_last_disconnect_reason(void);

#ifdef __cplusplus
}
#endif
