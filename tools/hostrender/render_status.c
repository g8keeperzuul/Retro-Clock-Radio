/* Render the diagnostics screen to a PPM. See render.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epd_paint.h"
#include "epd_status.h"
#include "settings.h"
#include "refresh_count.h"
#include "wifi_sta.h"

static uint8_t FB[EPD_FB_SIZE];
uint8_t *epd_fb_alloc(void) { return FB; }
void epd_fb_free(uint8_t *fb) { (void)fb; }
esp_err_t epd_wake(void) { return ESP_OK; }
esp_err_t epd_display(const uint8_t *fb) { (void)fb; return ESP_OK; }
esp_err_t epd_sleep(void) { return ESP_OK; }
uint32_t refresh_count_get(void) { return 48217; }
size_t wifi_sta_format_ip(char *b, size_t n) { return snprintf(b, n, "192.168.137.214"); }
size_t wifi_sta_format_mac(char *b, size_t n) { return snprintf(b, n, "a0:85:e3:4b:19:7c"); }
size_t wifi_sta_format_ssid(char *b, size_t n) { return snprintf(b, n, "%s", getenv("SSID") ?: "ETHERCI5"); }
int wifi_sta_rssi(void) { return -57; }
_Bool wifi_sta_is_connected(void) { return 1; }
int64_t esp_timer_get_time(void) { return 275040LL * 1000000LL; }

int main(int argc, char **argv)
{
    static const unsigned char RGB[4][3] = {
        {0,0,0},{255,255,255},{230,200,40},{200,40,40} };
    settings_init();

    /* Pretend an MQTT setter has been along, so the render shows live values
     * rather than the flags the firmware was built with. */
    if (getenv("SET_TZ"))       settings_set(SETTING_TIMEZONE, getenv("SET_TZ"));
    if (getenv("SET_LAT"))      settings_set(SETTING_LATITUDE, getenv("SET_LAT"));
    if (getenv("SET_LON"))      settings_set(SETTING_LONGITUDE, getenv("SET_LON"));
    if (getenv("SET_INTERVAL")) settings_set(SETTING_MQTT_INTERVAL, getenv("SET_INTERVAL"));

    epd_status_show();
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
