/*
 * diagnostics.c - Gathering what this thing is. See the header.
 */
#include "diagnostics.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "refresh_count.h"
#include "settings.h"
#include "wifi_sta.h"

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef TIME_ZONE
#define TIME_ZONE "UTC0"
#endif
#ifndef OWM_LATITUDE
#define OWM_LATITUDE "--"
#endif
#ifndef OWM_LONGITUDE
#define OWM_LONGITUDE "--"
#endif
#ifndef OWM_API_KEY
#define OWM_API_KEY "--"
#endif

/*
 * Whether to report the OpenWeather key in full.
 *
 * Reported whole by default, because a diagnostics report that cannot tell you
 * which key was flashed is not doing its job. Two things worth weighing before
 * leaving it that way, and they are different from each other:
 *
 *   - e-paper holds its last image with no power at all, so the key stays
 *     legible on the glass after the radio is unplugged, and survives into any
 *     photograph of the device;
 *   - MQTT over port 1883 is plaintext on the wire, the broker may retain the
 *     message, and a Home Assistant that ingests it will keep it in its
 *     recorder database and show it in history.
 *
 * Set to 1 to report the first and last four characters only, which is still
 * enough to tell two keys apart. MQTT_PUBLISH_API_KEY=0 drops it from the
 * published payload entirely while leaving it on the panel.
 */
#ifndef STATUS_MASK_API_KEY
#define STATUS_MASK_API_KEY 0
#endif

static void format_api_key(char *out, size_t n)
{
#if STATUS_MASK_API_KEY
    const char *key = settings_get(SETTING_API_KEY);
    const size_t len = strlen(key);

    if (len <= 8) {
        snprintf(out, n, "%s", key);     /* too short to be worth hiding */
    } else {
        snprintf(out, n, "%.4s...%s", key, key + len - 4);
    }
#else
    snprintf(out, n, "%s", settings_get(SETTING_API_KEY));
#endif
}

void diagnostics_gather(diag_t *out)
{
    memset(out, 0, sizeof(*out));

    wifi_sta_format_ssid(out->ssid, sizeof(out->ssid));
    wifi_sta_format_ip(out->ip, sizeof(out->ip));
    wifi_sta_format_mac(out->mac, sizeof(out->mac));
    format_api_key(out->api_key, sizeof(out->api_key));

    /* The live values, not the built-in ones: these can be changed over MQTT. */
    out->timezone = settings_get(SETTING_TIMEZONE);
    out->latitude = settings_get(SETTING_LATITUDE);
    out->longitude = settings_get(SETTING_LONGITUDE);

    out->refreshes = refresh_count_get();
    out->uptime_min = (uint32_t)(esp_timer_get_time() / 60000000);
    out->rssi_dbm = wifi_sta_rssi();
    out->online = wifi_sta_is_connected(); 
}

void diagnostics_format_uptime(const diag_t *d, char *buf, size_t len)
{
    const unsigned days = d->uptime_min / (24 * 60);
    const unsigned hours = (d->uptime_min / 60) % 24;
    const unsigned mins = d->uptime_min % 60;

    /* Leading units are dropped while they are still zero: "47m", then
     * "3h 12m", then "2d 03h 12m". */
    if (days > 0) {
        snprintf(buf, len, "%ud %02uh %02um", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, len, "%uh %02um", hours, mins);
    } else {
        snprintf(buf, len, "%um", mins);
    }
}
