/*
 * weather_parse.c - One Call 3.0 JSON into plain arrays. See the header.
 */
#include "weather_parse.h"

#include <string.h>

#include "cJSON.h"

const char *wx_status_str(wx_status_t status)
{
    switch (status) {
    case WX_OK:               return "ok";
    case WX_ERR_BAD_ARG:      return "bad argument";
    case WX_ERR_BAD_RESPONSE: return "malformed response";
    case WX_ERR_REJECTED:     return "rejected by the server";
    case WX_ERR_NO_MEM:       return "out of memory";
    case WX_ERR_NO_DATA:      return "no usable data";
    default:                  return "unknown";
    }
}

/* Temperatures are carried as tenths so the report is fixed-size and compares exactly. */
static int16_t tenths(const cJSON *node, bool *ok)
{
    if (!cJSON_IsNumber(node)) {
        if (ok != NULL) {
            *ok = false;
        }
        return 0;
    }

    const double v = node->valuedouble * 10.0;
    return (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
}

static time_t epoch_of(const cJSON *node)
{
    return cJSON_IsNumber(node) ? (time_t)node->valuedouble : (time_t)0;
}

/* pop arrives as 0..1; percent is what the screen wants. */
static uint8_t percent_of_fraction(const cJSON *node)
{
    if (!cJSON_IsNumber(node)) {
        return 0;
    }

    double v = node->valuedouble * 100.0 + 0.5;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    return (uint8_t)v;
}

static uint8_t percent_of(const cJSON *node)
{
    if (!cJSON_IsNumber(node)) {
        return 0;
    }

    double v = node->valuedouble;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    return (uint8_t)(v + 0.5);
}

/* The id of the first entry in a `weather` array; 0 when there is not one. */
static uint16_t condition_id(const cJSON *entry)
{
    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(entry, "weather");
    if (!cJSON_IsArray(weather)) {
        return 0;
    }

    const cJSON *first = cJSON_GetArrayItem(weather, 0);
    if (first == NULL) {
        return 0;
    }

    /* The first entry is the primary one; later ones are secondary phenomena. */
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(first, "id");
    return cJSON_IsNumber(id) ? (uint16_t)id->valuedouble : 0;
}

static void parse_hourly(const cJSON *hourly, wx_forecast_t *out)
{
    const cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, hourly) {
        if (out->n_hours >= WX_MAX_HOURLY) {
            break;
        }

        wx_hour_t *h = &out->hour[out->n_hours];
        h->dt = epoch_of(cJSON_GetObjectItemCaseSensitive(entry, "dt"));
        if (h->dt == 0) {
            continue;   /* no timestamp, nothing to bucket it by */
        }

        h->temp_x10   = tenths(cJSON_GetObjectItemCaseSensitive(entry, "temp"), NULL);
        h->pop_pct    = percent_of_fraction(cJSON_GetObjectItemCaseSensitive(entry, "pop"));
        h->clouds_pct = percent_of(cJSON_GetObjectItemCaseSensitive(entry, "clouds"));
        h->cond_id    = condition_id(entry);
        out->n_hours++;
    }
}

static void parse_daily(const cJSON *daily, wx_forecast_t *out)
{
    const cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, daily) {
        if (out->n_days >= WX_MAX_DAILY) {
            break;
        }

        wx_day_t *d = &out->day[out->n_days];
        d->dt = epoch_of(cJSON_GetObjectItemCaseSensitive(entry, "dt"));
        if (d->dt == 0) {
            continue;
        }

        /* Absent through a polar day or night, and the caller must cope. */
        d->sunrise = epoch_of(cJSON_GetObjectItemCaseSensitive(entry, "sunrise"));
        d->sunset  = epoch_of(cJSON_GetObjectItemCaseSensitive(entry, "sunset"));

        const cJSON *moon = cJSON_GetObjectItemCaseSensitive(entry, "moon_phase");
        d->moon_phase = cJSON_IsNumber(moon) ? (float)moon->valuedouble : 0.0f;
        d->cond_id = condition_id(entry);

        bool ok = true;
        const cJSON *temp = cJSON_GetObjectItemCaseSensitive(entry, "temp");
        d->temp_morn_x10  = tenths(cJSON_GetObjectItemCaseSensitive(temp, "morn"), &ok);
        d->temp_day_x10   = tenths(cJSON_GetObjectItemCaseSensitive(temp, "day"), &ok);
        d->temp_eve_x10   = tenths(cJSON_GetObjectItemCaseSensitive(temp, "eve"), &ok);
        d->temp_night_x10 = tenths(cJSON_GetObjectItemCaseSensitive(temp, "night"), &ok);
        d->temp_valid = ok;

        out->n_days++;
    }
}

wx_status_t weather_parse(const char *body, size_t len, wx_forecast_t *out)
{
    if (body == NULL || out == NULL) {
        return WX_ERR_BAD_ARG;
    }

    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithLength(body, len);
    if (root == NULL) {
        return WX_ERR_BAD_RESPONSE;
    }

    wx_status_t status = WX_OK;

    /*
     * An error reply is a small object carrying `cod` and `message` rather than
     * the forecast. Telling it apart is worth the few lines: it means the key or
     * the subscription is wrong, which no amount of retrying will fix.
     */
    const cJSON *cod = cJSON_GetObjectItemCaseSensitive(root, "cod");
    if (cod != NULL) {
        status = WX_ERR_REJECTED;
        goto done;
    }

    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone_offset");
    out->tz_offset_s = cJSON_IsNumber(tz) ? (int32_t)tz->valuedouble : 0;

    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *daily  = cJSON_GetObjectItemCaseSensitive(root, "daily");

    if (!cJSON_IsArray(hourly) && !cJSON_IsArray(daily)) {
        status = WX_ERR_BAD_RESPONSE;
        goto done;
    }

    if (cJSON_IsArray(hourly)) {
        parse_hourly(hourly, out);
    }
    if (cJSON_IsArray(daily)) {
        parse_daily(daily, out);
    }

    /* The day carries sunrise, sunset, the moon and every temperature. */
    if (out->n_days == 0) {
        status = WX_ERR_NO_DATA;
    }

done:
    cJSON_Delete(root);
    return status;
}
