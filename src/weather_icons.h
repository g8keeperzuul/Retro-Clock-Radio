/*
 * weather_icons.h - Line art for the weather screen.
 *
 * Drawn rather than downloaded. OpenWeather serves 100 x 100 anti-aliased
 * colour PNGs, which would need a decoder this firmware does not carry and
 * would come out as blobs in four inks; the mock-up this screen follows is line
 * art anyway. The geometry lives in tools/make_weather_icons.py and the table
 * in the generated weather_icons.c.
 */
#pragma once

#include <stdint.h>

#include "weather_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WX_ICON_SIZE   48
#define WX_ICON_STRIDE ((WX_ICON_SIZE + 7) / 8)

/* Order must match the emission order in tools/make_weather_icons.py. */
typedef enum {
    WX_ICON_SUN, WX_ICON_MOON,
    WX_ICON_CLOUD, WX_ICON_CLOUDS,
    WX_ICON_SUN_CLOUD, WX_ICON_MOON_CLOUD,
    WX_ICON_RAIN, WX_ICON_STORM, WX_ICON_SNOW, WX_ICON_FOG,
    WX_ICON_SUNRISE, WX_ICON_SUNSET,
    /* The five measures on the two-day forecast screen. */
    WX_ICON_THERMOMETER, WX_ICON_HUMIDITY, WX_ICON_UMBRELLA,
    WX_ICON_WIND, WX_ICON_CLOUDCOVER,
    WX_ICON_MOON_0, WX_ICON_MOON_1, WX_ICON_MOON_2, WX_ICON_MOON_3,
    WX_ICON_MOON_4, WX_ICON_MOON_5, WX_ICON_MOON_6, WX_ICON_MOON_7,
    WX_ICON_COUNT
} wx_icon_t;

typedef struct {
    const uint8_t *bits;   /* WX_ICON_SIZE rows of WX_ICON_STRIDE bytes */
} weather_icon_t;

extern const weather_icon_t WEATHER_ICONS[WX_ICON_COUNT];

/*
 * The icon for a condition. Thirteen conditions share ten glyphs -- drizzle,
 * showers and rain all get the same cloud -- because collapsing them is a
 * drawing decision and belongs next to the drawings, not in the parser.
 */
wx_icon_t weather_icon_for(wx_cond_t cond, bool night);

/* The moon glyph for a phase of 0..1. */
wx_icon_t weather_icon_for_moon(float phase);

#ifdef __cplusplus
}
#endif
