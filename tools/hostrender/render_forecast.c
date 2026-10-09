/*
 * Render the two-day forecast screen to a PPM, from a captured API response.
 *
 * Nothing here runs on the device: it exists so the layout can be looked at
 * before a 25 second refresh is spent on it. See tools/hostrender/render.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "epd_forecast.h"
#include "epd_paint.h"
#include "weather_aggregate.h"
#include "weather_parse25.h"

static uint8_t FB[EPD_FB_SIZE];

/* The panel's half of epd_5in83g.c, which needs an SPI bus to exist. */
uint8_t *epd_fb_alloc(void) { return FB; }
void epd_fb_free(uint8_t *fb) { (void)fb; }
esp_err_t epd_wake(void) { return ESP_OK; }
esp_err_t epd_display(const uint8_t *fb) { (void)fb; return ESP_OK; }

static void write_ppm(const char *path)
{
    static const unsigned char RGB[4][3] = {
        {0, 0, 0}, {255, 255, 255}, {230, 200, 40}, {200, 40, 40},
    };
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", EPD_USABLE_WIDTH, EPD_USABLE_HEIGHT);

    for (int y = 0; y < EPD_USABLE_HEIGHT; y++) {
        for (int x = 0; x < EPD_USABLE_WIDTH; x++) {
            const int px = EPD_USABLE_X + x, py = EPD_USABLE_Y + y;
            const uint8_t byte = FB[py * EPD_ROW_BYTES + px / 4];
            fwrite(RGB[(byte >> (6 - 2 * (px % 4))) & 3], 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s response.json out.ppm [now_epoch]\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *body = malloc(len + 1);
    if (fread(body, 1, len, f) != (size_t)len) { perror("read"); return 2; }
    body[len] = '\0';
    fclose(f);

    wx_forecast_t *fc = calloc(1, sizeof(*fc));
    const wx_status_t st = weather_parse25(body, len, fc);
    if (st != WX_OK) {
        fprintf(stderr, "parse failed: %d\n", (int)st);
        return 1;
    }

    const time_t now = (argc > 3) ? (time_t)atoll(argv[3]) : time(NULL);

    time_t start, noon, end;
    if (weather_day_bounds(now, &start, &noon, &end) != WX_OK) {
        fprintf(stderr, "day bounds failed\n");
        return 1;
    }

    fprintf(stderr, "now=%lld  today=[%lld,%lld)  entries=%u first=%lld last=%lld\n",
            (long long)now, (long long)start, (long long)end, fc->n_hours,
            (long long)fc->hour[0].dt, (long long)fc->hour[fc->n_hours - 1].dt);

    wx_daysum_t today, tomorrow;
    time_t t_start, t_noon, t_end;
    weather_day_bounds(end + 3600, &t_start, &t_noon, &t_end);

    weather_day_summary(fc, start, end, &today);
    weather_day_summary(fc, t_start, t_end, &tomorrow);

    fprintf(stderr, "today  : %u samples, cond %u \"%s\", %d..%d, rh %u, pop %u, "
            "gust %u, cloud %u, precip %u\n", today.samples, today.cond_id,
            wx_cond_description(today.cond_id), today.temp_min_x10,
            today.temp_max_x10, today.humidity_pct, today.pop_pct,
            today.gust_x10, today.clouds_pct, today.precip_x100);
    fprintf(stderr, "tomorrow: %u samples, cond %u \"%s\", %d..%d, rh %u, pop %u, "
            "gust %u, cloud %u, precip %u\n", tomorrow.samples, tomorrow.cond_id,
            wx_cond_description(tomorrow.cond_id), tomorrow.temp_min_x10,
            tomorrow.temp_max_x10, tomorrow.humidity_pct, tomorrow.pop_pct,
            tomorrow.gust_x10, tomorrow.clouds_pct, tomorrow.precip_x100);

    epd_forecast_draw(FB, &today, &tomorrow);
    write_ppm(argv[2]);
    return 0;
}
