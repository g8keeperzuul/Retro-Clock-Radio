/*
 * rotary_encoder.c - Polling the seesaw encoder breakout. See the header.
 */
#include "rotary_encoder.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "encoder";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

#ifndef ENC_PIN_SDA
#define ENC_PIN_SDA 8
#endif
#ifndef ENC_PIN_SCL
#define ENC_PIN_SCL 9
#endif
#ifndef ENC_I2C_PORT
#define ENC_I2C_PORT I2C_NUM_0
#endif
#ifndef ENC_I2C_SPEED_HZ
#define ENC_I2C_SPEED_HZ 100000
#endif
#ifndef ENC_I2C_ADDR
#define ENC_I2C_ADDR 0x36
#endif
#ifndef ENC_POLL_INTERVAL_MS
#define ENC_POLL_INTERVAL_MS 50
#endif
#ifndef ENC_COUNTS_PER_DETENT
#define ENC_COUNTS_PER_DETENT 1
#endif
#ifndef ENC_REVERSED
#define ENC_REVERSED 0
#endif
#ifndef ENC_LED_PIN
#define ENC_LED_PIN 6            /* the seesaw pin the NeoPixel hangs off */
#endif
#ifndef ENC_LED_BRIGHTNESS
#define ENC_LED_BRIGHTNESS 40    /* out of 255; a NeoPixel at full is painful */
#endif
#ifndef ENC_FLASH_MS
#define ENC_FLASH_MS 150
#endif

/* ---- seesaw register map (Adafruit's firmware, not a hardware datasheet) -- */

#define SEESAW_STATUS_BASE   0x00
#define SEESAW_STATUS_HW_ID  0x01
#define SEESAW_STATUS_SWRST  0x7F

#define SEESAW_ENCODER_BASE  0x11
#define SEESAW_ENCODER_DELTA 0x40

#define SEESAW_NEOPIXEL_BASE       0x0E
#define SEESAW_NEOPIXEL_PIN        0x01
#define SEESAW_NEOPIXEL_SPEED      0x02
#define SEESAW_NEOPIXEL_BUF_LENGTH 0x03
#define SEESAW_NEOPIXEL_BUF        0x04
#define SEESAW_NEOPIXEL_SHOW       0x05

#define NEOPIXEL_800KHZ 0x01

/* SAMD09, then the ATtiny 806/807/816/817/1616/1617 family. */
#define HW_ID_SAMD09    0x55
#define HW_ID_TINY_LOW  0x84
#define HW_ID_TINY_HIGH 0x89

/*
 * seesaw is firmware bit-banging a bus, so it wants a gap between being told
 * which register to serve and being asked for it. This is also why the reads
 * below are a transmit and a receive rather than one
 * i2c_master_transmit_receive(): a repeated START leaves it no gap at all and
 * returns whatever was in its buffer from last time.
 */
#define SEESAW_READ_SETTLE_US 500

/* A reset takes the ATtiny about a quarter of a second to come back from. */
#define SEESAW_RESET_MS 500

#define I2C_TIMEOUT_MS 100

/* Complain about the first read failure in a run, then every hundredth. */
#define FAULT_LOG_EVERY 100

#define TASK_STACK 3072
#define TASK_PRIO  5     /* above the display tasks: it must never miss a poll */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static rotary_encoder_cb_t s_on_turn;

static esp_err_t seesaw_write(uint8_t base, uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t frame[8];

    if (n > sizeof(frame) - 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    frame[0] = base;
    frame[1] = reg;
    if (n > 0) {
        memcpy(&frame[2], data, n);
    }

    return i2c_master_transmit(s_dev, frame, n + 2, I2C_TIMEOUT_MS);
}

static esp_err_t seesaw_write8(uint8_t base, uint8_t reg, uint8_t value)
{
    return seesaw_write(base, reg, &value, 1);
}

static esp_err_t seesaw_read(uint8_t base, uint8_t reg, uint8_t *out, size_t n)
{
    const uint8_t addr[2] = {base, reg};

    const esp_err_t err = i2c_master_transmit(s_dev, addr, sizeof(addr), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    esp_rom_delay_us(SEESAW_READ_SETTLE_US);

    return i2c_master_receive(s_dev, out, n, I2C_TIMEOUT_MS);
}

/*
 * What each verdict looks like on the knob, at ENC_LED_BRIGHTNESS. Red for a
 * turn thrown away, amber for one that will not be drawn for up to three
 * minutes, green for one about to happen -- traffic lights, because that is
 * what the hand on the knob is actually asking.
 */
static const struct {
    uint8_t r, g, b;
} FLASH[] = {
    [ENC_ACCEPTED] = {0, 1, 0},
    [ENC_DEFERRED] = {1, 1, 0},
    [ENC_IGNORED]  = {1, 0, 0},
};

/*
 * When the pixel currently lit should go out; 0 when it is already dark. The
 * flash is not slept through -- the poll loop puts it out once the moment has
 * passed -- so a turn is never queued behind a blink.
 */
static int64_t s_flash_until_us;

static esp_err_t led_set(uint8_t r, uint8_t g, uint8_t b)
{
    /* Offset 0, then one pixel. The breakout's NeoPixel is wired green first. */
    const uint8_t frame[5] = {0x00, 0x00, g, r, b};

    esp_err_t err = seesaw_write(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_BUF, frame,
                                 sizeof(frame));
    if (err != ESP_OK) {
        return err;
    }
    return seesaw_write(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_SHOW, NULL, 0);
}

static esp_err_t led_init(void)
{
    esp_err_t err = seesaw_write8(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_PIN, ENC_LED_PIN);
    if (err == ESP_OK) {
        err = seesaw_write8(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_SPEED, NEOPIXEL_800KHZ);
    }
    if (err == ESP_OK) {
        const uint8_t len[2] = {0x00, 0x03};    /* one GRB pixel, big-endian */
        err = seesaw_write(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_BUF_LENGTH, len,
                           sizeof(len));
    }
    if (err == ESP_OK) {
        err = led_set(0, 0, 0);
    }
    return err;
}

static void flash(enc_verdict_t verdict)
{
    if (verdict > ENC_IGNORED) {
        return;
    }

    (void)led_set(FLASH[verdict].r * ENC_LED_BRIGHTNESS,
                  FLASH[verdict].g * ENC_LED_BRIGHTNESS,
                  FLASH[verdict].b * ENC_LED_BRIGHTNESS);

    /*
     * Re-armed rather than queued, so keeping the knob turning during a refresh
     * reads as a steady red rather than a stutter.
     */
    s_flash_until_us = esp_timer_get_time() + (int64_t)ENC_FLASH_MS * 1000;
}

/* Put the pixel out once its moment has passed. Called from the poll loop. */
static void flash_expire(void)
{
    if (s_flash_until_us != 0 && esp_timer_get_time() >= s_flash_until_us) {
        s_flash_until_us = 0;
        (void)led_set(0, 0, 0);
    }
}

/*
 * Counts turned since the last call. seesaw clears the register as it is read,
 * so this is genuinely a delta and nothing accumulates twice.
 */
static esp_err_t read_delta(int32_t *delta)
{
    uint8_t b[4];

    const esp_err_t err = seesaw_read(SEESAW_ENCODER_BASE, SEESAW_ENCODER_DELTA, b,
                                      sizeof(b));
    if (err != ESP_OK) {
        return err;
    }

    *delta = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                       ((uint32_t)b[2] << 8) | (uint32_t)b[3]);
    return ESP_OK;
}

static void encoder_task(void *arg)
{
    (void)arg;

    /* Counts that have not yet added up to a whole detent. */
    int32_t residue = 0;
    unsigned faults = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ENC_POLL_INTERVAL_MS));

        flash_expire();

        int32_t delta = 0;
        const esp_err_t err = read_delta(&delta);

        if (err != ESP_OK) {
            if (faults++ % FAULT_LOG_EVERY == 0) {
                ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        faults = 0;

        if (delta == 0) {
            continue;
        }

        residue += (ENC_REVERSED ? -delta : delta);

        /* C truncates toward zero, so this is symmetric about a standing knob. */
        const int detents = (int)(residue / ENC_COUNTS_PER_DETENT);
        residue -= (int32_t)detents * ENC_COUNTS_PER_DETENT;

        if (detents != 0 && s_on_turn != NULL) {
            flash(s_on_turn(detents));
        }
    }
}

esp_err_t rotary_encoder_start(rotary_encoder_cb_t on_turn)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ENC_I2C_PORT,
        .sda_io_num = ENC_PIN_SDA,
        .scl_io_num = ENC_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /*
         * The breakout carries its own pull-ups; these are for the case where
         * someone has soldered to the pads instead of using a QT cable. They
         * are too weak to be relied on above 100 kHz.
         */
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not open I2C on SDA %d / SCL %d: %s",
                 ENC_PIN_SDA, ENC_PIN_SCL, esp_err_to_name(err));
        return err;
    }

    if (i2c_master_probe(s_bus, ENC_I2C_ADDR, I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "nothing answered at 0x%02X on SDA %d / SCL %d",
                 ENC_I2C_ADDR, ENC_PIN_SDA, ENC_PIN_SCL);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ENC_I2C_ADDR,
        .scl_speed_hz = ENC_I2C_SPEED_HZ,
    };

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        goto fail;
    }

    /*
     * Reset first. The seesaw keeps its encoder count across a host reboot, and
     * starting from a fresh zero is one less thing to reason about.
     */
    err = seesaw_write8(SEESAW_STATUS_BASE, SEESAW_STATUS_SWRST, 0xFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset failed: %s", esp_err_to_name(err));
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(SEESAW_RESET_MS));

    uint8_t hw_id = 0;
    err = seesaw_read(SEESAW_STATUS_BASE, SEESAW_STATUS_HW_ID, &hw_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no answer after reset: %s", esp_err_to_name(err));
        goto fail;
    }

    /*
     * Warn rather than refuse: an unrecognised id is far more likely to be a
     * seesaw build newer than this list than a different chip, and refusing to
     * drive a knob that would have worked is the worse mistake.
     */
    if (hw_id != HW_ID_SAMD09 && (hw_id < HW_ID_TINY_LOW || hw_id > HW_ID_TINY_HIGH)) {
        ESP_LOGW(TAG, "unfamiliar seesaw id 0x%02X -- carrying on anyway", hw_id);
    }

    /*
     * A dark pixel is not proof of anything, so a failure here is logged and
     * shrugged off: a knob that turns without lighting up is still a knob.
     */
    if (led_init() != ESP_OK) {
        ESP_LOGW(TAG, "the knob's NeoPixel did not answer; carrying on unlit");
    }

    int32_t discard = 0;
    (void)read_delta(&discard);   /* clear anything the reset left behind */

    s_on_turn = on_turn;

    if (xTaskCreate(encoder_task, "encoder", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "seesaw 0x%02X at 0x%02X on SDA %d / SCL %d, polled every %d ms",
             hw_id, ENC_I2C_ADDR, ENC_PIN_SDA, ENC_PIN_SCL, ENC_POLL_INTERVAL_MS);
    return ESP_OK;

fail:
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    return err;
}
