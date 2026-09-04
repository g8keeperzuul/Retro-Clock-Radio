/*
 * weather_moon.h - Moon phase, worked out locally.
 *
 * One Call reports the phase; the free 2.5 forecast does not, and the screen
 * wants it either way. The moon is regular enough that a date is all it takes.
 */
#pragma once

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Phase at `when` on the same scale One Call uses: 0 and 1 are new, 0.25 the
 * first quarter, 0.5 full, 0.75 the last quarter.
 *
 * A mean-phase calculation, so it ignores the orbit's eccentricity and can be
 * most of a day out around the quarters. The screen picks one of eight glyphs
 * from it, each covering about three and a half days, so that error never
 * shows.
 */
float wx_moon_phase_at(time_t when);

#ifdef __cplusplus
}
#endif
