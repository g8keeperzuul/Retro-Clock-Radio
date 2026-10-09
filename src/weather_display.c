/*
 * weather_display.c - Keeps the weather screen current. See the header.
 */
#include "weather_display.h"

#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "epd_forecast.h"
#include "epd_weather.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "screen_manager.h"
#include "time_sync.h"
#include "weather_aggregate.h"
#include "weather_api.h"
#include "weather_daily.h"
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

/* How soon to look again after something went wrong or the clock was unset. */
#define RETRY_INTERVAL_S 60

/*
 * A TLS handshake and a 40 KB parse run on this task; mbedtls alone wants
 * around 5 KB of stack. The drawing does not -- that is the screen manager's.
 */
#define TASK_STACK 8192
#define TASK_PRIO  3

/*
 * The latest good forecast, and the lock that lets the screen manager's task
 * copy it out while this one is writing a new one in. Held only long enough to
 * copy the struct, never across a fetch or a refresh.
 */
static wx_report_t s_report;

/*
 * The same response, reduced a second way. The dial screen wants four parts of
 * today; the two-day screen wants a row of figures for today and tomorrow.
 * Both come out of one fetch, so adding the second screen cost no API calls.
 */
static wx_daysum_t s_days[2];      /* [0] today, [1] tomorrow */

static SemaphoreHandle_t s_report_lock;

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

/* Fetch, parse and reduce -- twice, once for each screen's shape. */
static bool poll_forecast(wx_report_t *out, wx_daysum_t days[2])
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
    const wx_status_t agg = weather_aggregate(fc, &cfg, s_report.valid ? &s_report : NULL,
                                              out);
    if (agg != WX_OK) {
        ESP_LOGE(TAG, "could not reduce the forecast: %s", wx_status_str(agg));
        goto done;
    }

    out->fetched_at = time(NULL);

    /*
     * And the same samples again, by calendar day. Today's window has already
     * been worked out above; tomorrow's comes from asking for the bounds of an
     * hour past midnight, which keeps the one mktime-with-tm_isdst path and so
     * stays right across a clock change.
     */
    memset(days, 0, 2 * sizeof(days[0]));
    weather_day_summary(fc, cfg.day_start, cfg.day_end, &days[0]);

    time_t t_start, t_noon, t_end;
    if (weather_day_bounds(cfg.day_end + 3600, &t_start, &t_noon, &t_end) == WX_OK) {
        weather_day_summary(fc, t_start, t_end, &days[1]);
    }

    ok = true;

done:
    free(fc);
    return ok;
}

esp_err_t weather_display_draw(void)
{
    /*
     * Copy under the lock rather than drawing under it: the refresh takes
     * twenty-five seconds, and holding the poll task off for that long would
     * eventually cost a fetch. A forecast that lands mid-refresh simply marks
     * the screen stale again and is drawn on the next pass.
     */
    wx_report_t snapshot;

    if (s_report_lock == NULL) {
        /* Drawn before the poll task exists: an empty dial, which is the point. */
        memset(&snapshot, 0, sizeof(snapshot));
        return epd_weather_show(&snapshot);
    }

    xSemaphoreTake(s_report_lock, portMAX_DELAY);
    snapshot = s_report;
    xSemaphoreGive(s_report_lock);

    return epd_weather_show(&snapshot);
}

esp_err_t weather_display_draw_forecast(void)
{
    wx_daysum_t snapshot[2];

    if (s_report_lock == NULL) {
        memset(snapshot, 0, sizeof(snapshot));
        return epd_forecast_show(&snapshot[0], &snapshot[1]);
    }

    xSemaphoreTake(s_report_lock, portMAX_DELAY);
    snapshot[0] = s_days[0];
    snapshot[1] = s_days[1];
    xSemaphoreGive(s_report_lock);

    return epd_forecast_show(&snapshot[0], &snapshot[1]);
}

static void weather_display_task(void *arg)
{
    (void)arg;

    while (true) {
        wx_report_t fresh;
        memset(&fresh, 0, sizeof(fresh));

        wx_daysum_t days[2];
        const bool got = poll_forecast(&fresh, days);

        if (got) {
            /*
             * Announce only when the glass would actually differ. At a
             * half-hourly poll most fetches return the same forecast, and the
             * panel is rated for a limited number of refreshes -- see
             * refresh_count.c. Both screens are stored either way, so their
             * timestamps stay current; they are compared separately because a
             * change to one is no reason to redraw the other.
             */
            const bool dial_changed = !wx_report_equal(&fresh, &s_report);
            const bool days_changed = !wx_daysum_equal(&days[0], &s_days[0]) ||
                                      !wx_daysum_equal(&days[1], &s_days[1]);

            xSemaphoreTake(s_report_lock, portMAX_DELAY);
            s_report = fresh;
            s_days[0] = days[0];
            s_days[1] = days[1];
            xSemaphoreGive(s_report_lock);

            if (dial_changed) {
                screen_manager_content_changed(SCREEN_WEATHER);
            }
            if (days_changed) {
                screen_manager_content_changed(SCREEN_FORECAST);
            }
            if (!dial_changed && !days_changed) {
                ESP_LOGI(TAG, "forecast unchanged; leaving the panel alone");
            }
        }

        vTaskDelay(pdMS_TO_TICKS((got ? OWM_POLL_INTERVAL_S : RETRY_INTERVAL_S) * 1000));
    }
}

esp_err_t weather_display_start(void)
{
    install_json_hooks();
    weather_quota_init();

    s_report_lock = xSemaphoreCreateMutex();
    if (s_report_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t ok = xTaskCreate(weather_display_task, "wx-display", TASK_STACK,
                                      NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_report_lock);
        s_report_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "polling every %d s", OWM_POLL_INTERVAL_S);
    return ESP_OK;
}
