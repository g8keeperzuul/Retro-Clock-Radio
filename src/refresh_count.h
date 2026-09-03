/*
 * refresh_count.h - Lifetime count of panel refreshes, held in NVS.
 *
 * The panel is rated for a finite number of updates -- a million, per the
 * datasheet -- so the count is worth keeping across reboots and across firmware
 * uploads. It lives in the `nvs` partition, which a normal flash of the app
 * does not touch; only an explicit full erase (`pio run -t erase`, or
 * esptool's erase_flash) takes it with it.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load the stored count and hook the panel driver so every completed refresh is
 * counted from here on. Call once, before the first refresh.
 *
 * A failure here is not fatal: the display works, the count just stops being
 * kept, and the error says so.
 */
esp_err_t refresh_count_init(void);

/* Refreshes this panel has performed in its lifetime, 0 if unavailable. */
uint32_t refresh_count_get(void);

#ifdef __cplusplus
}
#endif
