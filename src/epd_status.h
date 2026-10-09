/*
 * epd_status.h - What this thing is, for when something is wrong.
 *
 * A diagnostics screen: the network it joined, the addresses it answers on, the
 * coordinates and key it forecasts with, how many refreshes the panel has left
 * in it, and how long it has been up.
 *
 * Set in the plain 5x7 font rather than the Deco face the other screens use.
 * That is deliberate: this screen is read character by character -- an IP
 * address, a MAC, a key -- and a decorative face makes 8 and B and 0 and O
 * harder to tell apart, which is exactly the distinction that matters here.
 *
 * It shows itself for STATUS_LINGER_S and then hands the panel back to the
 * clock, so it cannot be left up by accident. See README.md "Status screen".
 */
#pragma once

#include "epd_5in83g.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draw and push to the panel. Blocks for a refresh, about 25 seconds. */
esp_err_t epd_status_show(void);

#ifdef __cplusplus
}
#endif
