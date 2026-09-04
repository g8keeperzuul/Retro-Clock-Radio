/*
 * weather_types.h - The shape of a day's weather, as the screen needs it.
 *
 * Deliberately free of ESP-IDF: everything here, and everything in
 * weather_parse.c and weather_aggregate.c, compiles on a host so the awkward
 * parts -- classifying conditions, dividing a day into four -- can be tested
 * without a panel or a network.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Conditions, in ascending order of how much they should change your plans.
 * The numeric order IS the severity rank, so "the worse of two" is a
 * comparison and the whole ranking is visible in one place.
 *
 * Finer-grained than the nine icons, because collapsing conditions to glyphs
 * is the renderer's business: the parser should not throw away the difference
 * between freezing rain and light rain before anyone has had a chance to use it.
 */
typedef enum {
    WX_UNKNOWN = 0,
    WX_CLEAR,          /* 800           */
    WX_FEW_CLOUDS,     /* 801           */
    WX_PART_CLOUDY,    /* 802           */
    WX_OVERCAST,       /* 803, 804      */
    WX_FOG,            /* 7xx bar squall and tornado */
    WX_DRIZZLE,        /* 3xx           */
    WX_SHOWERS,        /* 520-531       */
    WX_RAIN,           /* 500-504       */
    WX_SLEET,          /* 511, 611-616  */
    WX_SNOW,           /* 600-602, 620-622 */
    WX_THUNDERSTORM,   /* 2xx           */
    WX_SEVERE,         /* 771 squall, 781 tornado */
    WX_COND_COUNT
} wx_cond_t;

/*
 * The four parts of the day, in chronological order. Night comes first because
 * it starts at midnight; the dial draws them clockwise from the top left,
 * which is the renderer's concern and not this file's.
 */
typedef enum {
    WX_NIGHT = 0,      /* midnight -> sunrise */
    WX_MORNING,        /* sunrise  -> noon    */
    WX_AFTERNOON,      /* noon     -> sunset  */
    WX_EVENING,        /* sunset   -> midnight */
    WX_PART_COUNT
} wx_part_t;

/* Where a window's condition came from, best first. */
typedef enum {
    WX_SRC_NONE = 0,   /* nothing known                       */
    WX_SRC_HOURLY,     /* aggregated from the hourly forecast */
    WX_SRC_CARRIED,    /* carried from an earlier poll today  */
    WX_SRC_DAILY,      /* fell back to the whole-day condition */
} wx_src_t;

typedef enum { WX_UNITS_STANDARD, WX_UNITS_METRIC, WX_UNITS_IMPERIAL } wx_units_t;

typedef struct {
    wx_cond_t cond;
    wx_src_t  src;
    int16_t   temp_x10;     /* tenths of the configured unit */
    bool      temp_valid;
    uint8_t   pop_pct;      /* highest chance of precipitation in the window */
    uint8_t   clouds_pct;   /* mean cloudiness */
    uint8_t   samples;      /* hourly entries that landed in the window */
    time_t    start, end;   /* unix UTC, start inclusive and end exclusive */
} wx_window_t;

typedef struct {
    bool       valid;
    wx_units_t units;
    time_t     fetched_at;
    time_t     day_start;       /* local midnight of the day described */
    time_t     sunrise, sunset;
    bool       sunrise_valid;   /* false through a polar day or night */
    bool       sunset_valid;
    float      moon_phase;      /* 0 and 1 new, 0.25 first quarter, 0.5 full */
    wx_window_t part[WX_PART_COUNT];
} wx_report_t;

/* Classify an OpenWeather condition id. Ids, not icon codes: an id separates
 * freezing rain from light rain, and the two share an icon. */
wx_cond_t wx_cond_from_id(int id);

const char *wx_cond_label(wx_cond_t cond);    /* "OVERCAST" */
const char *wx_part_label(wx_part_t part);    /* "MORNING"  */
const char *wx_unit_symbol(wx_units_t units); /* "F"        */

/* Which of the eight moon glyphs a phase of 0..1 falls on. */
uint8_t wx_moon_octant(float phase);

/*
 * True when two reports would draw the same screen. The panel is rated for a
 * finite number of refreshes and a half-hourly poll usually returns the same
 * forecast, so this is what stops it redrawing an identical dial.
 */
bool wx_report_equal(const wx_report_t *a, const wx_report_t *b);

#ifdef __cplusplus
}
#endif
