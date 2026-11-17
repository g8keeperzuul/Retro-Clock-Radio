#ifndef __UART_H__
#define __UART_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

void serial_begin(int baudrate, int txd, int rxd);
void serial_end(void);
int serial_available(void);
uint8_t serial_read(void);
uint8_t serial_peek(void);
void serial_print(char *ch);
void serial_println(char *ch);
void serial_write(uint8_t ch);
void serial_write_buffer(uint8_t *ch, int len);
void serial_flash();

/*
 * Route UART1's TX back into its own RX inside the SoC, without touching the
 * pins. Used by the boot self-test to tell "the MCU-side UART path is broken"
 * apart from "nothing is answering on the wire".
 */
esp_err_t serial_set_loopback(bool enable);

/* Drop anything already sitting in the driver + software RX buffers. */
void serial_rx_flush(void);

/* Change UART1's bit rate in place, for sweeping a module of unknown speed. */
esp_err_t serial_set_baudrate(int baudrate);

/* Re-route UART1's pins, e.g. to park TX on an unused pad during diagnostics. */
esp_err_t serial_set_pins(int txd, int rxd);

/*
 * Static electrical read of a pin, done before the UART claims it. A line that
 * an external device is actively driving holds its level regardless of which
 * internal pull we apply; a disconnected line simply follows the pull.
 */
typedef struct {
	int with_pulldown;
	int with_pullup;
} serial_line_probe_t;

void serial_probe_line(int gpio, serial_line_probe_t *out);

/*
 * Drive a pin both ways and read it back, to prove the pad itself is alive.
 * Only safe on a line nothing else is driving - check serial_probe_line() first.
 */
bool serial_test_pin_drive(int gpio);

/*
 * Direct continuity check between two pads: drive one, sense the other against
 * an opposing internal pull. A pad that follows the driven one in both
 * directions is joined to it. Run before the UART claims either pin.
 */
typedef struct {
	int sensed_when_driven_low;
	int sensed_when_driven_high;
} serial_continuity_t;

void serial_test_continuity(int drive_gpio, int sense_gpio, serial_continuity_t *out);

#endif
