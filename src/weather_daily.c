/*
 * weather_daily.c - Three-hourly samples reduced to a day apiece. See header.
 */
#include "weather_daily.h"

#include <string.h>

wx_status_t weather_day_summary(const wx_forecast_t *fc, time_t day_start,
                                time_t day_end, wx_daysum_t *out)
{
    if (fc == NULL || out == NULL || day_end <= day_start) {
        return WX_ERR_BAD_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    out->day_start = day_start;

    uint32_t humidity_sum = 0;
    uint32_t clouds_sum = 0;
    uint32_t precip_sum = 0;
    int16_t lo = 0, hi = 0;
    unsigned n = 0;

    /*
     * Severity decides the day's glyph and wording -- an afternoon of
     * thunderstorms is what the day was, however many calm hours surrounded
     * it. Among the samples that share the worst severity, the commonest exact
     * id wins, so "moderate rain" is not displaced by the one hour of "heavy
     * intensity rain" that happened to share its rank.
     */
    wx_cond_t worst = WX_CLEAR;
    uint16_t id_tally[WX_MAX_HOURLY];
    uint8_t id_count[WX_MAX_HOURLY];
    unsigned distinct = 0;

    for (unsigned i = 0; i < fc->n_hours; i++) {
        const wx_hour_t *h = &fc->hour[i];

        if (h->dt < day_start || h->dt >= day_end) {
            continue;
        }

        if (n == 0) {
            lo = h->temp_min_x10;
            hi = h->temp_max_x10;
        } else {
            if (h->temp_min_x10 < lo) {
                lo = h->temp_min_x10;
            }
            if (h->temp_max_x10 > hi) {
                hi = h->temp_max_x10;
            }
        }

        humidity_sum += h->humidity_pct;
        clouds_sum += h->clouds_pct;
        precip_sum += h->precip_x100;

        if (h->pop_pct > out->pop_pct) {
            out->pop_pct = h->pop_pct;
        }
        if (h->gust_x10 > out->gust_x10) {
            out->gust_x10 = h->gust_x10;
        }

        const wx_cond_t cond = wx_cond_from_id(h->cond_id);
        if ((int)cond > (int)worst) {
            worst = cond;
            distinct = 0;          /* a worse day: start the tally again */
        }
        if (cond == worst) {
            unsigned k = 0;
            while (k < distinct && id_tally[k] != h->cond_id) {
                k++;
            }
            if (k == distinct && distinct < WX_MAX_HOURLY) {
                id_tally[distinct] = h->cond_id;
                id_count[distinct] = 0;
                distinct++;
            }
            if (k < WX_MAX_HOURLY) {
                id_count[k]++;
            }
        }

        n++;
    }

    if (n == 0) {
        return WX_ERR_NO_DATA;
    }

    unsigned best = 0;
    for (unsigned k = 1; k < distinct; k++) {
        if (id_count[k] > id_count[best]) {
            best = k;
        }
    }
    out->cond_id = (distinct > 0) ? id_tally[best] : 0;

    out->samples = (uint8_t)n;
    out->temp_min_x10 = lo;
    out->temp_max_x10 = hi;
    out->humidity_pct = (uint8_t)((humidity_sum + n / 2) / n);
    out->clouds_pct = (uint8_t)((clouds_sum + n / 2) / n);
    out->precip_x100 = (precip_sum > UINT16_MAX) ? UINT16_MAX : (uint16_t)precip_sum;
    out->valid = true;

    return WX_OK;
}

bool wx_daysum_equal(const wx_daysum_t *a, const wx_daysum_t *b)
{
    if (a->valid != b->valid) {
        return false;
    }
    if (!a->valid) {
        return true;
    }

    /* Everything the screen draws, and nothing it does not: `samples` moves on
     * most polls without changing a single pixel. */
    return a->day_start == b->day_start && a->cond_id == b->cond_id &&
           a->temp_min_x10 == b->temp_min_x10 && a->temp_max_x10 == b->temp_max_x10 &&
           a->humidity_pct == b->humidity_pct && a->pop_pct == b->pop_pct &&
           a->gust_x10 == b->gust_x10 && a->clouds_pct == b->clouds_pct &&
           a->precip_x100 == b->precip_x100;
}

/*
 * OpenWeather's published condition table, in capitals. Sorted by id so the
 * lookup can binary-search, and complete so that it never has to guess.
 */
static const struct {
    uint16_t id;
    const char *text;
} DESCRIPTIONS[] = {
    {200, "THUNDERSTORM WITH LIGHT RAIN"},
    {201, "THUNDERSTORM WITH RAIN"},
    {202, "THUNDERSTORM WITH HEAVY RAIN"},
    {210, "LIGHT THUNDERSTORM"},
    {211, "THUNDERSTORM"},
    {212, "HEAVY THUNDERSTORM"},
    {221, "RAGGED THUNDERSTORM"},
    {230, "THUNDERSTORM WITH LIGHT DRIZZLE"},
    {231, "THUNDERSTORM WITH DRIZZLE"},
    {232, "THUNDERSTORM WITH HEAVY DRIZZLE"},
    {300, "LIGHT INTENSITY DRIZZLE"},
    {301, "DRIZZLE"},
    {302, "HEAVY INTENSITY DRIZZLE"},
    {310, "LIGHT INTENSITY DRIZZLE RAIN"},
    {311, "DRIZZLE RAIN"},
    {312, "HEAVY INTENSITY DRIZZLE RAIN"},
    {313, "SHOWER RAIN AND DRIZZLE"},
    {314, "HEAVY SHOWER RAIN AND DRIZZLE"},
    {321, "SHOWER DRIZZLE"},
    {500, "LIGHT RAIN"},
    {501, "MODERATE RAIN"},
    {502, "HEAVY INTENSITY RAIN"},
    {503, "VERY HEAVY RAIN"},
    {504, "EXTREME RAIN"},
    {511, "FREEZING RAIN"},
    {520, "LIGHT INTENSITY SHOWER RAIN"},
    {521, "SHOWER RAIN"},
    {522, "HEAVY INTENSITY SHOWER RAIN"},
    {531, "RAGGED SHOWER RAIN"},
    {600, "LIGHT SNOW"},
    {601, "SNOW"},
    {602, "HEAVY SNOW"},
    {611, "SLEET"},
    {612, "LIGHT SHOWER SLEET"},
    {613, "SHOWER SLEET"},
    {615, "LIGHT RAIN AND SNOW"},
    {616, "RAIN AND SNOW"},
    {620, "LIGHT SHOWER SNOW"},
    {621, "SHOWER SNOW"},
    {622, "HEAVY SHOWER SNOW"},
    {701, "MIST"},
    {711, "SMOKE"},
    {721, "HAZE"},
    {731, "SAND/DUST WHIRLS"},
    {741, "FOG"},
    {751, "SAND"},
    {761, "DUST"},
    {762, "VOLCANIC ASH"},
    {771, "SQUALLS"},
    {781, "TORNADO"},
    {800, "CLEAR SKY"},
    {801, "FEW CLOUDS"},
    {802, "SCATTERED CLOUDS"},
    {803, "BROKEN CLOUDS"},
    {804, "OVERCAST CLOUDS"},
};

const char *wx_cond_description(uint16_t cond_id)
{
    unsigned lo = 0;
    unsigned hi = sizeof(DESCRIPTIONS) / sizeof(DESCRIPTIONS[0]);

    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;

        if (DESCRIPTIONS[mid].id == cond_id) {
            return DESCRIPTIONS[mid].text;
        }
        if (DESCRIPTIONS[mid].id < cond_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    /*
     * An id outside the table. Falling back on the project's own coarser
     * vocabulary beats printing nothing: it is still true, just less specific.
     */
    return wx_cond_label(wx_cond_from_id(cond_id));
}
