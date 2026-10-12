#include "wifi_sta.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "settings.h"

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef WIFI_SSID
#error "WIFI_SSID is not defined. Add -D WIFI_SSID=\\\"your-ssid\\\" to build_flags in platformio.ini"
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD is not defined. Add -D WIFI_PASSWORD=\\\"your-password\\\" to build_flags in platformio.ini"
#endif
#ifndef WIFI_HOSTNAME
#define WIFI_HOSTNAME "retro-clock-radio"
#endif
#ifndef WIFI_AUTH_THRESHOLD
#define WIFI_AUTH_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif
#ifndef WIFI_CONNECT_MAX_RETRY
#define WIFI_CONNECT_MAX_RETRY 0 /* 0 = retry forever */
#endif
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000
#endif
#ifndef WIFI_CONNECT_BACKOFF_MIN_MS
#define WIFI_CONNECT_BACKOFF_MIN_MS 1000
#endif
#ifndef WIFI_CONNECT_BACKOFF_MAX_MS
#define WIFI_CONNECT_BACKOFF_MAX_MS 30000
#endif

/* sizeof() includes the NUL; the driver's fields are 32 and 64 bytes + NUL. */
_Static_assert(sizeof(WIFI_SSID) <= 33, "WIFI_SSID is longer than 32 characters");
_Static_assert(sizeof(WIFI_PASSWORD) <= 65, "WIFI_PASSWORD is longer than 64 characters");

/* ---- Module state ------------------------------------------------------- */

static const char *TAG = "wifi";

#define WIFI_EVT_GOT_IP     BIT0
#define WIFI_EVT_DISCONNECT BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

/* Once the first association succeeds, the event handler re-dials on its own. */
static volatile bool s_auto_reconnect;
static volatile bool s_connected;
static volatile uint8_t s_last_reason;

/* ---- Event handling ----------------------------------------------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "station started, hostname \"%s\"", WIFI_HOSTNAME);
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *ev = data;
        ESP_LOGI(TAG, "associated with \"%.*s\" bssid " MACSTR " channel %u",
                 ev->ssid_len, (const char *)ev->ssid, MAC2STR(ev->bssid), ev->channel);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        s_last_reason = ev->reason;
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from \"%s\" (reason %u)",
                 settings_get(SETTING_WIFI_SSID), ev->reason);
        xEventGroupClearBits(s_events, WIFI_EVT_GOT_IP);
        xEventGroupSetBits(s_events, WIFI_EVT_DISCONNECT);
        if (s_auto_reconnect) {
            /* Don't block the event task: the driver paces its own retries. */
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "reconnect failed: %s", esp_err_to_name(err));
            }
        }
        break;
    }

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case IP_EVENT_STA_GOT_IP: {
        const ip_event_got_ip_t *ev = data;
        s_connected = true;
        ESP_LOGI(TAG, "ip " IPSTR " netmask " IPSTR " gateway " IPSTR "%s",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
                 IP2STR(&ev->ip_info.gw), ev->ip_changed ? " (changed)" : "");
        xEventGroupClearBits(s_events, WIFI_EVT_DISCONNECT);
        xEventGroupSetBits(s_events, WIFI_EVT_GOT_IP);
        break;
    }

    case IP_EVENT_STA_LOST_IP:
        s_connected = false;
        ESP_LOGW(TAG, "lost ip address");
        xEventGroupClearBits(s_events, WIFI_EVT_GOT_IP);
        break;

    default:
        break;
    }
}

/* ---- Bring-up ----------------------------------------------------------- */

static esp_err_t nvs_bootstrap(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wifi_sta_init(void)
{
    if (s_events != NULL) {
        return ESP_ERR_INVALID_STATE; /* already initialised */
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(nvs_bootstrap());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, WIFI_HOSTNAME));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       &on_wifi_event, NULL, &s_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                       &on_ip_event, NULL, &s_ip_handler));

    /*
     * Keep credentials in RAM only: the compile-time SSID/password is the single
     * source of truth, so a stale copy in NVS can never win after a rebuild.
     */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_THRESHOLD,
            /* Scan every channel and pick the strongest BSS for this SSID. */
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            /* Advertise 802.11w so WPA3/WPA2-PMF access points accept us. */
            .pmf_cfg = { .capable = true, .required = false },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    /* strncpy over fixed-size, non-NUL-terminated driver fields. */
    /*
     * The effective credentials, which are the stored ones when a set has been
     * made and the radio restarted since, and the built-in ones otherwise.
     */
    strncpy((char *)sta_cfg.sta.ssid, settings_get(SETTING_WIFI_SSID),
            sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, settings_get(SETTING_WIFI_PASSWORD),
            sizeof(sta_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t mac[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "station mac " MACSTR, MAC2STR(mac));
    }
    return ESP_OK;
}

esp_err_t wifi_sta_connect(void)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t max_attempts = WIFI_CONNECT_MAX_RETRY;
    uint32_t backoff_ms = WIFI_CONNECT_BACKOFF_MIN_MS;

    for (uint32_t attempt = 1; max_attempts == 0 || attempt <= max_attempts; attempt++) {
        ESP_LOGI(TAG, "connecting to \"%s\" (attempt %" PRIu32 ")",
                 settings_get(SETTING_WIFI_SSID), attempt);
        xEventGroupClearBits(s_events, WIFI_EVT_GOT_IP | WIFI_EVT_DISCONNECT);

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }

        EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_EVT_GOT_IP | WIFI_EVT_DISCONNECT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_EVT_GOT_IP) {
            s_auto_reconnect = true; /* hand recovery to the event handler */
            return ESP_OK;
        }
        if (bits == 0) {
            ESP_LOGW(TAG, "attempt timed out after %d ms", WIFI_CONNECT_TIMEOUT_MS);
        }

        /* Make sure the driver is idle before the next dial. */
        esp_wifi_disconnect();
        ESP_LOGI(TAG, "retrying in %" PRIu32 " ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > WIFI_CONNECT_BACKOFF_MAX_MS) {
            backoff_ms = WIFI_CONNECT_BACKOFF_MAX_MS;
        }
    }

    ESP_LOGE(TAG, "giving up on \"%s\" after %" PRIu32 " attempts",
             settings_get(SETTING_WIFI_SSID), max_attempts);
    return ESP_ERR_TIMEOUT;
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}

uint8_t wifi_sta_last_disconnect_reason(void)
{
    return s_last_reason;
}

/* ---- What the station is, for the diagnostics screen -------------------- */

/* "--" for a field with nothing in it, so a caller always has something to draw. */
static size_t nothing(char *buf, size_t len)
{
    if (len > 0) {
        snprintf(buf, len, "--");
    }
    return 0;
}

size_t wifi_sta_format_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;

    if (s_sta_netif == NULL || !s_connected ||
        esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return nothing(buf, len);
    }

    return (size_t)snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

size_t wifi_sta_format_mac(char *buf, size_t len)
{
    uint8_t mac[6];

    /*
     * Read from efuse rather than from the interface: it is the same address,
     * and it is there to be printed before the radio has ever come up.
     */
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return nothing(buf, len);
    }

    return (size_t)snprintf(buf, len, MACSTR, MAC2STR(mac));
}

size_t wifi_sta_format_ssid(char *buf, size_t len)
{
    wifi_ap_record_t ap;

    /*
     * The network actually joined, not the one asked for. On a screen whose
     * job is to answer "what is this thing connected to", the configured value
     * is the one answer that cannot be trusted.
     */
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || ap.ssid[0] == '\0') {
        return nothing(buf, len);
    }

    return (size_t)snprintf(buf, len, "%.32s", (const char *)ap.ssid);
}

int wifi_sta_rssi(void)
{
    wifi_ap_record_t ap;

    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}
