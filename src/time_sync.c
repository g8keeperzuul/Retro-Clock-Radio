#include "time_sync.h"

#include <inttypes.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef TIME_ZONE
#define TIME_ZONE "UTC0"
#endif
#ifndef NTP_SYNC_TIMEOUT_MS
#define NTP_SYNC_TIMEOUT_MS 30000
#endif
#ifndef NTP_SYNC_INTERVAL_MS
#define NTP_SYNC_INTERVAL_MS (60 * 60 * 1000)
#endif

/*
 * The SNTP client holds CONFIG_LWIP_SNTP_MAX_SERVERS servers; this project is
 * built with one. pool.ntp.org resolves to a rotating set of hosts, so a single
 * slot still gives redundancy across reboots and re-resolutions.
 */
_Static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= 1, "SNTP needs at least one server slot");

/* ---- Module state ------------------------------------------------------- */

static const char *TAG = "time";

/*
 * A freshly booted ESP32 starts at the Unix epoch. Anything at or after this
 * threshold must have come from NTP rather than from the power-on default.
 */
#define TIME_VALID_THRESHOLD 1735689600 /* 2025-01-01T00:00:00Z */

static bool s_started;

/* ---- Helpers ------------------------------------------------------------ */

static void log_current_time(const char *what)
{
    char stamp[48];
    if (time_sync_format_local(stamp, sizeof(stamp)) > 0) {
        ESP_LOGI(TAG, "%s: %s", what, stamp);
    }
}

/* Called from the SNTP task on every successful sync, including re-syncs. */
static void on_time_synced(struct timeval *tv)
{
    ESP_LOGI(TAG, "clock set from %s (epoch %lld)", NTP_SERVER, (long long)tv->tv_sec);
    log_current_time("local time");
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t time_sync_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Timezone first, so the very first sync callback prints local time. */
    setenv("TZ", TIME_ZONE, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    config.sync_cb = on_time_synced;
    /*
     * Step the clock rather than slewing it: at boot the offset is decades, far
     * too large for adjtime() to close in any reasonable time.
     */
    config.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;

    esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
    ESP_LOGI(TAG, "sntp client started, server %s, tz %s, resync every %d s",
             NTP_SERVER, TIME_ZONE, NTP_SYNC_INTERVAL_MS / 1000);
    return ESP_OK;
}

esp_err_t time_sync_wait(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_SYNC_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no response from %s within %d ms (%s); retrying in background",
                 NTP_SERVER, NTP_SYNC_TIMEOUT_MS, esp_err_to_name(err));
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool time_sync_is_valid(void)
{
    return time(NULL) >= TIME_VALID_THRESHOLD;
}

size_t time_sync_format_local(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return strftime(buf, len, "%a %Y-%m-%d %H:%M:%S %Z", &local);
}
