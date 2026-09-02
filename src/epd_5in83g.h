/*
 * epd_5in83g.h - Waveshare 5.83" e-Paper (G) panel driver, 648x480, 4 colour.
 *
 * Ported from the Waveshare reference driver
 * (E-paper_Separate_Program/5in83_e-Paper_G) onto the ESP-IDF SPI master and
 * GPIO drivers. Pin assignment, SPI host and clock rate are compile-time
 * parameters supplied by platformio.ini (build_flags); see that file.
 *
 * Frame buffers are 2 bits per pixel, four pixels per byte, leftmost pixel in
 * the most significant bit pair -- the packing the panel itself expects, so
 * epd_display() ships the buffer verbatim.
 *
 * A full refresh takes roughly 25 seconds and blocks the calling task.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_WIDTH  648
#define EPD_HEIGHT 480

/* Four pixels per byte. 648 divides evenly, so rows need no padding. */
#define EPD_ROW_BYTES ((EPD_WIDTH + 3) / 4)
#define EPD_FB_SIZE   ((size_t)EPD_ROW_BYTES * EPD_HEIGHT)

/* The panel's own colour codes -- these values go on the wire as-is. */
typedef enum {
    EPD_BLACK  = 0x0,
    EPD_WHITE  = 0x1,
    EPD_YELLOW = 0x2,
    EPD_RED    = 0x3,
} epd_color_t;

/*
 * Claim the SPI bus and the DC/RST/BUSY/PWR pins. Leaves the panel powered but
 * un-initialised; call epd_wake() before drawing. Safe to call once, early.
 */
esp_err_t epd_init(void);

/*
 * Load the panel registers and power up the driver IC. Required after every
 * epd_sleep(), which is to say before every refresh in normal use.
 *
 * epd_wake_fast() selects the shorter waveform: quicker but with more visible
 * ghosting, so prefer epd_wake() when image quality matters.
 */
esp_err_t epd_wake(void);
esp_err_t epd_wake_fast(void);

/* Flood the whole panel with one colour. Blocks until the refresh completes. */
esp_err_t epd_clear(epd_color_t color);

/*
 * Push a full EPD_FB_SIZE frame buffer and refresh. The buffer may live in
 * PSRAM; it is copied through an internal DMA-capable bounce buffer.
 */
esp_err_t epd_display(const uint8_t *fb);

/*
 * Power the driver IC down into deep sleep. The image stays on the glass.
 * Nothing but epd_wake() works afterwards -- and the datasheet asks for at
 * least 2 s of settling before cutting panel power entirely.
 */
esp_err_t epd_sleep(void);

/* Allocate a zeroed frame buffer, preferring PSRAM. NULL when out of memory. */
uint8_t *epd_fb_alloc(void);
void epd_fb_free(uint8_t *fb);

#ifdef __cplusplus
}
#endif
