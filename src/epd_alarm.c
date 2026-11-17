/*
 * epd_alarm.c - The alarm screen. See the header.
 */
#include "epd_alarm.h"

#include <stdio.h>
#include <string.h>

#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

static const char *TAG = "epd-alarm";

#define VIEW_W EPD_USABLE_WIDTH    /* 478 */
#define VIEW_H EPD_USABLE_HEIGHT   /* 460 */

#define MARGIN 10

/* Matches the status screen, so the two utility screens read as a pair. */
#define TITLE_SCALE 4
#define TITLE_Y     12
#define RULE_Y      (TITLE_Y + PAINT_FONT_H * TITLE_SCALE + 10)

/*
 * "00:00" at scale 14 is 406 px wide and 98 tall: the largest that fits the
 * box with a margin each side.
 */
#define TIME_SCALE 14
#define TIME_Y     (RULE_Y + 36)

#define STATE_SCALE 6
#define STATE_Y     (TIME_Y + PAINT_FONT_H * TIME_SCALE + 32)

/*
 * The sound's name. Scale 3 when it fits on a line, which the default does;
 * otherwise scale 2, split across as many lines as it takes. Scale 1 is not an
 * option -- this panel renders a one-pixel stroke grey.
 */
#define SOUND_LABEL_Y (STATE_Y + PAINT_FONT_H * STATE_SCALE + 36)
#define SOUND_Y       (SOUND_LABEL_Y + PAINT_FONT_H * 2 + 12)
#define SOUND_W       (VIEW_W - 2 * MARGIN)
#define SOUND_STEP    (PAINT_FONT_H * 2 + 6)
#define SOUND_CHARS   ((SOUND_W + 2) / PAINT_CHAR_ADVANCE(2))

#define HINT_SCALE 2
#define HINT_Y     (VIEW_H - MARGIN - PAINT_FONT_H * HINT_SCALE)

/* "retro-alarm.wav" -> "retro-alarm": the extension tells nobody anything. */
static void sound_label(char *out, size_t len, const char *file)
{
    snprintf(out, len, "%s", file);

    char *dot = strrchr(out, '.');
    if (dot != NULL && dot != out) {
        *dot = '\0';
    }
}

static void draw_sound(uint8_t *fb, const char *name)
{
    if (paint_text_width(name, 3) <= SOUND_W) {
        paint_text_centered(fb, VIEW_W / 2, SOUND_Y, name, 3, EPD_BLACK);
        return;
    }

    size_t left = strlen(name);
    const char *p = name;
    int y = SOUND_Y;

    /* Stop short of the hint rather than draw over it. */
    while (left > 0 && y + SOUND_STEP <= HINT_Y) {
        const size_t n = (left < SOUND_CHARS) ? left : (size_t)SOUND_CHARS;
        char slice[SOUND_CHARS + 1];

        memcpy(slice, p, n);
        slice[n] = '\0';
        paint_text_centered(fb, VIEW_W / 2, y, slice, 2, EPD_BLACK);

        p += n;
        left -= n;
        y += SOUND_STEP;
    }
}

static void draw_alarm(uint8_t *fb)
{
    char at[8], sound[64];
    snprintf(at, sizeof(at), "%s", settings_get(SETTING_ALARM_TIME));
    sound_label(sound, sizeof(sound), settings_get(SETTING_ALARM_SOUND));

    const bool set = (at[0] != '\0');
    const bool on = settings_get_bool(SETTING_ALARM_ENABLED);

    paint_clear(fb, EPD_WHITE);

    paint_text_centered(fb, VIEW_W / 2, TITLE_Y, "ALARM", TITLE_SCALE, EPD_BLACK);
    paint_fill_rect(fb, MARGIN, RULE_Y, VIEW_W - 2 * MARGIN, 3, EPD_BLACK);

    paint_text_centered(fb, VIEW_W / 2, TIME_Y, set ? at : "--:--", TIME_SCALE,
                        EPD_BLACK);

    /*
     * Red only for an alarm that will actually go off. An enabled switch with
     * no time behind it is not that, and saying "ON" would be a promise.
     */
    const char *state = !set ? "NOT SET" : on ? "ON" : "OFF";
    paint_text_centered(fb, VIEW_W / 2, STATE_Y, state, STATE_SCALE,
                        (set && on) ? EPD_RED : EPD_BLACK);

    paint_text_centered(fb, VIEW_W / 2, SOUND_LABEL_Y, "SOUND", 2, EPD_BLACK);
    draw_sound(fb, sound);

    paint_text_centered(fb, VIEW_W / 2, HINT_Y, "PRESS KNOB TO SWITCH ON/OFF",
                        HINT_SCALE, EPD_BLACK);
}

esp_err_t epd_alarm_show(void)
{
    uint8_t *fb = epd_fb_alloc();
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_NO_MEM, TAG, "no room for a %u byte frame",
                        (unsigned)EPD_FB_SIZE);

    draw_alarm(fb);

    esp_err_t err = epd_wake();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "refreshing -- this takes about 25 seconds");
        err = epd_display(fb);
    }
    epd_fb_free(fb);

    if (err != ESP_OK) {
        return err;
    }

    /* epd_display() leaves the panel driving; without this it washes out. */
    ESP_RETURN_ON_ERROR(epd_sleep(), TAG, "sleep");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "alarm displayed, panel asleep");
    return ESP_OK;
}
