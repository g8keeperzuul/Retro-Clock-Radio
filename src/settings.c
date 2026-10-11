/*
 * settings.c - Runtime overrides for a few build flags. See the header.
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

/* ---- Compile-time defaults (platformio.ini -> build_flags) --------------- */

#ifndef OWM_LATITUDE
#define OWM_LATITUDE "0"
#endif
#ifndef OWM_LONGITUDE
#define OWM_LONGITUDE "0"
#endif
#ifndef TIME_ZONE
#define TIME_ZONE "UTC0"
#endif
#ifndef OWM_API_KEY
#define OWM_API_KEY ""
#endif
#ifndef MQTT_PUBLISH_INTERVAL_S
#define MQTT_PUBLISH_INTERVAL_S 300
#endif

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/* Shares refresh_count.c's namespace; NVS keys must stay under 16 characters. */
#define SETTINGS_NAMESPACE "clockradio"

/* The longest a value may be, NUL included. A POSIX TZ string is the big one. */
#define VALUE_MAX 64

#define MAX_OBSERVERS 4

/* ---- Validators ---------------------------------------------------------- */

/*
 * A decimal number within bounds, and nothing else on the line. strtod rather
 * than atof so that "43.9x" and "" are rejected rather than quietly becoming
 * 43.9 and 0 -- this is a value arriving from the network.
 */
static bool valid_decimal(const char *v, double lo, double hi)
{
    char *end = NULL;
    const double d = strtod(v, &end);

    if (end == v || *end != '\0') {
        return false;
    }
    return d >= lo && d <= hi;
}

static bool valid_latitude(const char *v) { return valid_decimal(v, -90.0, 90.0); }
static bool valid_longitude(const char *v) { return valid_decimal(v, -180.0, 180.0); }

/*
 * Between five seconds and a day. The floor is the real limit: below it the
 * publishing task spends more time awake than asleep, and a broker gains
 * nothing from being told the uptime ten times a second.
 */
static bool valid_interval(const char *v)
{
    char *end = NULL;
    const long n = strtol(v, &end, 10);

    if (end == v || *end != '\0') {
        return false;
    }
    return n >= 5 && n <= 86400;
}

/*
 * A POSIX TZ string. Not parsed -- the rules are baroque and newlib is the
 * authority on them -- but it must be printable, free of spaces, and long
 * enough to be more than a typo. A wrong-but-well-formed zone shows up as the
 * wrong time on the panel, which is its own report.
 */
static bool valid_timezone(const char *v)
{
    const size_t len = strlen(v);

    if (len < 3 || len >= VALUE_MAX) {
        return false;
    }
    for (const char *p = v; *p != '\0'; p++) {
        if (*p <= ' ' || *p > '~') {
            return false;
        }
    }
    return true;
}

/*
 * An OpenWeather key is 32 hex characters. Checked loosely rather than exactly,
 * so that a change at their end does not need one here: anything printable, no
 * whitespace, and long enough that a half-pasted key is caught.
 */
static bool valid_api_key(const char *v)
{
    const size_t len = strlen(v);

    if (len < 16 || len >= VALUE_MAX) {
        return false;
    }
    for (const char *p = v; *p != '\0'; p++) {
        if (*p <= ' ' || *p > '~') {
            return false;
        }
    }
    return true;
}

/* ---- The table ----------------------------------------------------------- */

/*
 * One line per parameter. `nvs_key` is separate from `name` because NVS allows
 * fifteen characters and MQTT_PUBLISH_INTERVAL_S is twenty-three; they are not
 * abbreviations of each other and must not be confused.
 */
static const struct {
    const char *name;
    const char *kind;      /* the Home Assistant component the topic names */
    const char *nvs_key;
    const char *fallback;  /* the compile-time value */
    bool (*valid)(const char *);
} TABLE[SETTING_COUNT] = {
    [SETTING_LATITUDE] = {
        "OWM_LATITUDE", "number", "s_lat", OWM_LATITUDE, valid_latitude },
    [SETTING_LONGITUDE] = {
        "OWM_LONGITUDE", "number", "s_lon", OWM_LONGITUDE, valid_longitude },
    [SETTING_TIMEZONE] = {
        "TIME_ZONE", "text", "s_tz", TIME_ZONE, valid_timezone },
    [SETTING_API_KEY] = {
        "OWM_API_KEY", "text", "s_key", OWM_API_KEY, valid_api_key },
    [SETTING_MQTT_INTERVAL] = {
        "MQTT_PUBLISH_INTERVAL_S", "number", "s_mqtt_iv",
        STRINGIFY(MQTT_PUBLISH_INTERVAL_S), valid_interval },
};

static char s_value[SETTING_COUNT][VALUE_MAX];
static bool s_overridden[SETTING_COUNT];
static nvs_handle_t s_nvs;
static bool s_nvs_open;

/*
 * Guards the value table. A mutex rather than a spinlock: a set writes NVS
 * while holding nothing, but readers hand out a pointer into the table and
 * copying under the lock is the only way to keep that honest.
 */
static SemaphoreHandle_t s_lock;

static settings_observer_t s_observers[MAX_OBSERVERS];
static unsigned s_observer_count;

/* ---- Applying ------------------------------------------------------------ */

/*
 * What a parameter does to the running system the moment it changes. Only the
 * timezone acts here; the rest are read at the point of use, which is why
 * nothing has to be told that a latitude has moved except the task that would
 * like to fetch the forecast again.
 */
static void apply(setting_id_t id)
{
    if (id == SETTING_TIMEZONE) {
        setenv("TZ", s_value[SETTING_TIMEZONE], 1);
        tzset();
    }
}

static void notify(setting_id_t id)
{
    for (unsigned i = 0; i < s_observer_count; i++) {
        s_observers[i](id);
    }
}

/* ---- The interface ------------------------------------------------------- */

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    for (int i = 0; i < SETTING_COUNT; i++) {
        snprintf(s_value[i], VALUE_MAX, "%s", TABLE[i].fallback);
    }

    const esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no NVS (%s): every parameter stays on its built-in value",
                 esp_err_to_name(err));
        apply(SETTING_TIMEZONE);
        return;
    }
    s_nvs_open = true;

    for (int i = 0; i < SETTING_COUNT; i++) {
        char stored[VALUE_MAX];
        size_t len = sizeof(stored);

        if (nvs_get_str(s_nvs, TABLE[i].nvs_key, stored, &len) != ESP_OK) {
            continue;
        }

        /*
         * Validate on the way out as well as on the way in. A stored value
         * predates any change to the rules, and one that no longer passes is
         * better ignored than obeyed.
         */
        if (!TABLE[i].valid(stored)) {
            ESP_LOGW(TAG, "stored %s is no longer valid; using the built-in value",
                     TABLE[i].name);
            continue;
        }

        memcpy(s_value[i], stored, len);
        s_overridden[i] = true;
    }

    apply(SETTING_TIMEZONE);

    for (int i = 0; i < SETTING_COUNT; i++) {
        if (s_overridden[i]) {
            /* Never the value: one of these is the API key. */
            ESP_LOGI(TAG, "%s is overridden from NVS", TABLE[i].name);
        }
    }
}

const char *settings_get(setting_id_t id)
{
    if (id >= SETTING_COUNT) {
        return "";
    }
    return s_value[id];
}

int32_t settings_get_int(setting_id_t id)
{
    return (id < SETTING_COUNT) ? (int32_t)strtol(s_value[id], NULL, 10) : 0;
}

esp_err_t settings_set(setting_id_t id, const char *value)
{
    if (id >= SETTING_COUNT || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Trailing whitespace is what an MQTT client's echo looks like, not intent. */
    char clean[VALUE_MAX];
    size_t n = strlen(value);
    while (n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r' ||
                     value[n - 1] == ' ' || value[n - 1] == '\t')) {
        n--;
    }
    if (n >= VALUE_MAX) {
        ESP_LOGW(TAG, "%s: value is too long (%u)", TABLE[id].name, (unsigned)n);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(clean, value, n);
    clean[n] = '\0';

    if (!TABLE[id].valid(clean)) {
        /* Never the value: one of these is the API key. */
        ESP_LOGW(TAG, "%s: rejected a value that did not validate", TABLE[id].name);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool same = (strcmp(s_value[id], clean) == 0);
    if (!same) {
        snprintf(s_value[id], VALUE_MAX, "%s", clean);
    }
    xSemaphoreGive(s_lock);

    if (same) {
        return ESP_OK;      /* still a success; there is just nothing to do */
    }

    if (s_nvs_open) {
        esp_err_t err = nvs_set_str(s_nvs, TABLE[id].nvs_key, clean);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
        if (err != ESP_OK) {
            /*
             * The value is live but will not survive a reboot. Worth saying
             * loudly rather than failing the set, which would be a worse lie.
             */
            ESP_LOGE(TAG, "%s changed but could not be stored: %s", TABLE[id].name,
                     esp_err_to_name(err));
        }
    }

    s_overridden[id] = true;
    apply(id);
    ESP_LOGI(TAG, "%s updated", TABLE[id].name);
    notify(id);

    return ESP_OK;
}

esp_err_t settings_reset(setting_id_t id)
{
    if (id >= SETTING_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_value[id], VALUE_MAX, "%s", TABLE[id].fallback);
    xSemaphoreGive(s_lock);

    if (s_nvs_open) {
        nvs_erase_key(s_nvs, TABLE[id].nvs_key);
        nvs_commit(s_nvs);
    }

    s_overridden[id] = false;
    apply(id);
    ESP_LOGI(TAG, "%s is back on its built-in value", TABLE[id].name);
    notify(id);

    return ESP_OK;
}

bool settings_is_overridden(setting_id_t id)
{
    return (id < SETTING_COUNT) && s_overridden[id];
}

const char *settings_name(setting_id_t id)
{
    return (id < SETTING_COUNT) ? TABLE[id].name : "";
}

const char *settings_kind(setting_id_t id)
{
    return (id < SETTING_COUNT) ? TABLE[id].kind : "";
}

const char *settings_default(setting_id_t id)
{
    return (id < SETTING_COUNT) ? TABLE[id].fallback : "";
}

void settings_subscribe(settings_observer_t observer)
{
    if (s_observer_count >= MAX_OBSERVERS) {
        ESP_LOGE(TAG, "no room for another observer; raise MAX_OBSERVERS");
        return;
    }
    s_observers[s_observer_count++] = observer;
}
