/*
 * weather_aggregate.c - Four parts of a day out of an hourly forecast.
 */
#include "weather_aggregate.h"

#include <string.h>

/* An hour is charged to the window its midpoint falls in. */
#define HALF_HOUR 1800

void weather_agg_default_cfg(wx_agg_cfg_t *cfg)
{
    cfg->day_start = 0;
    cfg->noon = 0;
    cfg->day_end = 0;
    cfg->pop_threshold_pct = 25;
    cfg->strong_pop_pct = 60;
    cfg->units = WX_UNITS_IMPERIAL;
}

wx_status_t weather_day_bounds(time_t now, time_t *day_start, time_t *noon, time_t *day_end)
{
    struct tm local;

    if (localtime_r(&now, &local) == NULL) {
        return WX_ERR_BAD_ARG;
    }

    struct tm midnight = local;
    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;
    midnight.tm_isdst = -1;     /* let mktime work out whether DST applies */

    struct tm at_noon = midnight;
    at_noon.tm_hour = 12;
    at_noon.tm_isdst = -1;

    struct tm tomorrow = midnight;
    tomorrow.tm_mday += 1;      /* mktime normalises a day past the month end */
    tomorrow.tm_isdst = -1;

    *day_start = mktime(&midnight);
    *noon = mktime(&at_noon);
    *day_end = mktime(&tomorrow);

    if (*day_start == (time_t)-1 || *noon == (time_t)-1 || *day_end == (time_t)-1) {
        return WX_ERR_BAD_ARG;
    }
    return WX_OK;
}

/* Tallies for one window while the hourly array is walked. */
typedef struct {
    uint16_t samples;
    uint32_t clouds_sum;
    int32_t  temp_sum_x10;
    uint8_t  pop_max;
    uint8_t  hours[WX_COND_COUNT];
    uint8_t  pop_max_for[WX_COND_COUNT];
} bucket_t;

static bool is_wet(wx_cond_t cond)
{
    return cond >= WX_DRIZZLE;
}

/*
 * Which window an instant belongs to, or -1 if it falls outside the day.
 * The bounds are already clamped into order by the caller.
 */
static int window_of(time_t at, const time_t bound[5])
{
    for (int w = 0; w < WX_PART_COUNT; w++) {
        if (at >= bound[w] && at < bound[w + 1]) {
            return w;
        }
    }
    return -1;
}

/*
 * Settle on one condition for a window.
 *
 * Not a plain "worst wins": OpenWeather reports `500 light rain` for any hour
 * with a trace of modelled precipitation, including ones it only gives a six
 * percent chance. Ranking those straight up the ladder would paint most of the
 * year as rain and the icon would stop meaning anything. So hazards are taken
 * on a single hour, liquid precipitation has to either persist or be likely,
 * and everything else is decided on how cloudy the window was on average.
 */
static wx_cond_t reduce(const bucket_t *b, const wx_agg_cfg_t *cfg)
{
    if (b->samples == 0) {
        return WX_UNKNOWN;
    }

    /* Tier one: one hour of any of these is worth the whole window's icon. */
    static const wx_cond_t HAZARDS[] = {WX_SEVERE, WX_THUNDERSTORM, WX_SNOW, WX_SLEET};
    for (size_t i = 0; i < sizeof(HAZARDS) / sizeof(HAZARDS[0]); i++) {
        if (b->hours[HAZARDS[i]] > 0) {
            return HAZARDS[i];
        }
    }

    /* Tier two: rain has to last a quarter of the window, or be a good bet. */
    const int need = (b->samples / 4) > 2 ? (b->samples / 4) : 2;
    static const wx_cond_t WET[] = {WX_RAIN, WX_SHOWERS, WX_DRIZZLE};
    for (size_t i = 0; i < sizeof(WET) / sizeof(WET[0]); i++) {
        if (b->hours[WET[i]] >= need || b->pop_max_for[WET[i]] >= cfg->strong_pop_pct) {
            return WET[i];
        }
    }

    /* Fog only when it owns the window; dawn mist burns off and should not. */
    if (b->hours[WX_FOG] * 2 > b->samples) {
        return WX_FOG;
    }

    /*
     * Tier three, and the reason cloudiness is averaged rather than voted on:
     * the dry conditions are bins on one continuous number, so taking the mode
     * of already-binned values makes the icon flip on a single percent. These
     * are OpenWeather's own thresholds for the same four icons.
     */
    const unsigned mean = b->clouds_sum / b->samples;
    if (mean <= 10) {
        return WX_CLEAR;
    }
    if (mean <= 25) {
        return WX_FEW_CLOUDS;
    }
    if (mean <= 50) {
        return WX_PART_CLOUDY;
    }
    return WX_OVERCAST;
}

/* The daily entry describing the day the caller asked about. */
static int resolve_day(const wx_forecast_t *fc, const wx_agg_cfg_t *cfg)
{
    for (int i = 0; i < fc->n_days; i++) {
        if (fc->day[i].dt >= cfg->day_start && fc->day[i].dt < cfg->day_end) {
            return i;
        }
    }

    /*
     * No exact hit: the entries are stamped noon local to the location, which
     * need not be the timezone this clock is set to. Take the nearest.
     */
    int best = -1;
    time_t best_gap = 0;
    for (int i = 0; i < fc->n_days; i++) {
        const time_t gap = fc->day[i].dt > cfg->noon ? fc->day[i].dt - cfg->noon
                                                     : cfg->noon - fc->day[i].dt;
        if (best < 0 || gap < best_gap) {
            best = i;
            best_gap = gap;
        }
    }
    return best;
}

/* Keep the five boundaries in order however odd sunrise and sunset are. */
static time_t clamp(time_t value, time_t low, time_t high)
{
    if (value < low) {
        return low;
    }
    return (value > high) ? high : value;
}

/*
 * The worst condition anywhere in the day. Used only for a window with no
 * samples and no earlier poll to carry: it is a poor answer, but it is drawn
 * from the right day, which "the worst of everything in the response" would
 * not be when the response covers five of them.
 */
static wx_cond_t worst_in_day(const bucket_t *bucket)
{
    wx_cond_t worst = WX_UNKNOWN;

    for (int w = 0; w < WX_PART_COUNT; w++) {
        for (int c = WX_COND_COUNT - 1; c > (int)worst; c--) {
            if (bucket[w].hours[c] > 0) {
                worst = (wx_cond_t)c;
                break;
            }
        }
    }
    return worst;
}

static void fill_gaps(wx_report_t *out, const wx_report_t *prev, const wx_day_t *day,
                      const bucket_t *bucket)
{
    /* daily.weather when the source states one, otherwise what today holds. */
    const wx_cond_t fallback = (day->cond_id != 0) ? wx_cond_from_id(day->cond_id)
                                                   : worst_in_day(bucket);

    for (int w = 0; w < WX_PART_COUNT; w++) {
        if (out->part[w].src != WX_SRC_NONE) {
            continue;
        }

        /*
         * The hourly forecast starts at the current hour and carries no
         * history, so a window that has already passed can never be
         * aggregated. An earlier poll from the same day is the next best
         * thing -- which is why the last report is kept in NVS.
         */
        if (prev != NULL && prev->valid && prev->day_start == out->day_start &&
            prev->part[w].src != WX_SRC_NONE) {
            out->part[w].cond = prev->part[w].cond;
            out->part[w].pop_pct = prev->part[w].pop_pct;
            out->part[w].clouds_pct = prev->part[w].clouds_pct;
            out->part[w].src = WX_SRC_CARRIED;
            continue;
        }

        /* Otherwise the day's own condition, which is all that is left. */
        out->part[w].cond = fallback;
        out->part[w].src = (fallback == WX_UNKNOWN) ? WX_SRC_NONE : WX_SRC_DAILY;
    }
}

wx_status_t weather_aggregate(const wx_forecast_t *fc, const wx_agg_cfg_t *cfg,
                              const wx_report_t *prev, wx_report_t *out)
{
    if (fc == NULL || cfg == NULL || out == NULL) {
        return WX_ERR_BAD_ARG;
    }

    const int d = resolve_day(fc, cfg);
    if (d < 0) {
        return WX_ERR_NO_DATA;
    }
    const wx_day_t *day = &fc->day[d];

    memset(out, 0, sizeof(*out));
    out->units = cfg->units;
    out->day_start = cfg->day_start;
    out->moon_phase = day->moon_phase;
    out->sunrise = day->sunrise;
    out->sunset = day->sunset;
    out->sunrise_valid = (day->sunrise != 0);
    out->sunset_valid = (day->sunset != 0);

    /*
     * Boundaries, clamped into a monotone chain. Without the clamp a polar day
     * -- or simply a location whose sunrise lands after the local noon this
     * clock is set to -- would produce a window with negative length.
     */
    time_t bound[5];
    bound[0] = cfg->day_start;
    bound[4] = cfg->day_end;

    /* Substitute six and eighteen hundred when the sun neither rises nor sets. */
    const time_t sunrise = out->sunrise_valid ? day->sunrise : cfg->day_start + 6 * 3600;
    const time_t sunset  = out->sunset_valid ? day->sunset : cfg->day_start + 18 * 3600;

    bound[1] = clamp(sunrise, bound[0], bound[4]);
    bound[2] = clamp(cfg->noon, bound[1], bound[4]);
    bound[3] = clamp(sunset, bound[2], bound[4]);

    bucket_t bucket[WX_PART_COUNT];
    memset(bucket, 0, sizeof(bucket));

    for (int i = 0; i < fc->n_hours; i++) {
        const wx_hour_t *h = &fc->hour[i];
        const int w = window_of(h->dt + HALF_HOUR, bound);
        if (w < 0) {
            continue;
        }

        bucket_t *b = &bucket[w];
        b->samples++;
        b->clouds_sum += h->clouds_pct;
        b->temp_sum_x10 += h->temp_x10;
        if (h->pop_pct > b->pop_max) {
            b->pop_max = h->pop_pct;
        }

        const wx_cond_t cond = wx_cond_from_id(h->cond_id);
        if (is_wet(cond) && h->pop_pct < cfg->pop_threshold_pct) {
            /*
             * Rain the forecaster does not really believe in is just cloud.
             * The hour's cloudiness is already in the sum above, which is all
             * tier three looks at, so dropping it here is the whole job.
             */
            continue;
        }

        b->hours[cond]++;
        if (h->pop_pct > b->pop_max_for[cond]) {
            b->pop_max_for[cond] = h->pop_pct;
        }
    }

    for (int w = 0; w < WX_PART_COUNT; w++) {
        wx_window_t *part = &out->part[w];

        part->start = bound[w];
        part->end = bound[w + 1];
        part->samples = (uint8_t)(bucket[w].samples > 255 ? 255 : bucket[w].samples);
        part->pop_pct = bucket[w].pop_max;
        part->clouds_pct = bucket[w].samples
                               ? (uint8_t)(bucket[w].clouds_sum / bucket[w].samples)
                               : 0;
        part->cond = reduce(&bucket[w], cfg);
        part->src = (part->cond == WX_UNKNOWN) ? WX_SRC_NONE : WX_SRC_HOURLY;
    }

    fill_gaps(out, prev, day, bucket);

    if (day->temp_valid) {
        /* One Call states them; its four periods line up with these windows. */
        out->part[WX_NIGHT].temp_x10     = day->temp_night_x10;
        out->part[WX_MORNING].temp_x10   = day->temp_morn_x10;
        out->part[WX_AFTERNOON].temp_x10 = day->temp_day_x10;
        out->part[WX_EVENING].temp_x10   = day->temp_eve_x10;
        for (int w = 0; w < WX_PART_COUNT; w++) {
            out->part[w].temp_valid = true;
        }
    } else {
        /*
         * The free forecast gives no daily figures, so average the samples in
         * each window. Not a fallback so much as a better answer: a mean over
         * the window is by construction the temperature of the period drawn
         * next to it, where a stated "night" figure is whatever the API means
         * by night.
         */
        for (int w = 0; w < WX_PART_COUNT; w++) {
            if (bucket[w].samples > 0) {
                out->part[w].temp_x10 =
                    (int16_t)(bucket[w].temp_sum_x10 / bucket[w].samples);
                out->part[w].temp_valid = true;
            } else if (prev != NULL && prev->valid && prev->day_start == out->day_start &&
                       prev->part[w].temp_valid) {
                /* Same reasoning as the conditions: an earlier poll saw it. */
                out->part[w].temp_x10 = prev->part[w].temp_x10;
                out->part[w].temp_valid = true;
            }
        }
    }

    out->valid = true;
    return WX_OK;
}
