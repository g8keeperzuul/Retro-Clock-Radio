/*
 * alarm.c - Ringing at the set time. See the header.
 */
#include "alarm.h"

#include <stdio.h>
#include <time.h>

#include "alarm_sounds.h"
#include "audio_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_manager.h"
#include "settings.h"
#include "time_sync.h"

static const char *TAG = "alarm";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

/* How long the sound loops before giving up on whoever it was meant to wake. */
#ifndef ALARM_RING_S
#define ALARM_RING_S 300
#endif

/* ---- Internals ----------------------------------------------------------- */

/* How often to look again while the clock is still unset. */
#define UNSET_CLOCK_RETRY_S 15

/*
 * How far past the turn of a minute to wake. A tick or two of slack either way
 * would otherwise land the check on 06:59:59.99 and miss 07:00 for a whole day.
 */
#define MINUTE_MARGIN_MS 250

#define TASK_STACK 3072
#define TASK_PRIO  3

static TaskHandle_t s_task;

/*
 * The minute last rung, as a count of minutes that is unique across days. It
 * is what stops an alarm that has been silenced from starting again on the
 * next check within the same minute -- and what lets an alarm set for the
 * minute it is now still go off, which is the least surprising reading of it.
 */
static long s_last_rung = -1;

static long minute_ordinal(const struct tm *t)
{
    return ((long)t->tm_year * 366 + t->tm_yday) * 1440 + t->tm_hour * 60 + t->tm_min;
}

/* Ring if this is the minute. Returns nothing: the next check is on a timer. */
static void check(const struct tm *now)
{
    if (!settings_get_bool(SETTING_ALARM_ENABLED)) {
        return;
    }

    /* settings.c guarantees "HH:MM" or empty, so this either parses or the
     * alarm is unset. */
    int h, m;
    if (sscanf(settings_get(SETTING_ALARM_TIME), "%2d:%2d", &h, &m) != 2) {
        return;
    }
    if (now->tm_hour != h || now->tm_min != m) {
        return;
    }

    const long minute = minute_ordinal(now);
    if (minute == s_last_rung) {
        return;
    }
    s_last_rung = minute;

    char sound[64];
    snprintf(sound, sizeof(sound), "%s", settings_get(SETTING_ALARM_SOUND));

    const uint8_t track = alarm_sound_track(sound);
    if (track == 0) {
        /* settings.c validates the name against the same table, so this is a
         * table edited under a stored value. */
        ESP_LOGE(TAG, "%02d:%02d: no track for \"%s\" -- not ringing", h, m, sound);
        return;
    }

    ESP_LOGI(TAG, "%02d:%02d: ringing %s for %d s", h, m, sound, ALARM_RING_S);
    audio_player_loop(ALARM_SOUND_FOLDER, track, ALARM_RING_S);
}

static void alarm_task(void *arg)
{
    (void)arg;

    while (true) {
        TickType_t wait;

        if (!time_sync_is_valid()) {
            wait = pdMS_TO_TICKS(UNSET_CLOCK_RETRY_S * 1000);
        } else {
            const time_t t = time(NULL);
            struct tm now;
            localtime_r(&t, &now);

            check(&now);

            wait = pdMS_TO_TICKS((60 - now.tm_sec) * 1000 + MINUTE_MARGIN_MS);
        }

        /* A setting changing cuts this short, so a time set for the current
         * minute is caught now and not at the turn of the next one. */
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

static void on_setting_changed(setting_id_t id)
{
    if (id != SETTING_ALARM_TIME && id != SETTING_ALARM_ENABLED &&
        id != SETTING_ALARM_SOUND && id != SETTING_TIMEZONE) {
        return;
    }

    /*
     * Switching the alarm off while it rings means "stop", whether it came
     * from the knob or from Home Assistant.
     */
    if (id == SETTING_ALARM_ENABLED && !settings_get_bool(SETTING_ALARM_ENABLED) &&
        audio_player_is_playing()) {
        audio_player_stop();
    }

    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

bool alarm_button_pressed(void)
{
    /* Silencing outranks everything, including the alarm screen's toggle: a
     * press to stop the noise must never also switch tomorrow's alarm off. */
    if (audio_player_is_playing()) {
        ESP_LOGI(TAG, "silenced by the knob");
        audio_player_stop();
        return true;
    }

    if (screen_manager_active() == SCREEN_ALARM) {
        const bool on = !settings_get_bool(SETTING_ALARM_ENABLED);

        /* Refused when there is no alarm time; the knob stays dark to say so. */
        if (settings_set(SETTING_ALARM_ENABLED, on ? "ON" : "OFF") != ESP_OK) {
            ESP_LOGI(TAG, "no alarm time set; nothing to switch on");
            return false;
        }
        ESP_LOGI(TAG, "switched %s by the knob", on ? "on" : "off");
        return true;
    }

    return false;
}

esp_err_t alarm_start(void)
{
    if (xTaskCreate(alarm_task, "alarm", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    settings_subscribe(on_setting_changed);

    const char *at = settings_get(SETTING_ALARM_TIME);
    if (at[0] == '\0') {
        ESP_LOGI(TAG, "no alarm set");
    } else {
        ESP_LOGI(TAG, "alarm at %s, %s, sound %s", at,
                 settings_get_bool(SETTING_ALARM_ENABLED) ? "on" : "off",
                 settings_get(SETTING_ALARM_SOUND));
    }
    return ESP_OK;
}
