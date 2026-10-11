/*
 * weather_api.c - Fetching the One Call response. See the header.
 */
#include "weather_api.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"
#include "weather_mock_data.h"

static const char *TAG = "wx-api";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef WEATHER_MOCK
#define WEATHER_MOCK 0
#endif

#if !WEATHER_MOCK
#ifndef OWM_API_KEY
#error "OWM_API_KEY is not defined. Add -D OWM_API_KEY=\\\"your-key\\\" to build_flags in platformio.ini"
#endif
#ifndef OWM_LATITUDE
#error "OWM_LATITUDE is not defined. Add -D OWM_LATITUDE=\\\"43.6532\\\" to build_flags in platformio.ini"
#endif
#ifndef OWM_LONGITUDE
#error "OWM_LONGITUDE is not defined. Add -D OWM_LONGITUDE=\\\"-79.3832\\\" to build_flags in platformio.ini"
#endif
#endif

#ifndef OWM_UNITS
#define OWM_UNITS "imperial"
#endif
/*
 * The forecast endpoint.
 *
 * data/2.5/forecast is what an ordinary key can reach: forty entries three
 * hours apart over five days, with the city's sunrise and sunset. One Call --
 * data/3.0/onecall, hourly and richer -- needs its own "One Call by Call"
 * subscription and answers 401 without it, whichever version number is in the
 * path. weather_parse.c still handles that response, and its tests still run,
 * for whenever the subscription exists.
 */
#ifndef OWM_FORECAST_PATH
#define OWM_FORECAST_PATH "/data/2.5/forecast"
#endif
#ifndef OWM_HTTP_TIMEOUT_MS
#define OWM_HTTP_TIMEOUT_MS 15000
#endif

/*
 * A five-day 2.5 forecast runs to about 18 KB. The ceiling is not tuning -- it
 * is what stops a runaway or hostile response from eating the heap.
 */
#define BODY_CEILING (128 * 1024)
#define BODY_INITIAL (32 * 1024)

/*
 * The URL is built per fetch rather than concatenated at compile time: the
 * coordinates and the key can all be changed over MQTT now, and a macro would
 * freeze whatever was flashed. Long enough for a 64-character key and a pair
 * of full-precision coordinates.
 */
#define URL_MAX 256

static void build_url(char *out, size_t len)
{
    snprintf(out, len,
             "https://api.openweathermap.org" OWM_FORECAST_PATH
             "?lat=%s&lon=%s&units=" OWM_UNITS "&appid=%s",
             settings_get(SETTING_LATITUDE), settings_get(SETTING_LONGITUDE),
             settings_get(SETTING_API_KEY));
}

static void *body_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return (p != NULL) ? p : malloc(size);
}

void weather_api_release(char *body)
{
    free(body);
}

#if WEATHER_MOCK

esp_err_t weather_api_fetch(char **body, size_t *len)
{
    /*
     * The one place the mock differs from the real thing. Everything
     * downstream -- parse, aggregate, render, schedule -- runs identically, so
     * what is exercised here is the whole path bar the socket.
     */
    const size_t size = sizeof(WEATHER_MOCK_JSON) - 1;

    char *copy = body_alloc(size + 1);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(copy, WEATHER_MOCK_JSON, size);
    copy[size] = '\0';
    *body = copy;
    *len = size;

    ESP_LOGW(TAG, "WEATHER_MOCK is set: serving a canned %u byte response",
             (unsigned)size);
    return ESP_OK;
}

#else

esp_err_t weather_api_fetch(char **body, size_t *len)
{
    char url[URL_MAX];
    build_url(url, sizeof(url));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OWM_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,     /* the 512 byte default is 80 reads for this body */
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,  /* polls are half an hour apart */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Never log cfg.url: the API key is in it. */
    ESP_LOGI(TAG, "fetching the forecast");

    char *buf = NULL;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        goto done;
    }

    const int64_t announced = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        /*
         * Log what the server actually said. OpenWeather explains itself in the
         * body -- an unsubscribed key returns 401 with "requires a separate
         * subscription to the One Call by Call plan", which is the difference
         * between a five-second fix and an afternoon of guessing. The body
         * carries no credentials, unlike the URL, so it is safe to print.
         */
        char reason[192];
        const int n = esp_http_client_read_response(client, reason, sizeof(reason) - 1);
        reason[n > 0 ? n : 0] = '\0';

        ESP_LOGE(TAG, "server returned %d: %s", status, n > 0 ? reason : "(no body)");

        /* 401 and 403 are the key or the plan; neither improves on retry. */
        err = (status == 401 || status == 403) ? ESP_ERR_NOT_ALLOWED : ESP_FAIL;
        goto done;
    }

    size_t cap = (announced > 0 && announced < BODY_CEILING)
                     ? (size_t)announced + 1
                     : BODY_INITIAL;
    buf = body_alloc(cap);
    if (buf == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t used = 0;
    while (true) {
        if (used + 1 >= cap) {
            /* Chunked, so no length was announced: grow until the ceiling. */
            if (cap >= BODY_CEILING) {
                ESP_LOGE(TAG, "response exceeded %d bytes", BODY_CEILING);
                err = ESP_ERR_INVALID_SIZE;
                goto done;
            }
            const size_t bigger = (cap * 2 > BODY_CEILING) ? BODY_CEILING : cap * 2;
            char *grown = body_alloc(bigger);
            if (grown == NULL) {
                err = ESP_ERR_NO_MEM;
                goto done;
            }
            memcpy(grown, buf, used);
            free(buf);
            buf = grown;
            cap = bigger;
        }

        const int n = esp_http_client_read(client, buf + used, cap - used - 1);
        if (n < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }

    if (!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGE(TAG, "connection closed after %u of %lld bytes",
                 (unsigned)used, (long long)announced);
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    buf[used] = '\0';
    *body = buf;
    *len = used;
    buf = NULL;                  /* handed to the caller */
    err = ESP_OK;

    ESP_LOGI(TAG, "received %u bytes", (unsigned)used);

done:
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

#endif /* WEATHER_MOCK */
