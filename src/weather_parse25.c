/*
 * weather_parse25.c - The free 2.5 forecast into the same arrays One Call fills.
 *
 * data/2.5/forecast is what an ordinary OpenWeather key can reach: forty
 * entries three hours apart over five days, plus the city's sunrise and sunset.
 * It carries no daily summary and no moon phase, so this fills in one synthetic
 * day entry -- sunrise and sunset from the city block, the phase worked out
 * locally, and no temperatures, which tells the aggregator to take each
 * window's temperature from the samples that fall in it.
 *
 * Deriving the temperatures that way is not a compromise. One Call's
 * temp.morn/day/eve/night are fixed to its own idea of those periods; a mean
 * over the window is by construction the temperature of the window drawn
 * beside it.
 */
#include "weather_parse25.h"

#include <string.h>

#include "cJSON.h"
#include "weather_moon.h"

static double number_or(const cJSON *node, double fallback)
{
    return cJSON_IsNumber(node) ? node->valuedouble : fallback;
}

static int16_t tenths(const cJSON *node)
{
    const double v = number_or(node, 0.0) * 10.0;
    return (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
}

static uint8_t clamp_pct(double v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    return (uint8_t)(v + 0.5);
}

wx_status_t weather_parse25(const char *body, size_t len, wx_forecast_t *out)
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
     * 2.5 always carries `cod`, as a string on success and a number on error,
     * so unlike One Call its presence means nothing. Only a non-200 value does.
     */
    const cJSON *cod = cJSON_GetObjectItemCaseSensitive(root, "cod");
    if (cJSON_IsNumber(cod) && cod->valuedouble != 200) {
        status = WX_ERR_REJECTED;
        goto done;
    }
    if (cJSON_IsString(cod) && strcmp(cod->valuestring, "200") != 0) {
        status = WX_ERR_REJECTED;
        goto done;
    }

    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    if (!cJSON_IsArray(list) || !cJSON_IsObject(city)) {
        status = WX_ERR_BAD_RESPONSE;
        goto done;
    }

    out->tz_offset_s =
        (int32_t)number_or(cJSON_GetObjectItemCaseSensitive(city, "timezone"), 0);

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, list) {
        if (out->n_hours >= WX_MAX_HOURLY) {
            break;
        }

        wx_hour_t *h = &out->hour[out->n_hours];
        h->dt = (time_t)number_or(cJSON_GetObjectItemCaseSensitive(entry, "dt"), 0);
        if (h->dt == 0) {
            continue;
        }

        const cJSON *main = cJSON_GetObjectItemCaseSensitive(entry, "main");
        h->temp_x10 = tenths(cJSON_GetObjectItemCaseSensitive(main, "temp"));

        h->pop_pct = clamp_pct(
            number_or(cJSON_GetObjectItemCaseSensitive(entry, "pop"), 0.0) * 100.0);

        const cJSON *clouds = cJSON_GetObjectItemCaseSensitive(entry, "clouds");
        h->clouds_pct =
            clamp_pct(number_or(cJSON_GetObjectItemCaseSensitive(clouds, "all"), 0.0));

        const cJSON *weather = cJSON_GetObjectItemCaseSensitive(entry, "weather");
        const cJSON *first = cJSON_GetArrayItem(weather, 0);
        h->cond_id =
            (uint16_t)number_or(cJSON_GetObjectItemCaseSensitive(first, "id"), 0);

        out->n_hours++;
    }

    if (out->n_hours == 0) {
        status = WX_ERR_NO_DATA;
        goto done;
    }

    /*
     * One synthetic day. Sunrise and sunset are the city's, which are today's
     * -- and today is the only day this screen draws.
     */
    wx_day_t *day = &out->day[0];
    day->sunrise = (time_t)number_or(cJSON_GetObjectItemCaseSensitive(city, "sunrise"), 0);
    day->sunset = (time_t)number_or(cJSON_GetObjectItemCaseSensitive(city, "sunset"), 0);
    day->dt = out->hour[0].dt;
    day->moon_phase = wx_moon_phase_at(day->dt);
    day->temp_valid = false;   /* derive per window from the samples */

    /*
     * No whole-day condition: 2.5 has no daily summary, and picking the worst
     * of the forty entries would let a storm four days out stand in for today.
     * The aggregator derives the fallback from the samples inside the day it is
     * actually drawing, which is knowledge only it has.
     */
    day->cond_id = 0;

    out->n_days = 1;

done:
    cJSON_Delete(root);
    return status;
}
