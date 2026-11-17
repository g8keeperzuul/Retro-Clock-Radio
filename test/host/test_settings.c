/*
 * test_settings.c - Host tests for the rules settings.c applies to a value on
 * its way in: the alarm time's canonical form, and that anything unparseable
 * clears the alarm rather than being rejected.
 *
 * Built and run by tools/run_host_tests.sh, against the stub ESP-IDF headers in
 * tools/hostrender/stub and the in-memory NVS below.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "alarm_sounds.h"
#include "nvs.h"
#include "settings.h"

/* ---- An NVS that forgets everything when the process ends ----------------- */

#define FAKE_KEYS 32

static struct {
    char key[16];
    char value[128];
    bool used;
} s_nvs[FAKE_KEYS];

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    (void)ns; (void)mode;
    *h = 1;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    for (int i = 0; i < FAKE_KEYS; i++) {
        if (s_nvs[i].used && strcmp(s_nvs[i].key, key) == 0) {
            const size_t n = strlen(s_nvs[i].value) + 1;
            if (n > *len) {
                return ESP_FAIL;
            }
            memcpy(out, s_nvs[i].value, n);
            *len = n;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    int slot = -1;
    for (int i = 0; i < FAKE_KEYS; i++) {
        if (s_nvs[i].used && strcmp(s_nvs[i].key, key) == 0) {
            slot = i;
            break;
        }
        if (!s_nvs[i].used && slot < 0) {
            slot = i;
        }
    }
    snprintf(s_nvs[slot].key, sizeof(s_nvs[slot].key), "%s", key);
    snprintf(s_nvs[slot].value, sizeof(s_nvs[slot].value), "%s", value);
    s_nvs[slot].used = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    for (int i = 0; i < FAKE_KEYS; i++) {
        if (s_nvs[i].used && strcmp(s_nvs[i].key, key) == 0) {
            s_nvs[i].used = false;
        }
    }
    return ESP_OK;
}

/* ---- The tests ------------------------------------------------------------ */

static int failures;
static unsigned notified;

static void count_notifications(setting_id_t id) { (void)id; notified++; }

static void eq_str(const char *what, const char *got, const char *want)
{
    const bool good = strcmp(got, want) == 0;
    printf("  %-4s %-46s \"%s\"%s%s%s\n", good ? "ok" : "FAIL", what, got,
           good ? "" : " want \"", good ? "" : want, good ? "" : "\"");
    if (!good) {
        failures++;
    }
}

static void ok(const char *what, bool cond)
{
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) {
        failures++;
    }
}

/* Set the alarm to "07:30" first, so each case shows what it does to a set alarm. */
static void alarm_case(const char *payload, const char *want)
{
    char what[64];
    settings_set(SETTING_ALARM_TIME, "07:30");
    const esp_err_t err = settings_set(SETTING_ALARM_TIME, payload);
    snprintf(what, sizeof(what), "ALARM_TIME <- \"%s\"", payload);
    eq_str(what, settings_get(SETTING_ALARM_TIME), want);
    if (err != ESP_OK) {
        printf("  FAIL     ...and it returned an error rather than ESP_OK\n");
        failures++;
    }
}

int main(void)
{
    settings_init();
    settings_subscribe(count_notifications);

    printf("alarm time, defaults\n");
    eq_str("unset out of the box", settings_get(SETTING_ALARM_TIME), "");
    eq_str("not enabled out of the box: no time", settings_get(SETTING_ALARM_ENABLED), "OFF");
    eq_str("default sound", settings_get(SETTING_ALARM_SOUND), "retro-alarm.wav");
    eq_str("ALARM_TIME topic kind", settings_kind(SETTING_ALARM_TIME), "text");
    eq_str("ALARM_ENABLED topic kind", settings_kind(SETTING_ALARM_ENABLED), "switch");

    printf("alarm time, parseable\n");
    alarm_case("06:45", "06:45");
    alarm_case("6:45", "06:45");
    alarm_case("00:00", "00:00");
    alarm_case("23:59", "23:59");
    alarm_case("06:45:00", "06:45");     /* a Home Assistant time helper */
    alarm_case("06:45\r\n", "06:45");    /* a line ending from a file */

    printf("alarm time, not parseable: unsets the alarm\n");
    alarm_case("", "");
    alarm_case("null", "");
    alarm_case("None", "");
    alarm_case("24:00", "");
    alarm_case("12:60", "");
    alarm_case("7:5", "");
    alarm_case("123:00", "");
    alarm_case("06:45:61", "");
    alarm_case("06:45 ", "");
    alarm_case("6:45pm", "");
    alarm_case("0645", "");
    alarm_case("-1:30", "");

    printf("alarm time, persistence\n");
    settings_set(SETTING_ALARM_TIME, "05:15");
    eq_str("reported on the get topic", settings_reported(SETTING_ALARM_TIME), "05:15");
    char stored[16];
    size_t len = sizeof(stored);
    ok("written to NVS", nvs_get_str(1, "s_alarm", stored, &len) == ESP_OK &&
                         strcmp(stored, "05:15") == 0);
    settings_set(SETTING_ALARM_TIME, "garbage");
    len = sizeof(stored);
    ok("unsetting erases it from NVS",
       nvs_get_str(1, "s_alarm", stored, &len) == ESP_ERR_NVS_NOT_FOUND);
    eq_str("and the get topic goes empty", settings_reported(SETTING_ALARM_TIME), "");

    printf("alarm enabled follows the alarm time\n");
    ok("ON is refused with no time set",
       settings_set(SETTING_ALARM_ENABLED, "ON") == ESP_ERR_INVALID_ARG);
    eq_str("...and it stays OFF", settings_get(SETTING_ALARM_ENABLED), "OFF");
    settings_set(SETTING_ALARM_ENABLED, "");
    eq_str("resetting with no time set gives OFF, not the default",
           settings_get(SETTING_ALARM_ENABLED), "OFF");
    settings_set(SETTING_ALARM_TIME, "06:30");
    eq_str("setting a time switches it to the default", settings_get(SETTING_ALARM_ENABLED), "ON");
    settings_set(SETTING_ALARM_ENABLED, "OFF");
    settings_set(SETTING_ALARM_TIME, "06:40");
    eq_str("changing a set time leaves the switch alone", settings_get(SETTING_ALARM_ENABLED), "OFF");
    settings_set(SETTING_ALARM_ENABLED, "ON");
    settings_set(SETTING_ALARM_TIME, "null");
    eq_str("clearing the time switches it OFF", settings_get(SETTING_ALARM_ENABLED), "OFF");
    eq_str("...on the get topic too", settings_reported(SETTING_ALARM_ENABLED), "OFF");
    settings_set(SETTING_ALARM_TIME, "06:30");
    settings_set(SETTING_ALARM_TIME, "");
    eq_str("an empty payload does the same", settings_get(SETTING_ALARM_ENABLED), "OFF");

    printf("alarm enabled\n");
    settings_set(SETTING_ALARM_TIME, "06:30");
    const char *ons[] = {"ON", "on", "true", "1", "Yes"};
    for (unsigned i = 0; i < sizeof(ons) / sizeof(ons[0]); i++) {
        settings_set(SETTING_ALARM_ENABLED, "OFF");
        settings_set(SETTING_ALARM_ENABLED, ons[i]);
        ok(ons[i], settings_get_bool(SETTING_ALARM_ENABLED));
    }
    settings_set(SETTING_ALARM_ENABLED, "off");
    ok("\"off\" switches it off", !settings_get_bool(SETTING_ALARM_ENABLED));
    eq_str("stored as OFF", settings_get(SETTING_ALARM_ENABLED), "OFF");
    ok("\"maybe\" is rejected",
       settings_set(SETTING_ALARM_ENABLED, "maybe") == ESP_ERR_INVALID_ARG);
    eq_str("...and leaves it as it was", settings_get(SETTING_ALARM_ENABLED), "OFF");
    settings_set(SETTING_ALARM_ENABLED, "");
    eq_str("empty goes back to the default", settings_get(SETTING_ALARM_ENABLED), "ON");

    printf("alarm sound\n");
    settings_set(SETTING_ALARM_SOUND, "./sfx/phony-buzzing-alarm-fx_99bpm.wav");
    eq_str("a path is reduced to its file name", settings_get(SETTING_ALARM_SOUND),
           "phony-buzzing-alarm-fx_99bpm.wav");
    ok("an unknown file is rejected",
       settings_set(SETTING_ALARM_SOUND, "nope.wav") == ESP_ERR_INVALID_ARG);
    eq_str("...and leaves it as it was", settings_get(SETTING_ALARM_SOUND),
           "phony-buzzing-alarm-fx_99bpm.wav");
    settings_set(SETTING_ALARM_SOUND, "");
    eq_str("empty goes back to the default", settings_get(SETTING_ALARM_SOUND),
           "retro-alarm.wav");
    ok("the default is track 1", alarm_sound_track("retro-alarm.wav") == 1);

    printf("observers\n");
    notified = 0;
    settings_set(SETTING_ALARM_TIME, "08:00");
    ok("a change is announced", notified == 1);
    settings_set(SETTING_ALARM_TIME, "8:00");
    ok("the same time spelled differently is not a change", notified == 1);

    printf("\n%s: %d failure%s\n", failures ? "FAILED" : "passed", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
