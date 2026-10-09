/*
 * epd_status.c - The diagnostics screen. See the header.
 */
#include "epd_status.h"

#include <stdio.h>
#include <string.h>

#include "diagnostics.h"
#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd-status";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

/*
 * The MQTT rows appear only when there is a client to describe. The broker URI
 * is printed as configured -- worth remembering that a URI of the form
 * mqtt://user:pass@host would put those credentials on the glass, and e-paper
 * keeps its last image with no power at all.
 */
#ifndef MQTT_ENABLED
#define MQTT_ENABLED 0
#endif
#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI "--"
#endif
#ifndef MQTT_TOPIC
#define MQTT_TOPIC "--"
#endif
#ifndef MQTT_PUBLISH_INTERVAL_S
#define MQTT_PUBLISH_INTERVAL_S 10
#endif

#define VIEW_W EPD_USABLE_WIDTH    /* 478 */

/*
 * Scale 2 is the largest that fits a 32-character key across the box, and the
 * smallest whose strokes this panel renders black rather than grey.
 */
#define SCALE      2
#define CHAR_W     PAINT_CHAR_ADVANCE(SCALE)        /* 12 */
#define LINE_H     (PAINT_FONT_H * SCALE)           /* 14 */

#define MARGIN     10
#define LABEL_X    MARGIN
#define VALUE_X    (MARGIN + 11 * CHAR_W)           /* widest label plus a space */
#define VALUE_W    (VIEW_W - VALUE_X - MARGIN)

/* Where an over-long value continues, and how much of it fits on a line. */
#define WRAP_X     (LABEL_X + CHAR_W)
#define WRAP_CHARS ((VIEW_W - WRAP_X - MARGIN) / CHAR_W)
#define WRAP_STEP  (LINE_H + 4)

#define TITLE_SCALE 4
#define TITLE_Y     12
#define RULE_Y      (TITLE_Y + PAINT_FONT_H * TITLE_SCALE + 10)
#define FIRST_ROW   (RULE_Y + 18)

/*
 * Eleven rows and fourteen lines in the worst case -- every MQTT field present
 * and both the key and the topic wrapped. That is what sets this: 30 leaves
 * about twenty pixels at the foot, and anything larger runs off the bottom.
 */
#define ROW_STEP    30

/*
 * One labelled row. A value too wide for its column runs on under the label
 * instead, in as many lines as it takes -- the MQTT topic needs two, and a long
 * SSID or the key need one. Returns the lines used, so the caller keeps place.
 */
static int draw_row(uint8_t *fb, int y, const char *label, const char *value)
{
    paint_text(fb, LABEL_X, y, label, SCALE, EPD_BLACK);

    if (paint_text_width(value, SCALE) <= VALUE_W) {
        paint_text(fb, VALUE_X, y, value, SCALE, EPD_BLACK);
        return 1;
    }

    /*
     * Split blind at whatever fits rather than on word boundaries: these are
     * URIs, topics and keys, which have no words to break at. Scale 1 would
     * fit them whole, and is not an option -- this panel renders a one-pixel
     * stroke grey, which is the whole reason the type here is scale 2.
     */
    size_t left = strlen(value);
    const char *p = value;
    int lines = 1;

    while (left > 0) {
        const size_t n = (left < WRAP_CHARS) ? left : (size_t)WRAP_CHARS;
        char slice[WRAP_CHARS + 1];

        memcpy(slice, p, n);
        slice[n] = '\0';
        paint_text(fb, WRAP_X, y + lines * WRAP_STEP, slice, SCALE, EPD_BLACK);

        p += n;
        left -= n;
        lines++;
    }

    return lines;
}

static void draw_status(uint8_t *fb)
{
    diag_t d;
    diagnostics_gather(&d);

    char uptime[16], refreshes[16], latlng[32];
    diagnostics_format_uptime(&d, uptime, sizeof(uptime));
    snprintf(refreshes, sizeof(refreshes), "%lu", (unsigned long)d.refreshes);
    snprintf(latlng, sizeof(latlng), "%s,%s", d.latitude, d.longitude);

#if MQTT_ENABLED
    char every[16];
    snprintf(every, sizeof(every), "%d S", MQTT_PUBLISH_INTERVAL_S);
#endif

    paint_clear(fb, EPD_WHITE);

    paint_text_centered(fb, VIEW_W / 2, TITLE_Y, "STATUS", TITLE_SCALE, EPD_BLACK);
    paint_fill_rect(fb, MARGIN, RULE_Y, VIEW_W - 2 * MARGIN, 3, EPD_BLACK);

    /*
     * The MQTT rows are compiled in or out with the client itself, so a build
     * without one does not show three lines about a broker it will never talk
     * to. Both arrays are indexed together; keep them in step.
     */
    static const char *LABELS[] = {
        "SSID", "IP", "MAC", "TIMEZONE", "LAT,LNG", "OWM KEY",
#if MQTT_ENABLED
        "MQTT URI", "MQTT TOPIC", "MQTT EVERY",
#endif
        "REFRESHES", "UPTIME",
    };
    const char *values[] = {
        d.ssid, d.ip, d.mac, d.timezone, latlng, d.api_key,
#if MQTT_ENABLED
        MQTT_BROKER_URI, MQTT_TOPIC, every,
#endif
        refreshes, uptime,
    };

    int y = FIRST_ROW;
    for (unsigned i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++) {
        const int lines = draw_row(fb, y, LABELS[i], values[i]);
        y += (lines - 1) * WRAP_STEP + ROW_STEP;
    }
}

esp_err_t epd_status_show(void)
{
    uint8_t *fb = epd_fb_alloc();
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_NO_MEM, TAG, "no room for a %u byte frame",
                        (unsigned)EPD_FB_SIZE);

    draw_status(fb);

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

    ESP_LOGI(TAG, "status displayed, panel asleep");
    return ESP_OK;
}
