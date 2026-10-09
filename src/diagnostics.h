/*
 * diagnostics.h - What this thing is, gathered in one place.
 *
 * Two things report it and they must not drift apart: the status screen draws
 * it on the panel, and mqtt_report.c publishes it as JSON. Collecting the
 * fields here means adding one is a single edit, and that the API key is
 * masked by the same rule wherever it appears.
 *
 * Everything is a formatted string or a plain number, with no pointers into
 * anything that could change underneath a caller -- a diag_t is a snapshot and
 * is safe to hold for as long as it is useful.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];        /* the network actually joined, "--" if none */
    char ip[16];
    char mac[18];
    const char *timezone; /* the build's TIME_ZONE; a literal, not a copy */
    const char *latitude;
    const char *longitude;
    char api_key[72];     /* whole or masked, see diagnostics.c */

    uint32_t refreshes;   /* panel refreshes over the device's life */
    uint32_t uptime_min;  /* truncated to the minute, as asked */
    int      rssi_dbm;    /* negative; 0 when not associated */
    bool     online;
} diag_t;

/* Read everything as it stands right now. Cheap; safe from any task. */
void diagnostics_gather(diag_t *out);

/* "3d 04h 24m" -- the longest form is 11 characters plus the NUL. */
void diagnostics_format_uptime(const diag_t *d, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
