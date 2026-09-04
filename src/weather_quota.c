/*
 * weather_quota.c - The call budget for the weather API. See the header.
 */
#include "weather_quota.h"

#include <inttypes.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "time_sync.h"

static const char *TAG = "wx-quota";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef OWM_MAX_CALLS_PER_DAY
#define OWM_MAX_CALLS_PER_DAY 200
#endif
#ifndef OWM_MIN_CALL_INTERVAL_S
#define OWM_MIN_CALL_INTERVAL_S 600
#endif

/* Well clear of the free tier even if every retry path fires at once. */
_Static_assert(OWM_MAX_CALLS_PER_DAY <= 900,
               "OWM_MAX_CALLS_PER_DAY leaves no headroom under the 1000/day free tier");

/* Shares refresh_count.c's namespace; keys must stay under 16 characters. */
#define QUOTA_NAMESPACE "clockradio"
#define KEY_DAY   "wx_day"
#define KEY_CALLS "wx_calls"
#define KEY_LAST  "wx_last"

static nvs_handle_t s_nvs;
static bool s_ready;
static uint32_t s_day;
static uint16_t s_calls;
static int64_t s_last;

/*
 * The UTC day, not the local one. OpenWeather meters against 00:00 UTC, so a
 * local day key in a western timezone rolls over hours late -- calls made in
 * the evening would land in the server's next day while our counter was still
 * on the current one, allowing up to twice the cap inside one of its days.
 */
static uint32_t utc_day_of(time_t now)
{
    return (uint32_t)(now / 86400);
}

esp_err_t weather_quota_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        err = nvs_open(QUOTA_NAMESPACE, NVS_READWRITE, &s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs unavailable (%s) -- the budget cannot be enforced",
                 esp_err_to_name(err));
        return err;
    }

    if (nvs_get_u32(s_nvs, KEY_DAY, &s_day) != ESP_OK) {
        s_day = 0;
    }
    if (nvs_get_u16(s_nvs, KEY_CALLS, &s_calls) != ESP_OK) {
        s_calls = 0;
    }
    if (nvs_get_i64(s_nvs, KEY_LAST, &s_last) != ESP_OK) {
        s_last = 0;
    }

    s_ready = true;
    ESP_LOGI(TAG, "%" PRIu16 " of %d calls used on utc day %" PRIu32,
             s_calls, OWM_MAX_CALLS_PER_DAY, s_day);
    return ESP_OK;
}

esp_err_t weather_quota_reserve(void)
{
    /*
     * Without a real clock time() reads zero, which would put every cold boot
     * on UTC day zero and reset the counter each time -- defeating the whole
     * mechanism. It is a genuine precondition anyway: the forecast cannot be
     * divided into parts of a day without knowing what time it is.
     */
    if (!time_sync_is_valid()) {
        return ESP_ERR_INVALID_STATE;
    }

    const time_t now = time(NULL);
    const uint32_t today = utc_day_of(now);

    /*
     * Only ever roll forward. A clock stepping backwards -- a bad NTP reply, a
     * stale RTC -- must not be able to hand out a fresh allowance.
     */
    if (today > s_day) {
        s_day = today;
        s_calls = 0;
    }

    if (s_last != 0 && now - (time_t)s_last < OWM_MIN_CALL_INTERVAL_S) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_calls >= OWM_MAX_CALLS_PER_DAY) {
        return ESP_ERR_NOT_ALLOWED;
    }

    s_calls++;
    s_last = (int64_t)now;

    if (!s_ready) {
        return ESP_OK;   /* count in RAM at least; init already warned */
    }

    esp_err_t err = nvs_set_u32(s_nvs, KEY_DAY, s_day);
    if (err == ESP_OK) {
        err = nvs_set_u16(s_nvs, KEY_CALLS, s_calls);
    }
    if (err == ESP_OK) {
        err = nvs_set_i64(s_nvs, KEY_LAST, s_last);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        /* Spend it anyway: an uncounted call is worse than an unrecorded one. */
        ESP_LOGW(TAG, "could not persist the budget: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

uint16_t weather_quota_used_today(void)
{
    return s_calls;
}
