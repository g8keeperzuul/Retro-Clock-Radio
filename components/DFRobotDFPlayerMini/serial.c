/* UART asynchronous example, that uses separate RX and TX tasks

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "driver/gpio.h"

#include "serial.h"

#define RX_BUF_SIZE 1024
#define TX_BUF_SIZE 1024

/* A lost task notification used to hang the caller forever on portMAX_DELAY. */
#define TX_TIMEOUT_MS 1000

typedef struct {
	TaskHandle_t parent_task_handle;
	TaskHandle_t tx_task_handle;
	uint8_t tx_buf[TX_BUF_SIZE];
	int tx_len;
	TaskHandle_t rx_task_handle;
	uint8_t rx_buf[RX_BUF_SIZE];
	int rx_len;
	int rx_save_idx;
	int rx_read_idx;
} uart_obj_t;

static uart_obj_t uart_obj;

/*
 * Guards against a second bring-up. DF_begin() keeps its own _isInstall flag,
 * but any caller that drives serial_begin() directly (the boot self-test does)
 * bypasses it, and a repeat uart_driver_install() returns ESP_FAIL. Owning the
 * state here makes serial_begin() idempotent for every caller.
 */
static bool serial_installed = false;

static void tx_task(void *pvParameters)
{
	ESP_LOGI(pcTaskGetName(NULL), "Start");
	uart_obj.tx_len = 0;
	while (1) {
		ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
		ESP_LOGD(pcTaskGetName(NULL), "uart_obj.tx_len=%d", uart_obj.tx_len);
		int txBytes = uart_write_bytes(UART_NUM_1, uart_obj.tx_buf, uart_obj.tx_len);
		ESP_LOGD(pcTaskGetName(NULL), "txBytes=%d", txBytes);
		assert(uart_obj.tx_len == txBytes);
		uart_obj.tx_len = 0;
		xTaskNotifyGive(uart_obj.parent_task_handle);
	}
}

static void rx_task(void *pvParameters)
{
	ESP_LOGI(pcTaskGetName(NULL), "Start");
	uart_obj.rx_len = 0;
	uart_obj.rx_save_idx = 0;
	uart_obj.rx_read_idx = 0;
	uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE+1);
	while (1) {
		//const int rxBytes = uart_read_bytes(UART_NUM_1, data, RX_BUF_SIZE, 1000 / portTICK_PERIOD_MS);
		const int rxBytes = uart_read_bytes(UART_NUM_1, data, RX_BUF_SIZE, 1);
		if (rxBytes > 0) {
			data[rxBytes] = 0;
#if CONFIG_DEBUG_MODE
			ESP_LOGI(pcTaskGetName(NULL), "Read %d bytes", rxBytes);
#endif
			//ESP_LOG_BUFFER_HEXDUMP(pcTaskGetName(NULL), data, rxBytes, ESP_LOG_INFO);
			for (int index=0; index<rxBytes; index++) {
#if CONFIG_DEBUG_MODE
				ESP_LOGI(pcTaskGetName(NULL), "data[%d]=0x%x", index, data[index]);
#endif
				uart_obj.rx_buf[uart_obj.rx_save_idx] = data[index];
				uart_obj.rx_len++;
				uart_obj.rx_save_idx++;
				if (uart_obj.rx_save_idx == RX_BUF_SIZE) uart_obj.rx_save_idx = 0;
			}
			ESP_LOGD(pcTaskGetName(NULL), "uart_obj.rx_len=%d", uart_obj.rx_len);
		}
	}
	free(data);
}

void serial_begin(int baudrate, int txd, int rxd) {
	if (serial_installed) {
		ESP_LOGD(__FUNCTION__, "UART1 already up; ignoring repeat bring-up");
		return;
	}

	const uart_config_t uart_config = {
		//.baud_rate = 115200,
		.baud_rate = baudrate,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		.source_clk = UART_SCLK_DEFAULT,
#else
		.source_clk = UART_SCLK_APB,
#endif
	};
	// We won't use a buffer for sending data.
	// These all return esp_err_t. Silently discarding them means an invalid pin
	// or a bad config leaves the UART dead while the code happily "sends" frames
	// into a void, which looks exactly like an unresponsive DFPlayer.
	ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
	ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, txd, rxd, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
	/*
	 * Hold RX at idle-high ourselves. The DFPlayer tri-states its TX while it
	 * reboots after a 0x0C reset, and an undriven RX floats and decodes noise
	 * into phantom bytes ("Stack Wrong!"), which is enough to make the first
	 * DF_begin() fail.
	 */
	ESP_ERROR_CHECK(gpio_set_pull_mode(rxd, GPIO_PULLUP_ONLY));
	ESP_LOGI(__FUNCTION__, "UART1 up: %d baud, TX=GPIO%d, RX=GPIO%d (RX pulled up)",
	         baudrate, txd, rxd);

	// Start uart task
	UBaseType_t prio = uxTaskPriorityGet( NULL );
	ESP_LOGD(__FUNCTION__, "prio=%d configMAX_PRIORITIES=%d", prio, configMAX_PRIORITIES);
	if (prio != configMAX_PRIORITIES) prio++; 
	uart_obj.parent_task_handle = xTaskGetCurrentTaskHandle();
	xTaskCreate(rx_task, "uart_rx_task", 1024*3, NULL, prio, &(uart_obj.rx_task_handle));
	xTaskCreate(tx_task, "uart_tx_task", 1024*3, NULL, prio, &(uart_obj.tx_task_handle));

	// Wait for task start
	vTaskDelay(10);
	serial_installed = true;
}

void serial_end(void) {
	if (!serial_installed) return;
	vTaskDelete(uart_obj.rx_task_handle);
	vTaskDelete(uart_obj.tx_task_handle);
	uart_obj.rx_task_handle = NULL;
	uart_obj.tx_task_handle = NULL;
	ESP_ERROR_CHECK(uart_driver_delete(UART_NUM_1));
	serial_installed = false;
}

int serial_available(void) {
	ESP_LOGD(__FUNCTION__, "uart_obj.rx_len=%d", uart_obj.rx_len);
	return (uart_obj.rx_len);
}

uint8_t serial_read(void) {
	uint8_t data = uart_obj.rx_buf[uart_obj.rx_read_idx];
	uart_obj.rx_read_idx++;
	if (uart_obj.rx_read_idx == RX_BUF_SIZE) uart_obj.rx_read_idx = 0;
	uart_obj.rx_len--;
	return data;
}

uint8_t serial_peek(void) {
	uint8_t data = uart_obj.rx_buf[uart_obj.rx_read_idx];
	//uart_obj.rx_read_idx++;
	//if (uart_obj.rx_read_idx == RX_BUF_SIZE) uart_obj.rx_read_idx = 0;
	//uart_obj.rx_len--;
	return data;
}

void serial_print(char *ch) {
	int len = strlen(ch);
	serial_write_buffer((uint8_t *)ch, len);
}

void serial_println(char *ch) {
	char _ch[128];
	int len = strlen(ch);
	strcpy(_ch, ch);
	_ch[len] = 0x0d;
	_ch[len+1] = 0x0a;
	serial_write_buffer((uint8_t *)_ch, len+2);
}

void serial_write(uint8_t ch) {
	uart_obj.tx_buf[0] = ch;
	uart_obj.tx_len=1;
	xTaskNotifyGive(uart_obj.tx_task_handle);
	if (ulTaskNotifyTake( pdTRUE, pdMS_TO_TICKS(TX_TIMEOUT_MS) ) == 0) {
		ESP_LOGE(__FUNCTION__, "tx_task did not ack within %d ms", TX_TIMEOUT_MS);
	}
}

void serial_write_buffer(uint8_t *ch, int len) {
	for (int i=0;i<len;i++) {
		uart_obj.tx_buf[i] = ch[i];
	}
	uart_obj.tx_len=len;
	xTaskNotifyGive(uart_obj.tx_task_handle);
	if (ulTaskNotifyTake( pdTRUE, pdMS_TO_TICKS(TX_TIMEOUT_MS) ) == 0) {
		ESP_LOGE(__FUNCTION__, "tx_task did not ack within %d ms", TX_TIMEOUT_MS);
	}
}

void serial_flash() {

}

esp_err_t serial_set_loopback(bool enable) {
	return uart_set_loop_back(UART_NUM_1, enable);
}

void serial_rx_flush(void) {
	/*
	 * Software-only: rx_task sits in uart_read_bytes() on this same UART
	 * essentially all the time, and calling uart_flush_input() underneath it
	 * contends for the driver's RX mutex. Just drop the ring buffer instead.
	 */
	/* Twice, with a gap: rx_task may still be carrying bytes across from the
	 * driver's own ring buffer when the first pass runs. */
	for (int i = 0; i < 2; i++) {
		uart_obj.rx_read_idx = uart_obj.rx_save_idx;
		uart_obj.rx_len = 0;
		vTaskDelay(pdMS_TO_TICKS(20));
	}
	uart_obj.rx_read_idx = uart_obj.rx_save_idx;
	uart_obj.rx_len = 0;
}

esp_err_t serial_set_baudrate(int baudrate) {
	return uart_set_baudrate(UART_NUM_1, baudrate);
}

esp_err_t serial_set_pins(int txd, int rxd) {
	return uart_set_pin(UART_NUM_1, txd, rxd, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void serial_probe_line(int gpio, serial_line_probe_t *out) {
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << gpio,
		.mode = GPIO_MODE_INPUT,
		.intr_type = GPIO_INTR_DISABLE,
	};

	cfg.pull_up_en = GPIO_PULLUP_DISABLE;
	cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
	ESP_ERROR_CHECK(gpio_config(&cfg));
	vTaskDelay(pdMS_TO_TICKS(20));
	out->with_pulldown = gpio_get_level(gpio);

	cfg.pull_up_en = GPIO_PULLUP_ENABLE;
	cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
	ESP_ERROR_CHECK(gpio_config(&cfg));
	vTaskDelay(pdMS_TO_TICKS(20));
	out->with_pullup = gpio_get_level(gpio);

	/* leave the pin neutral so it does not fight the UART afterwards */
	cfg.pull_up_en = GPIO_PULLUP_DISABLE;
	cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
	ESP_ERROR_CHECK(gpio_config(&cfg));
}

bool serial_test_pin_drive(int gpio) {
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << gpio,
		.mode = GPIO_MODE_INPUT_OUTPUT,   /* read back what we drive */
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&cfg));

	bool ok = true;
	gpio_set_level(gpio, 1);
	vTaskDelay(pdMS_TO_TICKS(5));
	if (gpio_get_level(gpio) != 1) ok = false;
	gpio_set_level(gpio, 0);
	vTaskDelay(pdMS_TO_TICKS(5));
	if (gpio_get_level(gpio) != 0) ok = false;

	cfg.mode = GPIO_MODE_INPUT;
	ESP_ERROR_CHECK(gpio_config(&cfg));
	return ok;
}

void serial_test_continuity(int drive_gpio, int sense_gpio, serial_continuity_t *out) {
	gpio_config_t drv = {
		.pin_bit_mask = 1ULL << drive_gpio,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config_t sns = {
		.pin_bit_mask = 1ULL << sense_gpio,
		.mode = GPIO_MODE_INPUT,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&drv));

	/* Drive low against a pull-up: a joined pad is dragged to 0. */
	sns.pull_up_en = GPIO_PULLUP_ENABLE;
	sns.pull_down_en = GPIO_PULLDOWN_DISABLE;
	ESP_ERROR_CHECK(gpio_config(&sns));
	gpio_set_level(drive_gpio, 0);
	vTaskDelay(pdMS_TO_TICKS(20));
	out->sensed_when_driven_low = gpio_get_level(sense_gpio);

	/* Drive high against a pull-down: a joined pad is pulled up to 1. */
	sns.pull_up_en = GPIO_PULLUP_DISABLE;
	sns.pull_down_en = GPIO_PULLDOWN_ENABLE;
	ESP_ERROR_CHECK(gpio_config(&sns));
	gpio_set_level(drive_gpio, 1);
	vTaskDelay(pdMS_TO_TICKS(20));
	out->sensed_when_driven_high = gpio_get_level(sense_gpio);

	/* leave both pads neutral inputs */
	drv.mode = GPIO_MODE_INPUT;
	ESP_ERROR_CHECK(gpio_config(&drv));
	sns.pull_down_en = GPIO_PULLDOWN_DISABLE;
	ESP_ERROR_CHECK(gpio_config(&sns));
}
