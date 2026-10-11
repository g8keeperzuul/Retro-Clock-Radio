/*
 * settings.h - The handful of parameters that can be changed without a reflash.
 *
 * Each one has a compile-time default in platformio.ini and may be overridden
 * at runtime over MQTT (see mqtt_report.c). An override is written to NVS, so
 * it outlives a reboot; flashing new firmware does not clear it, which is the
 * point but is also worth knowing when a build flag appears to have no effect.
 * settings_reset() puts a parameter back on its built-in value.
 *
 * Everything is stored and handed out as a string, including the numbers:
 * latitude and longitude are only ever pasted into a URL, the MQTT payloads
 * are text, and keeping one representation means one place to get it wrong.
 * settings_get_int() is there for the one caller that wants arithmetic.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SETTING_LATITUDE,
    SETTING_LONGITUDE,
    SETTING_TIMEZONE,
    SETTING_API_KEY,
    SETTING_MQTT_INTERVAL,
    SETTING_COUNT,
} setting_id_t;

/*
 * Load the overrides out of NVS and apply them. Call early -- before the clock
 * is started, because the timezone is one of these, and before the first
 * forecast, because three of the others go into its URL.
 *
 * Never fatal: a missing or unreadable NVS leaves every parameter on its
 * built-in value, which is exactly what the firmware did before this existed.
 */
void settings_init(void);

/* The current value. Never NULL, never empty, safe to read from any task. */
const char *settings_get(setting_id_t id);

/* The current value as a whole number. Zero for a parameter that is not one. */
int32_t settings_get_int(setting_id_t id);

/*
 * Validate, store and apply. Returns ESP_ERR_INVALID_ARG when the value does
 * not pass, leaving the old one in place, and ESP_OK when it took -- including
 * when it was already the value, which is not an error worth distinguishing.
 */
esp_err_t settings_set(setting_id_t id, const char *value);

/* Drop the override and go back to the compile-time default. */
esp_err_t settings_reset(setting_id_t id);

/* Whether this parameter is currently overriding its built-in value. */
bool settings_is_overridden(setting_id_t id);

/* "OWM_LATITUDE" -- the name used in the MQTT topic and on the status screen. */
const char *settings_name(setting_id_t id);

/* "number", "text" or "switch" -- the Home Assistant component in the topic. */
const char *settings_kind(setting_id_t id);

/* The built-in value, for reporting what a reset would go back to. */
const char *settings_default(setting_id_t id);

/*
 * Called after a successful set, from whichever task made it. Registered
 * callbacks must not block for long and must not call settings_set().
 * There is room for a small fixed number; registering more is a programming
 * error and is logged.
 */
typedef void (*settings_observer_t)(setting_id_t id);
void settings_subscribe(settings_observer_t observer);

#ifdef __cplusplus
}
#endif
