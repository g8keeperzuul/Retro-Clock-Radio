/*
 * Render the alarm screen to a PPM.
 *
 *   gcc -std=gnu11 -I src -I tools/hostrender/stub -o /tmp/render_alarm \
 *       tools/hostrender/render_alarm.c src/epd_alarm.c src/epd_paint.c \
 *       src/settings.c src/alarm_sounds.c -lm
 *   SET_ALARM=07:30 /tmp/render_alarm out.ppm && convert out.ppm out.png
 *
 * SET_ALARM, SET_ALARM_ON and SET_SOUND stand in for the MQTT setters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epd_paint.h"
#include "epd_alarm.h"
#include "nvs.h"
#include "settings.h"

static uint8_t FB[EPD_FB_SIZE];
uint8_t *epd_fb_alloc(void) { return FB; }
void epd_fb_free(uint8_t *fb) { (void)fb; }
esp_err_t epd_wake(void) { return ESP_OK; }
esp_err_t epd_display(const uint8_t *fb) { (void)fb; return ESP_OK; }
esp_err_t epd_sleep(void) { return ESP_OK; }

/* No NVS: every parameter starts on its built-in value. */
esp_err_t nvs_open(const char *n, nvs_open_mode_t m, nvs_handle_t *h) { return ESP_FAIL; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *n) { return ESP_FAIL; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_FAIL; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { return ESP_FAIL; }

int main(int argc, char **argv)
{
    static const unsigned char RGB[4][3] = {
        {0,0,0},{255,255,255},{230,200,40},{200,40,40} };
    settings_init();

    if (getenv("SET_ALARM"))    settings_set(SETTING_ALARM_TIME, getenv("SET_ALARM"));
    if (getenv("SET_ALARM_ON")) settings_set(SETTING_ALARM_ENABLED, getenv("SET_ALARM_ON"));
    if (getenv("SET_SOUND"))    settings_set(SETTING_ALARM_SOUND, getenv("SET_SOUND"));

    epd_alarm_show();
    FILE *f = fopen(argv[1], "wb");
    fprintf(f, "P6\n%d %d\n255\n", EPD_USABLE_WIDTH, EPD_USABLE_HEIGHT);
    for (int y = 0; y < EPD_USABLE_HEIGHT; y++)
        for (int x = 0; x < EPD_USABLE_WIDTH; x++) {
            const int px = EPD_USABLE_X + x, py = EPD_USABLE_Y + y;
            const uint8_t b = FB[py * EPD_ROW_BYTES + px / 4];
            fwrite(RGB[(b >> (6 - 2 * (px % 4))) & 3], 1, 3, f);
        }
    fclose(f);
    (void)argc; return 0;
}
