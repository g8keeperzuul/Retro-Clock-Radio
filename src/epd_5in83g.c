/*
 * epd_5in83g.c - Waveshare 5.83" e-Paper (G) panel driver. See epd_5in83g.h.
 *
 * The command sequences below are transcribed from Waveshare's reference
 * driver; the magic register values are theirs and are undocumented beyond the
 * comments they ship. What changed in the port is the transport: instead of
 * bit-banging one byte per chip-select, this pushes the frame in DMA-sized
 * bursts with the SPI peripheral driving CS.
 */
#include "epd_5in83g.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* --- Wiring. Defaults match Waveshare's own ESP32-S3 reference sketch. ----- */
#ifndef EPD_PIN_SCLK
#define EPD_PIN_SCLK 12
#endif
#ifndef EPD_PIN_MOSI
#define EPD_PIN_MOSI 11
#endif
#ifndef EPD_PIN_CS
#define EPD_PIN_CS 10
#endif
#ifndef EPD_PIN_DC
#define EPD_PIN_DC 13
#endif
#ifndef EPD_PIN_RST
#define EPD_PIN_RST 14
#endif
#ifndef EPD_PIN_BUSY
#define EPD_PIN_BUSY 4
#endif
/*
 * PWR is a real signal on this panel: Waveshare's pin-out is nine wires, and
 * their own ESP32-S3 reference drives it high before touching anything else.
 * Leave it floating and the panel simply never powers up. Set to -1 only for an
 * older 8-pin panel that has no such pin.
 */
#ifndef EPD_PIN_PWR
#define EPD_PIN_PWR 5
#endif

#ifndef EPD_SPI_HOST
#define EPD_SPI_HOST SPI2_HOST
#endif
#ifndef EPD_SPI_SPEED_HZ
#define EPD_SPI_SPEED_HZ 10000000
#endif

/*
 * Ceiling on a single BUSY wait. A full four-colour refresh takes ~25 s, so
 * this is generous enough to be a wiring diagnostic rather than a deadline.
 */
#ifndef EPD_BUSY_TIMEOUT_MS
#define EPD_BUSY_TIMEOUT_MS 60000
#endif

/* Bounce buffer size; also the SPI bus transfer ceiling. */
#define EPD_CHUNK_BYTES 4096

/* Panel commands, named as in the SSD-style datasheet. */
#define CMD_PANEL_SETTING      0x00
#define CMD_POWER_OFF          0x02
#define CMD_POWER_ON           0x04
#define CMD_DEEP_SLEEP         0x07
#define CMD_START_TRANSMISSION 0x10
#define CMD_DISPLAY_REFRESH    0x12
#define CMD_VCOM_INTERVAL      0x50
#define CMD_RESOLUTION         0x61
#define CMD_TCON_SETTING       0x65

static spi_device_handle_t s_dev;
static uint8_t *s_chunk;   /* internal, DMA-capable staging buffer */

/* --- Transport ------------------------------------------------------------ */

static esp_err_t epd_tx(int dc, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(EPD_PIN_DC, dc), TAG, "dc");

    while (len > 0) {
        size_t n = len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES;
        memcpy(s_chunk, data, n);

        spi_transaction_t t = {
            .length = n * 8,
            .tx_buffer = s_chunk,
        };
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(s_dev, &t), TAG, "spi tx");

        data += n;
        len -= n;
    }
    return ESP_OK;
}

/* Send the same byte len times, without staging a caller-side buffer for it. */
static esp_err_t epd_tx_fill(uint8_t value, size_t len)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(EPD_PIN_DC, 1), TAG, "dc");
    memset(s_chunk, value, len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES);

    while (len > 0) {
        size_t n = len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES;

        spi_transaction_t t = {
            .length = n * 8,
            .tx_buffer = s_chunk,
        };
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(s_dev, &t), TAG, "spi fill");

        len -= n;
    }
    return ESP_OK;
}

static esp_err_t epd_cmd(uint8_t reg)
{
    return epd_tx(0, &reg, 1);
}

static esp_err_t epd_data(uint8_t value)
{
    return epd_tx(1, &value, 1);
}

/* --- Panel handshake ------------------------------------------------------ */

/*
 * On this panel BUSY reads low while the controller is working and high once it
 * is idle -- the opposite sense of most Waveshare monochrome panels.
 */
static esp_err_t epd_wait_idle(int *elapsed_ms)
{
    const TickType_t poll = pdMS_TO_TICKS(10);
    int waited_ms = 0;

    vTaskDelay(pdMS_TO_TICKS(10));
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        if (waited_ms >= EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY stuck low for %d ms -- check wiring on GPIO %d",
                     waited_ms, EPD_PIN_BUSY);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(poll);
        waited_ms += 10;
    }

    if (elapsed_ms != NULL) {
        *elapsed_ms = waited_ms;
    }
    return ESP_OK;
}

/*
 * Waveshare asks for a >=2 ms low pulse, but a 100 Hz FreeRTOS tick rounds any
 * delay under 10 ms down to a bare yield -- so hold it low for two whole ticks.
 * Overshooting the minimum costs nothing here.
 */
static void epd_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/*
 * A real four-colour refresh holds BUSY low for the better part of half a
 * minute. Returning promptly means BUSY was never asserted at all -- the panel
 * is not listening, and the pin is just floating high. That failure is
 * otherwise silent, so name it here rather than reporting a phantom success.
 */
#define EPD_REFRESH_MIN_PLAUSIBLE_MS 2000

static epd_refresh_hook_t s_refresh_hook;

void epd_set_refresh_hook(epd_refresh_hook_t hook)
{
    s_refresh_hook = hook;
}

static esp_err_t epd_refresh(void)
{
    int elapsed_ms = 0;

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_DISPLAY_REFRESH), TAG, "refresh");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "refresh arg");
    ESP_RETURN_ON_ERROR(epd_wait_idle(&elapsed_ms), TAG, "refresh wait");

    if (elapsed_ms < EPD_REFRESH_MIN_PLAUSIBLE_MS) {
        ESP_LOGE(TAG, "refresh finished in %d ms; a real one takes ~25 s", elapsed_ms);
        ESP_LOGE(TAG, "BUSY (GPIO %d) never went low -- the panel is not responding",
                 EPD_PIN_BUSY);
#if EPD_PIN_PWR >= 0
        ESP_LOGE(TAG, "check that the panel's PWR pin is wired to GPIO %d", EPD_PIN_PWR);
#else
        ESP_LOGE(TAG, "EPD_PIN_PWR is -1: if the panel has a PWR pin it must be driven");
#endif
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGD(TAG, "refresh took %d ms", elapsed_ms);

    /* Only a refresh the panel actually performed counts against its life. */
    if (s_refresh_hook != NULL) {
        s_refresh_hook();
    }
    return ESP_OK;
}

/* Register load shared by the normal and fast waveform variants. */
static esp_err_t epd_load_registers(void)
{
    epd_reset();

    ESP_RETURN_ON_ERROR(epd_cmd(0x4D), TAG, "0x4D");
    ESP_RETURN_ON_ERROR(epd_data(0x78), TAG, "0x4D arg");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_PANEL_SETTING), TAG, "panel");
    ESP_RETURN_ON_ERROR(epd_data(0x2F), TAG, "panel arg0");
    ESP_RETURN_ON_ERROR(epd_data(0x29), TAG, "panel arg1");

    ESP_RETURN_ON_ERROR(epd_cmd(0xE3), TAG, "0xE3");
    ESP_RETURN_ON_ERROR(epd_data(0x88), TAG, "0xE3 arg");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_VCOM_INTERVAL), TAG, "vcom");
    ESP_RETURN_ON_ERROR(epd_data(0x37), TAG, "vcom arg");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_RESOLUTION), TAG, "resolution");
    ESP_RETURN_ON_ERROR(epd_data(EPD_WIDTH / 256), TAG, "res w hi");
    ESP_RETURN_ON_ERROR(epd_data(EPD_WIDTH % 256), TAG, "res w lo");
    ESP_RETURN_ON_ERROR(epd_data(EPD_HEIGHT / 256), TAG, "res h hi");
    ESP_RETURN_ON_ERROR(epd_data(EPD_HEIGHT % 256), TAG, "res h lo");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_TCON_SETTING), TAG, "tcon");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "tcon arg0");
    ESP_RETURN_ON_ERROR(epd_data(0x10), TAG, "tcon arg1");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "tcon arg2");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "tcon arg3");

    ESP_RETURN_ON_ERROR(epd_cmd(0xE9), TAG, "0xE9");
    ESP_RETURN_ON_ERROR(epd_data(0x01), TAG, "0xE9 arg");

    return ESP_OK;
}

/* --- Public API ----------------------------------------------------------- */

esp_err_t epd_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST)
#if EPD_PIN_PWR >= 0
                        | (1ULL << EPD_PIN_PWR)
#endif
        ,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "output pins");

    gpio_config_t busy = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy), TAG, "busy pin");

#if EPD_PIN_PWR >= 0
    /* Open the power gate first and let the panel's rails come up. */
    gpio_set_level(EPD_PIN_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_RST, 1);

    /*
     * The frame is copied here before every transfer: the caller's buffer may
     * live in PSRAM, and this keeps the DMA source unambiguously internal.
     */
    s_chunk = heap_caps_malloc(EPD_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_chunk != NULL, ESP_ERR_NO_MEM, TAG, "no DMA buffer");

    spi_bus_config_t bus = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_CHUNK_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_SPEED_HZ,
        .mode = 0,
        .spics_io_num = EPD_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_dev), TAG, "spi device");

    ESP_LOGI(TAG, "5.83\" (G) %dx%d on SPI%d: sclk %d, mosi %d, cs %d, dc %d, rst %d, busy %d, pwr %d",
             EPD_WIDTH, EPD_HEIGHT, EPD_SPI_HOST + 1, EPD_PIN_SCLK, EPD_PIN_MOSI,
             EPD_PIN_CS, EPD_PIN_DC, EPD_PIN_RST, EPD_PIN_BUSY, EPD_PIN_PWR);
    ESP_LOGI(TAG, "BUSY reads %d at rest (1 = idle, but also what a floating pin reads)",
             gpio_get_level(EPD_PIN_BUSY));
    return ESP_OK;
}

esp_err_t epd_wake(void)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    ESP_RETURN_ON_ERROR(epd_load_registers(), TAG, "registers");

    ESP_RETURN_ON_ERROR(epd_cmd(0x30), TAG, "waveform");  /* frame goes with waveform */
    ESP_RETURN_ON_ERROR(epd_data(0x08), TAG, "waveform arg");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_POWER_ON), TAG, "power on");
    return epd_wait_idle(NULL);
}

esp_err_t epd_wake_fast(void)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    ESP_RETURN_ON_ERROR(epd_load_registers(), TAG, "registers");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_POWER_ON), TAG, "power on");
    ESP_RETURN_ON_ERROR(epd_wait_idle(NULL), TAG, "power on wait");

    /* Swap in the shortened waveform and let the IC re-latch it. */
    ESP_RETURN_ON_ERROR(epd_cmd(0xE0), TAG, "fast 0xE0");
    ESP_RETURN_ON_ERROR(epd_data(0x02), TAG, "fast 0xE0 arg");
    ESP_RETURN_ON_ERROR(epd_cmd(0xE6), TAG, "fast 0xE6");
    ESP_RETURN_ON_ERROR(epd_data(0x68), TAG, "fast 0xE6 arg");
    ESP_RETURN_ON_ERROR(epd_cmd(0xA5), TAG, "fast 0xA5");
    return epd_wait_idle(NULL);
}

esp_err_t epd_clear(epd_color_t color)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    uint8_t packed = (uint8_t)((color << 6) | (color << 4) | (color << 2) | color);

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_START_TRANSMISSION), TAG, "start tx");
    ESP_RETURN_ON_ERROR(epd_tx_fill(packed, EPD_FB_SIZE), TAG, "fill");
    return epd_refresh();
}

esp_err_t epd_display(const uint8_t *fb)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(fb != NULL, ESP_ERR_INVALID_ARG, TAG, "null frame");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_START_TRANSMISSION), TAG, "start tx");
    ESP_RETURN_ON_ERROR(epd_tx(1, fb, EPD_FB_SIZE), TAG, "frame");
    return epd_refresh();
}

esp_err_t epd_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_POWER_OFF), TAG, "power off");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "power off arg");
    ESP_RETURN_ON_ERROR(epd_wait_idle(NULL), TAG, "power off wait");

    ESP_RETURN_ON_ERROR(epd_cmd(CMD_DEEP_SLEEP), TAG, "deep sleep");
    return epd_data(0xA5);
}

uint8_t *epd_fb_alloc(void)
{
    /* 76 KB: worth spending PSRAM on, but internal RAM will do if there is none. */
    uint8_t *fb = heap_caps_calloc(1, EPD_FB_SIZE, MALLOC_CAP_SPIRAM);
    if (fb == NULL) {
        fb = calloc(1, EPD_FB_SIZE);
    }
    return fb;
}

void epd_fb_free(uint8_t *fb)
{
    heap_caps_free(fb);
}
