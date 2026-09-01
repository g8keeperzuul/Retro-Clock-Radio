# Retro Clock-Radio

## Component List
| Part | Notes |
|---|---|
| [ESP32-S3 v1.1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html) | ESP32-S3-DevKitC-1 Development Board BT 2.4G Wifi Module for Arduino 8MB PSRAM 16MB FLASH N16R8 42Pin CH340C Type-C ESP32 S3. Verify your microcontroller details with [ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/). |
| [5.83inch E-Paper Display](https://www.waveshare.com/5.83inch-e-paper-g.htm) | E-ink Display, 648 × 480, Red/Yellow/Black/White, SPI Interface |
| [Rotary Encoder](https://www.adafruit.com/product/5880) | Adafruit I2C Stemma QT Rotary Encoder Breakout with Encoder - STEMMA QT / Qwiic |

## Firmware Status
Bootstraps the ESP32-S3 (NVS, TCP/IP stack, default event loop) and joins a
Wi-Fi network as a station. The e-paper display and rotary encoder are not
driven yet.

- [src/main.c](src/main.c) — boot banner, bring-up, link-health heartbeat
- [src/wifi_sta.c](src/wifi_sta.c) — station bring-up, retry/backoff, auto-reconnect

## Configuration
Wi-Fi credentials and connection tuning are compile-time parameters, defined as
`build_flags` in [platformio.ini](platformio.ini) — nothing is hard-coded in the
sources. Set `WIFI_SSID` and `WIFI_PASSWORD` there before the first build.

| Macro | Default | Purpose |
|---|---|---|
| `WIFI_SSID` | *(required)* | Network to join |
| `WIFI_PASSWORD` | *(required)* | Pre-shared key |
| `WIFI_HOSTNAME` | `retro-clock-radio` | Name requested from DHCP |
| `WIFI_AUTH_THRESHOLD` | `WIFI_AUTH_WPA2_PSK` | Weakest acceptable AP security |
| `WIFI_CONNECT_MAX_RETRY` | `0` | Attempts before reboot; 0 = retry forever |
| `WIFI_CONNECT_TIMEOUT_MS` | `15000` | Per-attempt deadline for assoc + DHCP |
| `WIFI_CONNECT_BACKOFF_MIN_MS` | `1000` | Initial delay between attempts |
| `WIFI_CONNECT_BACKOFF_MAX_MS` | `30000` | Backoff ceiling |
| `BOOT_LOG_DELAY_MS` | `5000` | Silent pause before the first log line |

Omitting `WIFI_SSID` or `WIFI_PASSWORD` fails the build with an `#error` rather
than silently producing firmware that cannot connect.

To keep credentials out of version control, comment those two flags out and
export them at build time instead:

```sh
export PLATFORMIO_BUILD_FLAGS='-D WIFI_SSID=\"my-ssid\" -D WIFI_PASSWORD=\"my-pass\"'
pio run -t upload -t monitor
```
