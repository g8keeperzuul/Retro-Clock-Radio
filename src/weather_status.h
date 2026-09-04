/*
 * weather_status.h - Result codes for the parts of the weather feature that
 * have to build on a host as well as on the panel.
 *
 * ESP-IDF's esp_err_t would drag in sdkconfig.h, which does not exist off the
 * target, so the pure modules speak this instead and the IDF-facing ones map it
 * at the boundary.
 */
#pragma once

typedef enum {
    WX_OK = 0,
    WX_ERR_BAD_ARG,
    WX_ERR_BAD_RESPONSE,   /* malformed, or not the document we expected */
    WX_ERR_REJECTED,       /* the server said no: bad key, or no subscription */
    WX_ERR_NO_MEM,
    WX_ERR_NO_DATA,        /* parsed, but nothing usable for the day asked for */
} wx_status_t;

const char *wx_status_str(wx_status_t status);
