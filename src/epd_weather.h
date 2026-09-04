/*
 * epd_weather.h - The daily weather screen, laid out inside the usable box.
 *
 * Follows docs/pictures/weather-display-design.jpeg: the date across the top, a
 * circle quartered into the four parts of the day, sunrise and sunset flanking
 * it, and the moon below.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "weather_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compose the screen into an existing EPD_FB_SIZE frame buffer. Split out from
 * epd_weather_show() so it can be rendered and inspected off the target.
 */
void epd_weather_draw(uint8_t *fb, const wx_report_t *report);

/*
 * Bring up the panel, draw the screen, refresh, and put the panel back to
 * sleep. Blocks for roughly half a minute while the panel refreshes.
 */
esp_err_t epd_weather_show(const wx_report_t *report);

#ifdef __cplusplus
}
#endif
