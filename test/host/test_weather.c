/*
 * test_weather.c - Host tests for the parts of the weather feature that have no
 * business touching a panel: parsing a One Call response and reducing it to
 * four parts of a day.
 *
 * Built and run by tools/run_host_tests.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weather_aggregate.h"
#include "weather_daily.h"
#include "weather_moon.h"
#include "weather_parse.h"
#include "weather_parse25.h"
#include "weather_types.h"

/* 2026-07-15 00:00 EDT, matching tools/make_weather_fixtures.py. */
#define DAY_START 1784088000
#define HOUR 3600

static int failures;

static void ok(const char *what, bool cond)
{
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) {
        failures++;
    }
}

static void eq_cond(const char *what, wx_cond_t got, wx_cond_t want)
{
    if (got != want) {
        printf("  FAIL %-46s got %s want %s\n", what, wx_cond_label(got),
               wx_cond_label(want));
        failures++;
    } else {
        printf("  ok   %-46s %s\n", what, wx_cond_label(got));
    }
}

static char *slurp(const char *name, size_t *len)
{
    char path[256];
    snprintf(path, sizeof(path), "test/fixtures/%s", name);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }

    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(*len + 1);
    if (fread(buf, 1, *len, f) != *len) {
        exit(2);
    }
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

/* Parse a fixture and reduce it against a stated day. */
static wx_status_t run(const char *fixture, const wx_report_t *prev, wx_report_t *out)
{
    size_t len;
    char *body = slurp(fixture, &len);

    static wx_forecast_t fc;
    wx_status_t st = weather_parse(body, len, &fc);
    free(body);
    if (st != WX_OK) {
        return st;
    }

    wx_agg_cfg_t cfg;
    weather_agg_default_cfg(&cfg);
    cfg.day_start = DAY_START;
    cfg.noon = DAY_START + 12 * HOUR;
    cfg.day_end = DAY_START + 24 * HOUR;

    return weather_aggregate(&fc, &cfg, prev, out);
}

int main(void)
{
    wx_report_t r;

    puts("condition classification");
    eq_cond("id 211 thunderstorm", wx_cond_from_id(211), WX_THUNDERSTORM);
    eq_cond("id 511 freezing rain is sleet, not rain",
            wx_cond_from_id(511), WX_SLEET);
    eq_cond("id 500 light rain", wx_cond_from_id(500), WX_RAIN);
    eq_cond("id 521 shower", wx_cond_from_id(521), WX_SHOWERS);
    eq_cond("id 781 tornado is severe, not fog", wx_cond_from_id(781), WX_SEVERE);
    eq_cond("id 741 fog", wx_cond_from_id(741), WX_FOG);
    eq_cond("id 804 overcast", wx_cond_from_id(804), WX_OVERCAST);
    eq_cond("id 0 unknown", wx_cond_from_id(0), WX_UNKNOWN);

    puts("baseline day");
    ok("parses and aggregates", run("onecall_clear.json", NULL, &r) == WX_OK);
    eq_cond("night is clear", r.part[WX_NIGHT].cond, WX_CLEAR);
    eq_cond("morning is clear", r.part[WX_MORNING].cond, WX_CLEAR);
    eq_cond("one storm hour claims the afternoon",
            r.part[WX_AFTERNOON].cond, WX_THUNDERSTORM);
    eq_cond("evening is overcast", r.part[WX_EVENING].cond, WX_OVERCAST);
    ok("temperatures came through", r.part[WX_AFTERNOON].temp_x10 == 260);
    ok("morning temperature is the morning one", r.part[WX_MORNING].temp_x10 == 160);
    ok("sunrise is flagged valid", r.sunrise_valid);
    ok("every window was aggregated, not filled",
       r.part[WX_MORNING].src == WX_SRC_HOURLY &&
       r.part[WX_EVENING].src == WX_SRC_HOURLY);

    puts("windows follow sunrise and sunset");
    ok("morning starts at sunrise",
       r.part[WX_MORNING].start == DAY_START + 5 * HOUR + 45 * 60);
    ok("afternoon starts at noon", r.part[WX_AFTERNOON].start == DAY_START + 12 * HOUR);
    ok("evening starts at sunset",
       r.part[WX_EVENING].start == DAY_START + 20 * HOUR + 30 * 60);
    ok("evening ends at midnight", r.part[WX_EVENING].end == DAY_START + 24 * HOUR);

    puts("low-confidence rain is not rain");
    ok("parses", run("onecall_drizzle.json", NULL, &r) == WX_OK);
    ok("four hours of 8%-chance light rain do not read as rain",
       r.part[WX_AFTERNOON].cond < WX_DRIZZLE);
    printf("       (read as %s)\n", wx_cond_label(r.part[WX_AFTERNOON].cond));

    puts("booted after dark");
    ok("parses", run("onecall_evening.json", NULL, &r) == WX_OK);
    ok("morning has no samples", r.part[WX_MORNING].samples == 0);
    ok("morning falls back to the daily condition",
       r.part[WX_MORNING].src == WX_SRC_DAILY);
    eq_cond("and that condition is the day's",
            r.part[WX_MORNING].cond, WX_PART_CLOUDY);

    puts("booted after dark, with this morning's earlier poll to hand");
    wx_report_t prev;
    ok("baseline reparses", run("onecall_clear.json", NULL, &prev) == WX_OK);
    ok("parses", run("onecall_evening.json", &prev, &r) == WX_OK);
    ok("morning is carried from the earlier poll",
       r.part[WX_MORNING].src == WX_SRC_CARRIED);
    eq_cond("and keeps what that poll found", r.part[WX_MORNING].cond, WX_CLEAR);

    prev.day_start = DAY_START - 24 * HOUR;      /* yesterday's report */
    ok("parses", run("onecall_evening.json", &prev, &r) == WX_OK);
    ok("a stale report is not carried into today",
       r.part[WX_MORNING].src == WX_SRC_DAILY);

    puts("polar day");
    ok("parses", run("onecall_polar.json", NULL, &r) == WX_OK);
    ok("sunrise is flagged invalid", !r.sunrise_valid && !r.sunset_valid);
    ok("windows stay in order",
       r.part[WX_NIGHT].start <= r.part[WX_MORNING].start &&
       r.part[WX_MORNING].start <= r.part[WX_AFTERNOON].start &&
       r.part[WX_AFTERNOON].start <= r.part[WX_EVENING].start &&
       r.part[WX_EVENING].end == DAY_START + 24 * HOUR);

    puts("server errors");
    ok("a 401 body is told apart from a malformed one",
       run("onecall_err_401.json", NULL, &r) == WX_ERR_REJECTED);
    ok("garbage is malformed",
       weather_parse("{not json", 9, &(wx_forecast_t){0}) == WX_ERR_BAD_RESPONSE);

    puts("redraw suppression");
    ok("baseline", run("onecall_clear.json", NULL, &r) == WX_OK);
    ok("a report equals itself", wx_report_equal(&r, &r));
    wx_report_t other = r;
    other.fetched_at += 1800;
    ok("a later poll of the same forecast is still equal",
       wx_report_equal(&r, &other));
    other.part[WX_MORNING].cond = WX_SNOW;
    ok("a changed condition is not equal", !wx_report_equal(&r, &other));

    puts("moon phase, worked out locally");
    /* 2024-01-11 11:57 UTC was a new moon; 2024-01-25 17:54 UTC a full one. */
    ok("a known new moon reads as new",
       wx_moon_octant(wx_moon_phase_at(1704974220)) == 0);
    ok("a known full moon reads as full",
       wx_moon_octant(wx_moon_phase_at(1706205240)) == 4);
    ok("phase stays in range a century out",
       wx_moon_phase_at(4102444800) >= 0.0f && wx_moon_phase_at(4102444800) <= 1.0f);

    puts("the free 2.5 forecast, from a live capture");
    {
        size_t len;
        char *body = slurp("forecast25_live.json", &len);
        static wx_forecast_t fc;
        ok("parses", weather_parse25(body, len, &fc) == WX_OK);
        free(body);

        ok("three-hourly samples came through", fc.n_hours >= 30);
        ok("one synthetic day was built", fc.n_days == 1);
        ok("sunrise and sunset came from the city block",
           fc.day[0].sunrise > 0 && fc.day[0].sunset > fc.day[0].sunrise);
        ok("no daily temperatures, so the aggregator derives them",
           !fc.day[0].temp_valid);
        ok("a moon phase was worked out",
           fc.day[0].moon_phase >= 0.0f && fc.day[0].moon_phase <= 1.0f);

        /* Aggregate against the day the capture actually covers. */
        wx_agg_cfg_t cfg;
        weather_agg_default_cfg(&cfg);
        struct tm lt;
        time_t t0 = fc.hour[0].dt;
        localtime_r(&t0, &lt);
        lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
        cfg.day_start = mktime(&lt);
        lt.tm_hour = 12; lt.tm_isdst = -1;
        cfg.noon = mktime(&lt);
        cfg.day_end = cfg.day_start + 24 * 3600;

        wx_report_t live;
        ok("aggregates", weather_aggregate(&fc, &cfg, NULL, &live) == WX_OK);

        int with_temp = 0;
        for (int w = 0; w < WX_PART_COUNT; w++) {
            if (live.part[w].temp_valid) {
                with_temp++;
            }
        }
        ok("temperatures were derived per window", with_temp > 0);
        printf("       live: ");
        for (int w = 0; w < WX_PART_COUNT; w++) {
            printf("%s=%s/%d%s  ", wx_part_label(w), wx_cond_label(live.part[w].cond),
                   live.part[w].temp_x10 / 10,
                   live.part[w].src == WX_SRC_HOURLY ? "" : "*");
        }
        printf("\n");
    }

    /* ---- weather_daily: the two-day screen's arithmetic ------------------ */
    {
        /*
         * A hand-built day: four three-hourly samples, chosen so every
         * reduction can be checked by hand rather than against itself.
         *
         *   temp_min  2.0  1.0  3.0  4.0  -> lowest  1.0
         *   temp_max  8.0  9.0 12.0  7.0  -> highest 12.0
         *   humidity   60   70   80   90  -> mean 75
         *   pop        10   90   20   30  -> worst 90, not the mean
         *   gust      5.0  9.0  3.0  1.0  -> strongest 9.0
         *   clouds     20   40   60   80  -> mean 50
         *   precip    0.5  1.25 0.0  0.25 -> total 2.0
         */
        static const struct { int16_t lo, hi; uint8_t rh, pop, cl; uint16_t g, p; } S[] = {
            {20, 80, 60, 10, 20, 50, 50}, {10, 90, 70, 90, 40, 90, 125},
            {30, 120, 80, 20, 60, 30, 0}, {40, 70, 90, 30, 80, 10, 25},
        };

        wx_forecast_t fc;
        memset(&fc, 0, sizeof(fc));
        const time_t day = 1700000000;   /* arbitrary; only the span matters */

        for (int i = 0; i < 4; i++) {
            wx_hour_t *h = &fc.hour[i];
            h->dt = day + i * 3 * 3600;
            h->temp_min_x10 = S[i].lo;
            h->temp_max_x10 = S[i].hi;
            h->humidity_pct = S[i].rh;
            h->pop_pct = S[i].pop;
            h->clouds_pct = S[i].cl;
            h->gust_x10 = S[i].g;
            h->precip_x100 = S[i].p;
            h->cond_id = 500;            /* light rain throughout */
        }
        fc.n_hours = 4;

        wx_daysum_t d;
        ok("a day reduces", weather_day_summary(&fc, day, day + 86400, &d) == WX_OK);
        ok("every sample counted", d.samples == 4);
        ok("low is the lowest min", d.temp_min_x10 == 10);
        ok("high is the highest max", d.temp_max_x10 == 120);
        ok("humidity is the mean", d.humidity_pct == 75);
        ok("cloud is the mean", d.clouds_pct == 50);
        ok("pop is the worst hour, not the mean", d.pop_pct == 90);
        ok("gust is the strongest", d.gust_x10 == 90);
        ok("precipitation is the total", d.precip_x100 == 200);

        /* A window outside the samples is empty, which is what a late evening
         * looks like -- ordinary, and it must not be mistaken for an error. */
        wx_daysum_t gap;
        ok("an empty window says so",
           weather_day_summary(&fc, day + 86400, day + 2 * 86400, &gap) == WX_ERR_NO_DATA);
        ok("and leaves nothing to draw", !gap.valid);

        /*
         * Severity, not frequency, picks the glyph: three calm hours must not
         * bury one thunderstorm. But within the worst rank, the commonest
         * wording wins, so a lone "heavy" hour does not retitle the day.
         */
        fc.hour[1].cond_id = 201;    /* thunderstorm with rain */
        fc.hour[2].cond_id = 202;    /* ... and one heavier hour */
        fc.hour[3].cond_id = 201;
        wx_daysum_t storm;
        weather_day_summary(&fc, day, day + 86400, &storm);
        ok("the worst condition of the day wins", storm.cond_id == 201);

        ok("descriptions come back in capitals",
           strcmp(wx_cond_description(803), "BROKEN CLOUDS") == 0);
        ok("and cover the whole published table",
           strcmp(wx_cond_description(781), "TORNADO") == 0 &&
           strcmp(wx_cond_description(500), "LIGHT RAIN") == 0);
        ok("an unlisted id still says something",
           wx_cond_description(999) != NULL && wx_cond_description(999)[0] != '\0');

        /* The comparison is what decides whether a refresh is spent. */
        wx_daysum_t a = d, b = d;
        ok("identical days compare equal", wx_daysum_equal(&a, &b));
        b.samples = 1;
        ok("a changed sample count alone is not a redraw", wx_daysum_equal(&a, &b));
        b.pop_pct = 1;
        ok("a changed reading is", !wx_daysum_equal(&a, &b));
    }

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
