/*
 * weather_types.c - Condition classification and labels. See the header.
 */
#include "weather_types.h"

#include <string.h>

wx_cond_t wx_cond_from_id(int id)
{
    /* Groups follow OpenWeather's own numbering; see their conditions table. */
    if (id >= 200 && id < 300) {
        return WX_THUNDERSTORM;
    }
    if (id >= 300 && id < 400) {
        return WX_DRIZZLE;
    }
    if (id == 511) {
        return WX_SLEET;            /* freezing rain, which 5xx otherwise hides */
    }
    if (id >= 500 && id <= 504) {
        return WX_RAIN;
    }
    if (id >= 520 && id <= 531) {
        return WX_SHOWERS;
    }
    if (id == 771 || id == 781) {
        return WX_SEVERE;           /* squall and tornado, not mere haze */
    }
    if (id >= 611 && id <= 616) {
        return WX_SLEET;
    }
    if (id >= 600 && id < 700) {
        return WX_SNOW;
    }
    if (id >= 700 && id < 800) {
        return WX_FOG;
    }
    if (id == 800) {
        return WX_CLEAR;
    }
    if (id == 801) {
        return WX_FEW_CLOUDS;
    }
    if (id == 802) {
        return WX_PART_CLOUDY;
    }
    if (id == 803 || id == 804) {
        return WX_OVERCAST;
    }
    return WX_UNKNOWN;
}

const char *wx_cond_label(wx_cond_t cond)
{
    /* Short enough to sit under an icon in a quadrant. */
    static const char *LABELS[WX_COND_COUNT] = {
        [WX_UNKNOWN]      = "NO DATA",
        [WX_CLEAR]        = "CLEAR",
        [WX_FEW_CLOUDS]   = "FAIR",
        [WX_PART_CLOUDY]  = "CLOUDY",
        [WX_OVERCAST]     = "OVERCAST",
        [WX_FOG]          = "FOG",
        [WX_DRIZZLE]      = "DRIZZLE",
        [WX_SHOWERS]      = "SHOWERS",
        [WX_RAIN]         = "RAIN",
        [WX_SLEET]        = "SLEET",
        [WX_SNOW]         = "SNOW",
        [WX_THUNDERSTORM] = "STORM",
        [WX_SEVERE]       = "SEVERE",
    };

    return (cond < WX_COND_COUNT && LABELS[cond]) ? LABELS[cond] : "NO DATA";
}

const char *wx_part_label(wx_part_t part)
{
    static const char *LABELS[WX_PART_COUNT] = {
        [WX_NIGHT]     = "NIGHT",
        [WX_MORNING]   = "MORNING",
        [WX_AFTERNOON] = "AFTERNOON",
        [WX_EVENING]   = "EVENING",
    };

    return (part < WX_PART_COUNT) ? LABELS[part] : "";
}

const char *wx_unit_symbol(wx_units_t units)
{
    switch (units) {
    case WX_UNITS_IMPERIAL: return "F";
    case WX_UNITS_METRIC:   return "C";
    default:                return "K";
    }
}

uint8_t wx_moon_octant(float phase)
{
    /*
     * Eight glyphs over a cycle that wraps, so the boundaries sit half a step
     * either side of each named phase: anything within a sixteenth of 0.5 is
     * "full" rather than a gibbous that happens to round there.
     */
    if (phase < 0.0f || phase > 1.0f) {
        return 0;
    }

    const int octant = (int)(phase * 8.0f + 0.5f);
    return (uint8_t)(octant & 7);
}

bool wx_report_equal(const wx_report_t *a, const wx_report_t *b)
{
    if (a->valid != b->valid) {
        return false;
    }
    if (!a->valid) {
        return true;    /* two blanks draw the same nothing */
    }

    /*
     * fetched_at is deliberately not compared: it changes every poll and never
     * reaches the glass. Comparing it would defeat the whole point.
     */
    if (a->day_start != b->day_start || a->units != b->units ||
        a->sunrise != b->sunrise || a->sunset != b->sunset ||
        a->sunrise_valid != b->sunrise_valid || a->sunset_valid != b->sunset_valid) {
        return false;
    }

    /* A phase difference too small to move the glyph is not a difference. */
    if (wx_moon_octant(a->moon_phase) != wx_moon_octant(b->moon_phase)) {
        return false;
    }

    for (int i = 0; i < WX_PART_COUNT; i++) {
        const wx_window_t *x = &a->part[i], *y = &b->part[i];

        if (x->cond != y->cond || x->temp_valid != y->temp_valid ||
            (x->temp_valid && x->temp_x10 != y->temp_x10)) {
            return false;
        }
    }
    return true;
}
