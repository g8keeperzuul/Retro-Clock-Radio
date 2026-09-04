/*
 * weather_moon.c - Moon phase from a date. See the header.
 */
#include "weather_moon.h"

#include <math.h>

/*
 * A known new moon, and the mean interval between them. 2000-01-06 18:14 UTC
 * is the standard epoch for this; the synodic month is the long-run average,
 * which is why this is a mean phase rather than a true one.
 */
#define NEW_MOON_EPOCH  947182440.0   /* 2000-01-06 18:14 UTC */
#define SYNODIC_MONTH_S (29.530588853 * 86400.0)

float wx_moon_phase_at(time_t when)
{
    double cycles = ((double)when - NEW_MOON_EPOCH) / SYNODIC_MONTH_S;

    /* Keep the fraction in 0..1 for dates before the epoch as well as after. */
    cycles -= floor(cycles);
    if (cycles < 0.0) {
        cycles += 1.0;
    }
    return (float)cycles;
}
