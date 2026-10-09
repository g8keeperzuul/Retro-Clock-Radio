/*
 * mqtt_report.h - Publishing the diagnostics to an MQTT broker.
 *
 * The same fields the status screen shows, plus the Wi-Fi signal strength,
 * as a JSON object on one topic every MQTT_PUBLISH_INTERVAL_S. Both the
 * gathering and the masking of the API key live in diagnostics.h, so the panel
 * and the broker cannot come to disagree about what the device is.
 *
 * Broker, credentials, topic and interval are all build flags; see
 * platformio.ini. The password is a secret and lives in the ignored
 * platformio.ini alongside the Wi-Fi key, never in a source file.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connect and start publishing.
 *
 * Returns as soon as the client is running, not once it has reached the
 * broker -- esp-mqtt connects and reconnects in the background, and a broker
 * that is down is a thing to keep trying rather than a reason to fail boot.
 * Safe to call before Wi-Fi is up.
 */
esp_err_t mqtt_report_start(void);

#ifdef __cplusplus
}
#endif
