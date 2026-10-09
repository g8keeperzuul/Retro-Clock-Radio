/*
 * epd_forecast.h - Today and tomorrow, side by side above one another.
 *
 * Two identical panels stacked in the 478 x 460 usable box: the date and what
 * the day will be like across the top of each, then five columns -- temperature,
 * humidity, precipitation, wind, cloud -- each an icon over its reading.
 *
 * The figures come from weather_daily.h, which derives them from the free
 * three-hourly forecast. See README.md "Two-day forecast".
 */
#pragma once

#include <stdint.h>

#include "epd_5in83g.h"
#include "weather_daily.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Draw both days into `fb`. Either may be invalid, in which case its half says
 * so rather than being left blank -- a blank half looks like a drawing fault,
 * and this panel holds its last image for days.
 */
void epd_forecast_draw(uint8_t *fb, const wx_daysum_t *today,
                       const wx_daysum_t *tomorrow);

/* Draw and push to the panel. Blocks for a refresh, about 25 seconds. */
esp_err_t epd_forecast_show(const wx_daysum_t *today, const wx_daysum_t *tomorrow);

#ifdef __cplusplus
}
#endif
