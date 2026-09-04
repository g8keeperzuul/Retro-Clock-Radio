/*
 * weather_icons.c - Which drawing goes with which condition.
 *
 * The bitmaps themselves are generated into weather_icons_data.c; this is the
 * mapping, kept by hand because it is a judgement about what to show rather
 * than geometry. Thirteen conditions share ten glyphs: drizzle, showers and
 * rain all get the same cloud, because at 76 pixels the distinction is not one
 * a drawing can carry, and the word underneath already says which it is.
 */
#include "weather_icons.h"

wx_icon_t weather_icon_for(wx_cond_t cond, bool night)
{
    switch (cond) {
    case WX_CLEAR:
        return night ? WX_ICON_MOON : WX_ICON_SUN;
    case WX_FEW_CLOUDS:
        return night ? WX_ICON_MOON_CLOUD : WX_ICON_SUN_CLOUD;
    case WX_PART_CLOUDY:
        return WX_ICON_CLOUD;
    case WX_OVERCAST:
        return WX_ICON_CLOUDS;
    case WX_FOG:
        return WX_ICON_FOG;
    case WX_DRIZZLE:
    case WX_SHOWERS:
    case WX_RAIN:
        return WX_ICON_RAIN;
    case WX_SLEET:
    case WX_SNOW:
        return WX_ICON_SNOW;
    case WX_THUNDERSTORM:
    case WX_SEVERE:
        return WX_ICON_STORM;
    default:
        /* Nothing known: the sky is the least misleading thing to draw. */
        return night ? WX_ICON_MOON : WX_ICON_SUN;
    }
}

wx_icon_t weather_icon_for_moon(float phase)
{
    return (wx_icon_t)(WX_ICON_MOON_0 + wx_moon_octant(phase));
}
