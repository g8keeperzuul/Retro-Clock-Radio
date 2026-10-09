/*
 * epd_status.c - The diagnostics screen. See the header.
 */
#include "epd_status.h"

#include <stdio.h>
#include <string.h>

#include "epd_paint.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "refresh_count.h"
#include "wifi_sta.h"

static const char *TAG = "epd-status";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef TIME_ZONE
#define TIME_ZONE "UTC0"
#endif
#ifndef OWM_LATITUDE
#define OWM_LATITUDE "--"
#endif
#ifndef OWM_LONGITUDE
#define OWM_LONGITUDE "--"
#endif
#ifndef OWM_API_KEY
#define OWM_API_KEY "--"
#endif

/*
 * Whether to print the OpenWeather key in full.
 *
 * It is shown whole by default, because a diagnostics screen that cannot tell
 * you which key was flashed is not doing its job. Worth knowing before leaving
 * it that way: e-paper holds its last image with no power at all, so the key
 * stays legible on the glass after the radio is unplugged, and survives into
 * any photograph of the device. Set this to 1 to print the first and last four
 * characters only, which is still enough to tell two keys apart.
 */
#ifndef STATUS_MASK_API_KEY
#define STATUS_MASK_API_KEY 0
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
#define VALUE_X    (MARGIN + 10 * CHAR_W)           /* widest label plus a space */
#define VALUE_W    (VIEW_W - VALUE_X - MARGIN)

#define TITLE_SCALE 4
#define TITLE_Y     12
#define RULE_Y      (TITLE_Y + PAINT_FONT_H * TITLE_SCALE + 10)
#define FIRST_ROW   (RULE_Y + 18)
#define ROW_STEP    38

/*
 * One labelled row. A value too wide for its column drops to the next line and
 * runs the full width -- which is what the key does, and what a long SSID will
 * do. Returns how many lines were used, so the caller keeps its place.
 */
static int draw_row(uint8_t *fb, int y, const char *label, const char *value)
{
    paint_text(fb, LABEL_X, y, label, SCALE, EPD_BLACK);

    if (paint_text_width(value, SCALE) <= VALUE_W) {
        paint_text(fb, VALUE_X, y, value, SCALE, EPD_BLACK);
        return 1;
    }

    paint_text(fb, LABEL_X + CHAR_W, y + LINE_H + 6, value, SCALE, EPD_BLACK);
    return 2;
}

/* "3d 04h 17m", truncated to the minute as the clock radio's owner asked. */
static void format_uptime(char *out, size_t n)
{
    const uint64_t minutes = (uint64_t)(esp_timer_get_time() / 60000000);
    const unsigned d = (unsigned)(minutes / (24 * 60));
    const unsigned h = (unsigned)((minutes / 60) % 24);
    const unsigned m = (unsigned)(minutes % 60);

    if (d > 0) {
        snprintf(out, n, "%ud %02uh %02um", d, h, m);
    } else if (h > 0) {
        snprintf(out, n, "%uh %02um", h, m);
    } else {
        snprintf(out, n, "%um", m);
    }
}

static void format_api_key(char *out, size_t n)
{
#if STATUS_MASK_API_KEY
    const char *key = OWM_API_KEY;
    const size_t len = strlen(key);

    if (len <= 8) {
        snprintf(out, n, "%s", key);     /* too short to be worth hiding */
    } else {
        snprintf(out, n, "%.4s...%s  (%u chars)", key, key + len - 4, (unsigned)len);
    }
#else
    snprintf(out, n, "%s", OWM_API_KEY);
#endif
}

static void draw_status(uint8_t *fb)
{
    char ip[20], mac[20], ssid[40], key[64], uptime[24], refreshes[16];

    wifi_sta_format_ip(ip, sizeof(ip));
    wifi_sta_format_mac(mac, sizeof(mac));
    wifi_sta_format_ssid(ssid, sizeof(ssid));
    format_api_key(key, sizeof(key));
    format_uptime(uptime, sizeof(uptime));
    snprintf(refreshes, sizeof(refreshes), "%lu",
             (unsigned long)refresh_count_get());

    paint_clear(fb, EPD_WHITE);

    paint_text_centered(fb, VIEW_W / 2, TITLE_Y, "STATUS", TITLE_SCALE, EPD_BLACK);
    paint_fill_rect(fb, MARGIN, RULE_Y, VIEW_W - 2 * MARGIN, 3, EPD_BLACK);

    static const char *LABELS[] = {
        "SSID", "IP", "MAC", "TIMEZONE", "LATITUDE", "LONGITUDE",
        "OWM KEY", "REFRESHES", "UPTIME",
    };
    const char *values[] = {
        ssid, ip, mac, TIME_ZONE, OWM_LATITUDE, OWM_LONGITUDE,
        key, refreshes, uptime,
    };

    int y = FIRST_ROW;
    for (unsigned i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++) {
        const int lines = draw_row(fb, y, LABELS[i], values[i]);
        y += (lines == 1) ? ROW_STEP : (LINE_H + 6 + ROW_STEP);
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
