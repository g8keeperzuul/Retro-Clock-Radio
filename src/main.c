/*
 * Retro Clock-Radio - firmware entry point.
 *
 * Hardware (see README.md "Component List"):
 *   - ESP32-S3-DevKitC-1 v1.1 (N16R8: 16 MB flash, 8 MB PSRAM)
 *   - Waveshare 5.83" e-paper (G) display, 648x480, four colour, SPI
 *   - Adafruit I2C STEMMA QT rotary encoder breakout, for choosing a screen
 *
 * This stage bootstraps the microcontroller, joins the Wi-Fi network configured
 * in platformio.ini, sets the real-time clock from NTP, and hands the e-paper
 * panel to the screen manager. Turning the knob walks the ring of screens.
 */
#include <inttypes.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "clock_display.h"
#include "epd_5in83g.h"
#include "epd_clockface.h"
#include "epd_pattern.h"
#include "mqtt_report.h"
#include "refresh_count.h"
#include "settings.h"
#include "refresh_gauge.h"
#include "rotary_encoder.h"
#include "screen_manager.h"
#include "time_sync.h"
#include "weather_display.h"
#include "wifi_sta.h"

static const char *TAG = "main";

/* How often the idle loop reports link health, in milliseconds. */
#define STATUS_INTERVAL_MS 60000

/*
 * Quiet period at the very top of app_main(), long enough to attach a serial
 * monitor before the first log line is emitted. Overridable in platformio.ini.
 */
#ifndef BOOT_LOG_DELAY_MS
#define BOOT_LOG_DELAY_MS 5000
#endif

static void log_boot_banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "%s %s (idf %s, built %s %s)",
             app->project_name, app->version, app->idf_ver, app->date, app->time);
    /* chip.revision is packed as MXX: wafer major * 100 + wafer minor. */
    ESP_LOGI(TAG, "chip: %s rev %u.%u, %u core(s), flash %" PRIu32 " MB, psram %u KB",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
             chip.cores, flash_size / (1024 * 1024), (unsigned)(psram_size / 1024));
    ESP_LOGI(TAG, "reset reason: %d, free heap: %" PRIu32 " bytes",
             (int)esp_reset_reason(), esp_get_free_heap_size());
}

static void log_link_status(void)
{
    char stamp[48];
    if (!time_sync_is_valid() || time_sync_format_local(stamp, sizeof(stamp)) == 0) {
        snprintf(stamp, sizeof(stamp), "clock not set");
    }

    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "%s | offline (last disconnect reason %u), free heap %" PRIu32,
                 stamp, wifi_sta_last_disconnect_reason(), esp_get_free_heap_size());
        return;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "%s | online: rssi %d dBm, channel %u, free heap %" PRIu32,
                 stamp, ap.rssi, ap.primary, esp_get_free_heap_size());
    } else {
        ESP_LOGI(TAG, "%s | online, free heap %" PRIu32, stamp, esp_get_free_heap_size());
    }
}

void app_main(void)
{
    /* Nothing may log before this delay elapses -- that is the whole point. */
    vTaskDelay(pdMS_TO_TICKS(BOOT_LOG_DELAY_MS));

    log_boot_banner();

    /*
     * The refresh counter goes first, so it can hook the panel driver before
     * anything has a chance to refresh. Losing it is not fatal; it says so.
     */
    refresh_count_init();

    /*
     * Settings next, and before everything else: the timezone is one of them,
     * and three more go into the forecast URL. Anything started ahead of this
     * would be running on the flashed value rather than the stored one.
     */
    settings_init();

    /*
     * Panel bring-up next: it does not need the network, and a wrong pin map or
     * a loose ribbon shows up here rather than behind a Wi-Fi timeout. Not
     * fatal either -- the clock still keeps time without a screen.
     *
     * Nothing is drawn yet. The display task owns the glass from here, and
     * putting a handless dial up now would only burn a refresh that the first
     * scheduled one repeats half a minute later. For the bring-up test pattern
     * instead, call epd_pattern_show() here and skip clock_display_start().
     */
    esp_err_t epd_err = epd_init();
    if (epd_err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper display unavailable: %s", esp_err_to_name(epd_err));
    }

    ESP_ERROR_CHECK(wifi_sta_init());

    if (wifi_sta_connect() == ESP_OK) {
        /*
        * Set the real-time clock from NTP. A timeout here is not fatal -- the SNTP
        * client keeps retrying in the background, and log_link_status() reports
        * whether the clock has been set yet.
        */
        ESP_ERROR_CHECK(time_sync_start());
        if (time_sync_wait() != ESP_OK) {
            ESP_LOGW(TAG, "starting up with an unset clock");
        }

        ESP_LOGI(TAG, "network joined and clock synced");

        /*
         * Diagnostics over MQTT. Started only with a network, and not fatal
         * without a broker: esp-mqtt reconnects on its own, and a radio whose
         * broker is down is still a radio.
         */
        const esp_err_t mqtt_err = mqtt_report_start();
        if (mqtt_err != ESP_OK) {
            ESP_LOGW(TAG, "MQTT reporting unavailable: %s", esp_err_to_name(mqtt_err));
        }
    } else {
        /*
         * Only reachable when WIFI_CONNECT_MAX_RETRY is non-zero. 
         */
        ESP_LOGW(TAG, "could not join the network");
        
        if (WIFI_CONNECT_RESTART_ON_FAIL) {
            ESP_LOGE(TAG, "restarting");
            esp_restart();
        }
        /* else treat lack of wifi connection as non-fatal */
    }

    /*
     * The content tasks first, then the manager that arbitrates between them.
     * Neither draws anything on its own; both are started whether or not the
     * network came up, since each has something to put up while it waits.
     */
    if (epd_err == ESP_OK) {
        ESP_ERROR_CHECK(clock_display_start());
        ESP_ERROR_CHECK(weather_display_start());

        /*
         * The countdown strip before the manager that drives it. Not fatal:
         * without it a deferred screen change simply happens in silence, which
         * is what the firmware did until now.
         */
        const esp_err_t gauge_err = refresh_gauge_start();
        if (gauge_err != ESP_OK) {
            ESP_LOGW(TAG, "no countdown strip: %s", esp_err_to_name(gauge_err));
        }

        ESP_ERROR_CHECK(screen_manager_start(SCREEN_CLOCK));

        /*
         * No knob fitted is not a fault: the screens still change themselves,
         * there is just no way to choose between them.
         */
        const esp_err_t enc_err = rotary_encoder_start(screen_manager_step);
        if (enc_err != ESP_OK) {
            ESP_LOGW(TAG, "no rotary encoder: %s -- screens cannot be switched",
                     esp_err_to_name(enc_err));
        }
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        log_link_status();
    }
}
