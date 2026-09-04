/*
 * weather_display.c - Keeps the weather screen current. See the header.
 */
#include "weather_display.h"

#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "epd_weather.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_sync.h"
#include "weather_aggregate.h"
#include "weather_api.h"
#include "weather_parse25.h"
#include "weather_quota.h"

static const char *TAG = "wx-display";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef OWM_POLL_INTERVAL_S
#define OWM_POLL_INTERVAL_S 1800     /* 48 calls a day */
#endif
#ifndef OWM_UNITS_ENUM
#define OWM_UNITS_ENUM WX_UNITS_IMPERIAL
#endif

/* The panel datasheet asks for at least this long between refreshes. */
#define MIN_REFRESH_INTERVAL_S 180

/* How soon to look again after something went wrong or the clock was unset. */
#define RETRY_INTERVAL_S 60

/*
 * The whole pipeline runs on this task: a TLS handshake, a 40 KB parse, the
 * drawing, and a 25 second blocking SPI refresh. mbedtls alone wants around
 * 5 KB of stack.
 */
#define TASK_STACK 8192
#define TASK_PRIO  3

static wx_report_t s_shown;
static int64_t s_last_refresh_us;

/*
 * cJSON allocates a node per value, each well under the 16 KB threshold that
 * decides internal versus external RAM -- so by default a 40 KB document puts
 * something like 90 KB of small nodes into internal DRAM, alongside the Wi-Fi
 * stack and a live TLS session. Pointing it at PSRAM removes the problem
 * outright. The hooks are process-global and not thread-safe, so this runs
 * once, before the task exists.
 */
static void *json_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return (p != NULL) ? p : malloc(size);
}

static void json_free(void *p)
{
    free(p);
}

static void install_json_hooks(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
}

/* Fetch, parse and reduce. The forecast is a kilobyte, so it lives on the heap. */
static bool poll_forecast(wx_report_t *out)
{
    const esp_err_t reserved = weather_quota_reserve();
    if (reserved != ESP_OK) {
        /* Not an error: the budget doing its job, or the clock not set yet. */
        ESP_LOGI(TAG, "skipping this poll: %s", esp_err_to_name(reserved));
        return false;
    }

    char *body = NULL;
    size_t len = 0;
    if (weather_api_fetch(&body, &len) != ESP_OK) {
        return false;
    }

    wx_forecast_t *fc = calloc(1, sizeof(*fc));
    if (fc == NULL) {
        weather_api_release(body);
        return false;
    }

    bool ok = false;
    const wx_status_t parsed = weather_parse25(body, len, fc);
    weather_api_release(body);

    if (parsed != WX_OK) {
        ESP_LOGE(TAG, "could not read the forecast: %s", wx_status_str(parsed));
        goto done;
    }

    wx_agg_cfg_t cfg;
    weather_agg_default_cfg(&cfg);
    cfg.units = OWM_UNITS_ENUM;
    if (weather_day_bounds(time(NULL), &cfg.day_start, &cfg.noon, &cfg.day_end) != WX_OK) {
        goto done;
    }

    /*
     * The previous report is passed in so windows that have already gone by
     * can be carried forward: the hourly forecast begins at the current hour
     * and holds no history, so by evening there is nothing left to aggregate
     * for the morning.
     */
    const wx_status_t agg = weather_aggregate(fc, &cfg, s_shown.valid ? &s_shown : NULL,
                                              out);
    if (agg != WX_OK) {
        ESP_LOGE(TAG, "could not reduce the forecast: %s", wx_status_str(agg));
        goto done;
    }

    out->fetched_at = time(NULL);
    ok = true;

done:
    free(fc);
    return ok;
}

/* Hold off until the panel's minimum interval has passed. */
static void respect_min_interval(void)
{
    if (s_last_refresh_us == 0) {
        return;
    }

    const int64_t since_s = (esp_timer_get_time() - s_last_refresh_us) / 1000000;
    if (since_s >= MIN_REFRESH_INTERVAL_S) {
        return;
    }

    const int wait_s = (int)(MIN_REFRESH_INTERVAL_S - since_s);
    ESP_LOGI(TAG, "holding %d s to keep %d s between refreshes", wait_s,
             MIN_REFRESH_INTERVAL_S);
    vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
}

static void weather_display_task(void *arg)
{
    (void)arg;

    bool drawn_anything = false;

    while (true) {
        wx_report_t fresh;
        memset(&fresh, 0, sizeof(fresh));

        const bool got = poll_forecast(&fresh);

        /*
         * Redraw only when the glass would actually differ. At a half-hourly
         * poll most fetches return the same forecast, and the panel is rated
         * for a limited number of refreshes -- see refresh_count.c.
         */
        const bool changed = got && !wx_report_equal(&fresh, &s_shown);

        if (changed || !drawn_anything) {
            respect_min_interval();

            const wx_report_t *draw = got ? &fresh : &s_shown;
            const esp_err_t err = epd_weather_show(draw);

            if (err == ESP_OK) {
                s_shown = *draw;
                s_last_refresh_us = esp_timer_get_time();
                drawn_anything = true;
            } else {
                ESP_LOGE(TAG, "could not update the display: %s", esp_err_to_name(err));
            }
        } else if (got) {
            ESP_LOGI(TAG, "forecast unchanged; leaving the panel alone");
            s_shown = fresh;    /* keep the timestamps current */
        }

        const int wait_s = (got || drawn_anything) ? OWM_POLL_INTERVAL_S
                                                   : RETRY_INTERVAL_S;
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
}

esp_err_t weather_display_start(void)
{
    install_json_hooks();
    weather_quota_init();

    const BaseType_t ok = xTaskCreate(weather_display_task, "wx-display", TASK_STACK,
                                      NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "polling every %d s", OWM_POLL_INTERVAL_S);
    return ESP_OK;
}
