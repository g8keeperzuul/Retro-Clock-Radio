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
#include "settings.h"
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
#ifndef MQTT_DIAGNOSTIC_TOPIC
#error "MQTT_DIAGNOSTIC_TOPIC is not defined. Add -D MQTT_DIAGNOSTIC_TOPIC=\\\"some/topic\\\" to build_flags in platformio.ini"
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
 * The availability topic: "online" while the device is connected, "offline"
 * once it is not.
 *
 * The device publishes only the first of those. The second is a *last will* --
 * handed to the broker at connect time, and published by the broker on the
 * device's behalf when the connection drops. That is the whole point: a radio
 * that has crashed, been unplugged or fallen off the network cannot announce
 * it, so the broker is asked in advance to do it.
 *
 * Both are retained, so a subscriber connecting later is told the current state
 * at once rather than waiting for the next change.
 */
#ifndef WIFI_HOSTNAME
#define WIFI_HOSTNAME "retro-clock-radio"
#endif
#ifndef MQTT_AVAILABILITY_TOPIC
#define MQTT_AVAILABILITY_TOPIC "homeassistant/sensor/" WIFI_HOSTNAME "/availability"
#endif
#ifndef MQTT_ONLINE_PAYLOAD
#define MQTT_ONLINE_PAYLOAD "online"
#endif
#ifndef MQTT_OFFLINE_PAYLOAD
#define MQTT_OFFLINE_PAYLOAD "offline"
#endif

/*
 * How long the broker waits for a sign of life before declaring the will.
 *
 * esp-mqtt defaults to 120 s, which makes for a slow healthcheck: a device that
 * dies the instant after a keepalive goes on reading "online" for two minutes
 * and change. 30 s has the client checking in every 15 and the broker giving up
 * within about 45 -- the cost is a handful of two-byte packets an hour.
 */
#ifndef MQTT_KEEPALIVE_S
#define MQTT_KEEPALIVE_S 30
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
 * Whether the key is broadcast on its own get topic.
 *
 * Follows MQTT_PUBLISH_API_KEY, the same switch that governs the diagnostics
 * payload: with it off, the get topic carries the masked form rather than the
 * key. The get topic is retained, so publishing it whole leaves the key sitting
 * on the broker until something replaces it -- a wider exposure than the panel,
 * and a different one from being able to *set* it, which still works either
 * way. Turn MQTT_PUBLISH_API_KEY on if a round-trip of get and set matters more.
 */

/* homeassistant/<kind>/<host>/<PARAM>/<get|set>, the longest being about 85. */
#define TOPIC_MAX 128

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
static TaskHandle_t s_task;

/* ---- Parameters over MQTT ------------------------------------------------ */

/* homeassistant/<kind>/<host>/<PARAM>/<verb> */
static void param_topic(char *buf, size_t len, setting_id_t id, const char *verb)
{
    snprintf(buf, len, "homeassistant/%s/%s/%s/%s", settings_kind(id),
             WIFI_HOSTNAME, settings_name(id), verb);
}

/*
 * Publish a parameter's current value on its get topic, retained so that a
 * subscriber connecting later is told it without having to ask.
 */
static void publish_param(setting_id_t id)
{
    char topic[TOPIC_MAX];
    param_topic(topic, sizeof(topic), id, "get");

    /*
     * settings_reported(), not settings_get(): for a boot setting these differ
     * for the whole of an uptime, and the get topic is meant to answer "what
     * will this be after a restart", not "what is it now".
     */
    const char *value = settings_reported(id);

#if !MQTT_PUBLISH_API_KEY
    /*
     * The OpenWeather key is masked when the switch is off. The Wi-Fi and
     * broker passwords are not, and deliberately so: they are only ever
     * reported when someone has set them over this same broker, so masking
     * would hide a value from the one party already known to have it.
     */
    char masked[24];
    if (id == SETTING_API_KEY) {
        const size_t n = strlen(value);
        if (n > 8) {
            snprintf(masked, sizeof(masked), "%.4s...%s", value, value + n - 4);
            value = masked;
        }
    }
#endif

    esp_mqtt_client_publish(s_client, topic, value, 0, MQTT_QOS, 1);
}

/* Subscribe to every set topic and announce every current value. */
static void publish_all_params(void)
{
    for (int id = 0; id < SETTING_COUNT; id++) {
        char topic[TOPIC_MAX];

        param_topic(topic, sizeof(topic), (setting_id_t)id, "set");
        esp_mqtt_client_subscribe(s_client, topic, MQTT_QOS);

        publish_param((setting_id_t)id);
    }
}

/*
 * A message on a set topic. The event's topic and data are counted, not
 * NUL-terminated, so both are copied before anything looks at them.
 */
static void on_set_message(const char *topic, int topic_len, const char *data,
                           int data_len)
{
    for (int id = 0; id < SETTING_COUNT; id++) {
        char want[TOPIC_MAX];
        param_topic(want, sizeof(want), (setting_id_t)id, "set");

        if ((int)strlen(want) != topic_len || strncmp(want, topic, topic_len) != 0) {
            continue;
        }

        char value[80];
        if (data_len < 0 || (size_t)data_len >= sizeof(value)) {
            ESP_LOGW(TAG, "%s: ignoring an oversized value", settings_name(id));
            return;
        }
        memcpy(value, data, data_len);
        value[data_len] = '\0';

        /*
         * An empty payload means "go back to the built-in value" -- the natural
         * reading, and also what clearing a retained set topic looks like, so
         * the two agree rather than fighting. settings_set() knows that rule;
         * there is no need for a second copy of it here.
         */
        const esp_err_t err = settings_set((setting_id_t)id, value);

        if (err != ESP_OK) {
            /*
             * Republish regardless. A rejected set leaves the get topic
             * disagreeing with whatever the sender believes, and saying so is
             * the only acknowledgement this protocol has.
             */
            ESP_LOGW(TAG, "%s: rejected", settings_name(id));
        }
        publish_param((setting_id_t)id);
        return;
    }

    ESP_LOGD(TAG, "message on an unrecognised topic");
}

/*
 * Any parameter changing, from here or from anywhere else. The get topic is
 * republished so the broker's copy never lags the device's.
 */
static void on_setting_changed(setting_id_t id)
{
    if (s_connected) {
        publish_param(id);
    }

    /*
     * The publishing task may be most of an interval into a sleep. Wake it so
     * a new interval takes effect now rather than after the old one expires --
     * which, going from an hour to ten seconds, is the difference between
     * "instantly" and "eventually".
     */
    if (id == SETTING_MQTT_INTERVAL && s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static void on_mqtt_event(void *handler_args, esp_event_base_t base, int32_t id,
                          void *event_data)
{
    const esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;

        /*
         * Retract the will the moment there is a connection to retract it
         * with. Published from the event handler rather than the reporting
         * task so that it goes out on every reconnect, and immediately --
         * waiting for the next interval would leave the device reading
         * "offline" for minutes after it had plainly come back.
         */
        esp_mqtt_client_publish(s_client, MQTT_AVAILABILITY_TOPIC,
                                MQTT_ONLINE_PAYLOAD, 0, MQTT_QOS, 1);

        /*
         * Subscribing and announcing belong here rather than in the task for
         * the same reason the will does: a subscription does not survive a
         * reconnect, so it has to be remade every time one happens.
         */
        publish_all_params();

        /* Never log the URI: MQTT_BROKER_URI may carry credentials. */
        ESP_LOGI(TAG, "connected; %s on %s, diagnostics to %s every %ld s",
                 MQTT_ONLINE_PAYLOAD, MQTT_AVAILABILITY_TOPIC, MQTT_DIAGNOSTIC_TOPIC,
                 (long)settings_get_int(SETTING_MQTT_INTERVAL));
        break;

    case MQTT_EVENT_DATA:
        on_set_message(ev->topic, ev->topic_len, ev->data, ev->data_len);
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
                const int id = esp_mqtt_client_publish(s_client, MQTT_DIAGNOSTIC_TOPIC, payload, 0,
                                                       MQTT_QOS, MQTT_RETAIN);
                if (id < 0) {
                    ESP_LOGW(TAG, "publish failed");
                }
                cJSON_free(payload);
            } else {
                ESP_LOGE(TAG, "out of memory building the payload");
            }
        }

        /*
         * Read the interval every pass rather than caching it, and sleep on a
         * notification rather than a plain delay, so that a change over MQTT
         * both takes effect and cuts the current sleep short.
         */
        const int32_t every = settings_get_int(SETTING_MQTT_INTERVAL);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((every > 0 ? every : 1) * 1000));
    }
}

esp_err_t mqtt_report_start(void)
{
    const esp_mqtt_client_config_t cfg = {
        /*
         * The effective values. A set made while this is running is written
         * down but not used -- tearing down the connection a change arrived on
         * in order to apply it is not a sequence worth having.
         */
        .broker.address.uri = settings_get(SETTING_MQTT_BROKER_URI),
        .credentials = {
            .username = (settings_get(SETTING_MQTT_USERNAME)[0] != '\0')
                            ? settings_get(SETTING_MQTT_USERNAME) : NULL,
            .authentication.password = (settings_get(SETTING_MQTT_PASSWORD)[0] != '\0')
                            ? settings_get(SETTING_MQTT_PASSWORD) : NULL,
        },
        .session = {
            /*
             * Given to the broker in the CONNECT packet, so it is in place
             * before anything can go wrong -- including before this client has
             * published a single thing.
             */
            .last_will = {
                .topic = MQTT_AVAILABILITY_TOPIC,
                .msg = MQTT_OFFLINE_PAYLOAD,
                .msg_len = sizeof(MQTT_OFFLINE_PAYLOAD) - 1,
                .qos = MQTT_QOS,
                .retain = 1,
            },
            .keepalive = MQTT_KEEPALIVE_S,
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

    if (xTaskCreate(mqtt_report_task, "mqtt", TASK_STACK, NULL, TASK_PRIO,
                    &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    settings_subscribe(on_setting_changed);

    return ESP_OK;
}

#else /* !MQTT_ENABLED */

esp_err_t mqtt_report_start(void)
{
    ESP_LOGI(TAG, "MQTT_ENABLED is 0: not reporting");
    return ESP_OK;
}

#endif
