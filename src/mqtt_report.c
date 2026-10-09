/*
 * mqtt_report.c - Publishing the diagnostics. See the header.
 */
#include "mqtt_report.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "diagnostics.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef MQTT_ENABLED
#define MQTT_ENABLED 1
#endif

#if MQTT_ENABLED

#ifndef MQTT_BROKER_URI
#error "MQTT_BROKER_URI is not defined. Add -D MQTT_BROKER_URI=\\\"mqtt://host:1883\\\" to build_flags in platformio.ini"
#endif
#ifndef MQTT_TOPIC
#error "MQTT_TOPIC is not defined. Add -D MQTT_TOPIC=\\\"some/topic\\\" to build_flags in platformio.ini"
#endif

#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef MQTT_PUBLISH_INTERVAL_S
#define MQTT_PUBLISH_INTERVAL_S 300
#endif
#ifndef MQTT_QOS
#define MQTT_QOS 0
#endif
#ifndef MQTT_RETAIN
#define MQTT_RETAIN 0
#endif

/*
 * Whether the OpenWeather key goes out over the wire at all.
 *
 * On by default because the ask was for the same information the status screen
 * carries. It is a different exposure from the panel, though: port 1883 is
 * plaintext, a retained message sits on the broker until something replaces it,
 * and Home Assistant will file it in its recorder database. Set to 0 to drop
 * the field from the payload while leaving it on the glass; set
 * STATUS_MASK_API_KEY=1 to shorten it in both.
 */
#ifndef MQTT_PUBLISH_API_KEY
#define MQTT_PUBLISH_API_KEY 0
#endif

/*
 * Small: the payload is a few hundred bytes and the client does the sending.
 * The JSON is built with cJSON rather than snprintf because an SSID is
 * arbitrary bytes -- one with a quote or a backslash in it would otherwise
 * produce a payload no subscriber could parse.
 */
#define TASK_STACK 4096
#define TASK_PRIO  2

static esp_mqtt_client_handle_t s_client;
static bool s_connected;

static void on_mqtt_event(void *handler_args, esp_event_base_t base, int32_t id,
                          void *event_data)
{
    const esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        /* Never log the URI: MQTT_BROKER_URI may carry credentials. */
        ESP_LOGI(TAG, "connected, publishing to %s every %d s", MQTT_TOPIC,
                 MQTT_PUBLISH_INTERVAL_S);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected; the client will keep retrying");
        break;

    case MQTT_EVENT_ERROR:
        /*
         * Worth distinguishing: a refused connection is a wrong password or a
         * broker that does not want us, and no amount of retrying fixes it.
         */
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "broker refused the connection (code %d) -- check "
                          "MQTT_USERNAME and MQTT_PASSWORD",
                     ev->error_handle->connect_return_code);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error: %s",
                     esp_err_to_name(ev->error_handle->esp_transport_sock_errno));
        }
        break;

    default:
        break;
    }

    (void)handler_args;
    (void)base;
}

/* The payload. Returns a cJSON-allocated string the caller must free. */
static char *build_payload(void)
{
    diag_t d;
    diagnostics_gather(&d);

    char uptime[16];
    diagnostics_format_uptime(&d, uptime, sizeof(uptime));

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "ssid", d.ssid);
    cJSON_AddStringToObject(root, "ip", d.ip);
    cJSON_AddStringToObject(root, "mac", d.mac);
    cJSON_AddNumberToObject(root, "rssi", d.rssi_dbm);
    //cJSON_AddBoolToObject(root, "online", d.online); // no point in publishing if device is connected to network via MQTT

    cJSON_AddStringToObject(root, "timezone", d.timezone);
    cJSON_AddStringToObject(root, "latitude", d.latitude);
    cJSON_AddStringToObject(root, "longitude", d.longitude);
#if MQTT_PUBLISH_API_KEY
    cJSON_AddStringToObject(root, "owm_api_key", d.api_key);
#endif

    cJSON_AddNumberToObject(root, "screen_refreshes", (double)d.refreshes);

    /*
     * Minutes as a number as well as the string the panel shows. A subscriber
     * wants to graph it; a person reading the topic by hand wants to read it.
     */
    cJSON_AddNumberToObject(root, "uptime_minutes", (double)d.uptime_min);
    cJSON_AddStringToObject(root, "uptime", uptime);

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", app->version);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void mqtt_report_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_connected) {
            char *payload = build_payload();

            if (payload != NULL) {
                const int id = esp_mqtt_client_publish(s_client, MQTT_TOPIC, payload, 0,
                                                       MQTT_QOS, MQTT_RETAIN);
                if (id < 0) {
                    ESP_LOGW(TAG, "publish failed");
                }
                cJSON_free(payload);
            } else {
                ESP_LOGE(TAG, "out of memory building the payload");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_INTERVAL_S * 1000));
    }
}

esp_err_t mqtt_report_start(void)
{
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials = {
            .username = (MQTT_USERNAME[0] != '\0') ? MQTT_USERNAME : NULL,
            .authentication.password = (MQTT_PASSWORD[0] != '\0') ? MQTT_PASSWORD : NULL,
        },
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   on_mqtt_event, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(s_client);
    }

    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    if (xTaskCreate(mqtt_report_task, "mqtt", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#else /* !MQTT_ENABLED */

esp_err_t mqtt_report_start(void)
{
    ESP_LOGI(TAG, "MQTT_ENABLED is 0: not reporting");
    return ESP_OK;
}

#endif
