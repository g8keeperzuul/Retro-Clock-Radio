/*
 * alarm.h - Ringing at the set time, and what the knob's button does about it.
 *
 * The alarm is three parameters in settings.h -- ALARM_TIME, ALARM_ENABLED and
 * ALARM_SOUND -- so it is set over MQTT like everything else and survives a
 * reboot in NVS. This module only watches the clock: when the local time
 * reaches ALARM_TIME on a minute with the alarm enabled, it has audio_player.c
 * loop the sound for ALARM_RING_S and then stop.
 *
 * The button on the knob is routed through here, because what a press means
 * depends on the alarm: it silences one that is ringing, and otherwise, with
 * the alarm screen showing, switches the alarm on or off. Anywhere else a
 * press does nothing, which is what it did before there was an alarm.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start watching the clock. Call after settings_init() and audio_player_start().
 * A clock that NTP has not set yet is waited out, not rung on.
 */
esp_err_t alarm_start(void);

/*
 * The knob's button went down. Returns whether the press did anything, which
 * is what the knob's light reports. Shaped as a rotary_encoder_press_cb_t.
 */
bool alarm_button_pressed(void);

#ifdef __cplusplus
}
#endif
