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

/*
 * Two kinds, and the difference matters at every call site below.
 *
 * The first eight are LIVE: a new value is adopted the moment it arrives, and
 * what is stored is also what is running.
 *
 * The rest are BOOT settings -- the credentials and addresses needed to reach
 * the network and the broker in the first place. Changing one of those under a
 * running radio would mean tearing down the connection the change arrived on,
 * so a new value is only written down; it is picked up at the next start. That
 * makes "the value in use" and "the value stored" two different things for the
 * length of an uptime, and both are needed: settings_get() answers the first,
 * settings_reported() the second.
 */
typedef enum {
    SETTING_LATITUDE,
    SETTING_LONGITUDE,
    SETTING_TIMEZONE,
    SETTING_API_KEY,
    SETTING_MQTT_INTERVAL,

    /*
     * The alarm. ALARM_TIME is "HH:MM" on a 24-hour clock, or empty for no
     * alarm -- and unlike every other parameter, a value that does not parse
     * is not rejected but taken to mean "no alarm". ALARM_ENABLED is "ON" or
     * "OFF", and is what the knob's button flips on the alarm screen; it is
     * held OFF while there is no alarm time, and a set of ON is rejected.
     * ALARM_SOUND is a file name from alarm_sounds.c.
     */
    SETTING_ALARM_TIME,
    SETTING_ALARM_ENABLED,
    SETTING_ALARM_SOUND,

    SETTING_WIFI_SSID,
    SETTING_WIFI_PASSWORD,
    SETTING_MQTT_BROKER_URI,
    SETTING_MQTT_USERNAME,
    SETTING_MQTT_PASSWORD,

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

/*
 * The value in use. Never NULL, never empty, safe to read from any task.
 *
 * For a boot setting this is what was loaded at start-up, which is deliberately
 * not affected by a set made since: the radio is still on that network, still
 * talking to that broker, and saying otherwise would be a lie.
 */
const char *settings_get(setting_id_t id);

/*
 * What the MQTT get topic should carry, which is not always what is in use.
 *
 * For a live setting it is the value, same as settings_get(). For a boot
 * setting it is the stored override *only* -- empty when there is none, which
 * is how a subscriber is told "this one is still on its compile-time value".
 * Published to a retained topic, an empty payload also clears the retention,
 * so "nothing" survives as nothing.
 */
const char *settings_reported(setting_id_t id);

/* Whether a change to this one waits for a restart. */
bool settings_takes_effect_on_reboot(setting_id_t id);

/* The current value as a whole number. Zero for a parameter that is not one. */
int32_t settings_get_int(setting_id_t id);

/* Whether a "switch" parameter is "ON". False for anything else. */
bool settings_get_bool(setting_id_t id);

/*
 * Validate, store and apply. Returns ESP_ERR_INVALID_ARG when the value does
 * not pass, leaving the old one in place, and ESP_OK when it took -- including
 * when it was already the value, which is not an error worth distinguishing.
 *
 * Some parameters are put into a canonical form first: "7:30:00" becomes
 * "07:30", "on" becomes "ON". For ALARM_TIME anything that does not parse
 * becomes empty, which clears the alarm and returns ESP_OK.
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
