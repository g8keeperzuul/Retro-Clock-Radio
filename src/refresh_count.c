/*
 * refresh_count.c - Lifetime count of panel refreshes, held in NVS. See header.
 */
#include "refresh_count.h"

#include <inttypes.h>
#include <stdbool.h>

#include "epd_5in83g.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "refresh-count";

/*
 * Namespace and key. Both are part of the on-flash format: renaming either one
 * abandons the stored count and starts again from zero.
 */
#define COUNT_NAMESPACE "clockradio"
#define COUNT_KEY       "epd_refresh"

static nvs_handle_t s_nvs;
static uint32_t s_count;
static bool s_ready;

/*
 * One write per refresh. At the display's five-minute cadence that is 288 a
 * day, which NVS wear levelling absorbs without trouble -- and the panel itself
 * gives out long before the flash does.
 */
static void on_refresh(void)
{
    s_count++;

    if (!s_ready) {
        return;
    }

    esp_err_t err = nvs_set_u32(s_nvs, COUNT_KEY, s_count);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not store count %" PRIu32 ": %s", s_count,
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "panel refresh %" PRIu32, s_count);
}

esp_err_t refresh_count_init(void)
{
    /*
     * Deliberately no nvs_flash_erase() on failure, unlike the Wi-Fi
     * bootstrap: erasing to recover would throw away the very thing being
     * counted. If NVS is unusable, say so and carry on uncounted.
     */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        err = nvs_open(COUNT_NAMESPACE, NVS_READWRITE, &s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs unavailable (%s) -- refreshes will not be counted",
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_get_u32(s_nvs, COUNT_KEY, &s_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_count = 0;   /* first run on this panel */
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not read stored count: %s", esp_err_to_name(err));
        nvs_close(s_nvs);
        return err;
    }

    s_ready = true;
    epd_set_refresh_hook(on_refresh);

    ESP_LOGI(TAG, "panel has been refreshed %" PRIu32 " times", s_count);
    return ESP_OK;
}

uint32_t refresh_count_get(void)
{
    return s_count;
}
