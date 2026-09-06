/*
 * refresh_gauge.c - Driving the countdown strip. See the header.
 */
#include "refresh_gauge.h"

#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "gauge";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef GAUGE_PIN
#define GAUGE_PIN 21
#endif
/* GAUGE_LED_COUNT lives in the header: the caller needs it too. */

/* ---- WS2812 ------------------------------------------------------------- */

/*
 * 20 MHz, so one tick is 0.05 us. The obvious 10 MHz grid can only express
 * 0.40/0.80/0.80/0.40, which suits a WS2812B but puts T1L under the plain
 * WS2812's minimum and T1H over the SK6812's maximum. At 20 MHz the timings
 * below land inside all three windows, and the clock costs nothing: it is the
 * 80 MHz APB over four, and this build has no power management to move it.
 */
#define GAUGE_RESOLUTION_HZ (20 * 1000 * 1000)

#define T0H_TICKS 7    /* 0.35 us */
#define T0L_TICKS 18   /* 0.90 us */
#define T1H_TICKS 14   /* 0.70 us */
#define T1L_TICKS 11   /* 0.55 us */

/*
 * Low time that latches the frame. The datasheets say 50 us; the WS2812B-V5
 * wants 280. 300 covers every part anyone is likely to solder on.
 */
#define RESET_GAP_US 300

/*
 * The whole frame has to fit in the channel's own memory. Six pixels is 144
 * symbols plus an end marker, and this field is rounded up to whole 48-word
 * blocks -- so anything under 192 leaves the channel in ping-pong mode, where
 * an interrupt must refill it inside sixty microseconds while the Wi-Fi stack
 * is competing for the same core. 192 is all four TX blocks and needs no
 * interrupt at all. Nothing else here uses RMT; the first thing that does will
 * have to share.
 */
#define GAUGE_MEM_BLOCK_SYMBOLS 192

#define BYTES_PER_LED 3
#define PAYLOAD_BYTES (GAUGE_LED_COUNT * BYTES_PER_LED)

#define TX_TIMEOUT_MS 100

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static bool s_ready;

/*
 * Static because rmt_transmit() does not copy: the buffer has to stay put
 * until the transmission finishes.
 */
static uint8_t s_pixels[PAYLOAD_BYTES];

/* When the last frame finished, so the next one can leave the strip a gap. */
static int64_t s_last_tx_end_us;

static void transmit(void)
{
    if (!s_ready) {
        return;
    }

    /*
     * Leave the line low long enough for the strip to latch. It is tempting to
     * argue this is free -- frames are a fifth of a second apart -- but that is
     * only true on the paths where the caller waits. When a countdown is
     * interrupted, the clear follows its show by microseconds, and without this
     * the second frame is read as a continuation of the first: it shifts into
     * pixels seven onwards, the real six keep their colour, and the strip stays
     * lit with no way to put it out.
     */
    if (s_last_tx_end_us != 0) {
        const int64_t since_us = esp_timer_get_time() - s_last_tx_end_us;
        if (since_us < RESET_GAP_US) {
            esp_rom_delay_us((uint32_t)(RESET_GAP_US - since_us));
        }
    }

    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,          /* hold the line low afterwards */
            .queue_nonblocking = 1,  /* never block the screen task on a full queue */
        },
    };

    esp_err_t err = rmt_transmit(s_chan, s_encoder, s_pixels, sizeof(s_pixels), &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_chan, TX_TIMEOUT_MS);
    }

    s_last_tx_end_us = esp_timer_get_time();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not drive the strip: %s", esp_err_to_name(err));
    }
}

void refresh_gauge_show(uint8_t r, uint8_t g, uint8_t b, int lit)
{
    if (!s_ready) {
        return;
    }

    if (lit < 0) {
        lit = 0;
    } else if (lit > GAUGE_LED_COUNT) {
        lit = GAUGE_LED_COUNT;
    }

    for (int i = 0; i < GAUGE_LED_COUNT; i++) {
        const bool on = (i < lit);

        /* WS2812s take green first. */
        s_pixels[i * BYTES_PER_LED + 0] = on ? g : 0;
        s_pixels[i * BYTES_PER_LED + 1] = on ? r : 0;
        s_pixels[i * BYTES_PER_LED + 2] = on ? b : 0;
    }

    transmit();
}

void refresh_gauge_clear(void)
{
    if (!s_ready) {
        return;
    }

    memset(s_pixels, 0, sizeof(s_pixels));
    transmit();
}

esp_err_t refresh_gauge_start(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = GAUGE_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = GAUGE_RESOLUTION_HZ,
        .mem_block_symbols = GAUGE_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth = 4,
        .flags.init_level = 0,      /* the strip sees a low line from boot */
    };

    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not claim an RMT channel for GPIO %d: %s", GAUGE_PIN,
                 esp_err_to_name(err));
        return err;
    }

    const rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {.level0 = 1, .duration0 = T0H_TICKS, .level1 = 0, .duration1 = T0L_TICKS},
        .bit1 = {.level0 = 1, .duration0 = T1H_TICKS, .level1 = 0, .duration1 = T1L_TICKS},
        .flags.msb_first = 1,
    };

    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not build the encoder: %s", esp_err_to_name(err));
        goto fail;
    }

    /*
     * Without this the channel stays in its init state and rmt_transmit()
     * quietly does nothing -- it returns ESP_OK regardless, and once the
     * transaction queue has drained the call blocks forever. Forgetting it does
     * not look like an error; it looks like the display dying.
     */
    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not enable the channel: %s", esp_err_to_name(err));
        goto fail;
    }

    s_ready = true;
    refresh_gauge_clear();

    ESP_LOGI(TAG, "%d WS2812s on GPIO %d", GAUGE_LED_COUNT, GAUGE_PIN);
    return ESP_OK;

fail:
    if (s_encoder != NULL) {
        rmt_del_encoder(s_encoder);
        s_encoder = NULL;
    }
    rmt_del_channel(s_chan);
    s_chan = NULL;
    return err;
}
