# Retro Clock-Radio
This retro clock is modelled after a [General Electric C437A Radio](https://www.radiomuseum.org/r/general_el_c437a.html) from 1959.
![GE C437A](./docs/pictures/GE_C437A.png)
![Clock front](./docs/pictures/front-clock-web.jpg)
Sixty-five years of technological innovation (microcontrollers, 3D FDM printing, wifi, etc) hidden behind a mid-century facade. 
![Internals](/docs/pictures/back-open-web.jpg)
![Back cover](./docs/pictures/back-web.jpg)

## Operation

The radio tuner dial is the function selector. 
Since there are five functions and 20 notches on the rotary encoder, each screen has a contiguous 4 notches assigned. The icons on the dial ring is used to indicate the currently selected function. See [Choosing a screen](#choosing-a-screen)

<p float="left">
<img width="33%" src="./docs/pictures/front-clock-web.jpg" />
<img width="33%" src="./docs/pictures/front-forecast-web.jpg" />
<img width="33%" src="./docs/pictures/front-weather-web.jpg" />
</p>

<p float="left">
<img width="24%" src="./docs/pictures/hclock.png" />
<img width="24%" src="./docs/pictures/forecast.png" />
<img width="24%" src="./docs/pictures/alarm.png" />
<img width="24%" src="./docs/pictures/status.png" />
</p>
<img width="97%" src="./docs/pictures/anglesc.png" />


## Component List
| Part | Notes |
|---|---|
| [ESP32-S3 v1.1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html) | ESP32-S3-DevKitC-1 Development Board BT 2.4G Wifi Module for Arduino 8MB PSRAM 16MB FLASH N16R8 42Pin CH340C Type-C ESP32 S3. Verify your microcontroller details with [ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/). |
| [5.83inch E-Paper Display](https://www.waveshare.com/5.83inch-e-paper-g.htm) | E-ink Display, 648 × 480, Red/Yellow/Black/White, SPI Interface |
| [Rotary Encoder](https://www.adafruit.com/product/5880) | Adafruit I2C Stemma QT Rotary Encoder Breakout with Encoder - STEMMA QT / Qwiic |
| [DFPlayer Mini](https://wiki.dfrobot.com/DFPlayer_Mini_SKU_DFR0299) | DFRobot MP3/WAV player module with its own microSD slot and 3 W amplifier, UART controlled. Plays the alarm. [Datasheet](docs/DFPlayer__DFR0299_mp3-player-module_datasheet_V1.0.pdf) |
| Mini speaker | Pimoroni mini speaker, 4 Ω 3 W, on the DFPlayer's SPK1/SPK2. [Datasheet](docs/Pimoroni_mini-speaker_ST404004-30.pdf) |
| 470 uF capacitor | Across 3V3 + GND rails of microcontroller to handle wifi power spikes. |
| 1000 uF + 0.1 uF / 100nF capacitors | Across 5V + GRD rails of DFPlayer to handle playback power spikes. | 

## Wiring
Every pin is a `build_flag` in [platformio.ini](platformio.ini.default) — rewire freely
and update the flags to match.

<p float="left">
<img width="41%" src="./docs/pictures/connection-diagram-web.jpg" />
<img width="55%" src="./docs/pictures/ESP32-S3_DevKitC-1.png" />
</p>

### E-paper panel
The 4-color e-Paper panel has a long refresh time of about 25 seconds, where as the black-and-white version refresh time is much lower at about 5 seconds. 
This project uses the 4-color display, but I would recommend the quicker refresh display. 
Due to the strongly suggested idle time of 3 minutes between screen updates, the program enforces a delay between successive updates to guard against abuse. This delay resulted in the addition of a [LED countdown strip](#the-countdown-strip) of 6 RGB lights that count down until the next permitted screen refresh. These indicator lights serve as the visual notification of a pending screen update.  

Defaults follow Waveshare's own ESP32-S3 reference wiring.

This panel's interface is **nine** wires, not the usual eight — `PWR` is a
power-control input, and with it floating the panel never powers up.

| Panel | ESP32-S3 GPIO | Macro |
|---|---|---|
| VCC / GND | 3V3 / GND | — |
| DIN | 11 | `EPD_PIN_MOSI` |
| SCLK | 12 | `EPD_PIN_SCLK` |
| CS | 10 | `EPD_PIN_CS` |
| DC | 13 | `EPD_PIN_DC` |
| RST | 14 | `EPD_PIN_RST` |
| BUSY | 4 | `EPD_PIN_BUSY` |
| PWR | 5 | `EPD_PIN_PWR` |

Set `EPD_PIN_PWR` to `-1` only for an older 8-pin panel with no PWR pin.

The panel's mode switch must be in the **4-line SPI** position; the driver does
not implement 3-line mode.

#### Panel troubleshooting
BUSY on this panel reads **low while busy** and high when idle — the opposite
sense of Waveshare's monochrome panels. That makes an unpowered or disconnected
panel look identical to an idle one, so the driver cross-checks the timing:

- `BUSY stuck low` — the pin is held low past `EPD_BUSY_TIMEOUT_MS`. Usually
  miswired, or shorted.
- `refresh finished in N ms` — the refresh "succeeded" far too quickly, so BUSY
  was never asserted and the panel is not responding at all. Check PWR first,
  then the mode switch, then DIN/SCLK/CS/DC.

A silent success with a blank panel should not be possible; if you see one,
that check is what failed.

### Rotary encoder
Four wires over a STEMMA QT cable, which is most of the appeal: no quadrature
decoding, no debouncing, one connector.

| Encoder | ESP32-S3 GPIO | Macro |
|---|---|---|
| 3V3 / GND | 3V3 / GND | — |
| SDA | 8 | `ENC_PIN_SDA` |
| SCL | 9 | `ENC_PIN_SCL` |

GPIO 8 and 9 are adjacent on the header, are not strapping pins, and sit at the
opposite end of the board from the panel's 10–14 block, so the two cables leave
from different sides. The breakout answers at `0x36`, with solder jumpers for
`0x36`–`0x3D`. I2C is a bus, so a real-time clock or an audio codec added later
costs no further pins.

The breakout also carries a NeoPixel under the knob, on the same seesaw and so
on the same two wires — see [Choosing a screen](#choosing-a-screen) for what it
is used to say.

The QT connector carries only SDA, SCL, 3V3 and GND, so the breakout's `INT`
output is **not** wired and the encoder is polled rather than interrupt driven.
Little is lost by that: the seesaw counts detents and latches the button press
itself, so one delta read after a 25-second panel refresh still reports every
click made during it.

### Countdown strip
Six WS2812s in a line, one data wire, driven by RMT.

| Strip | ESP32-S3 GPIO | Macro |
|---|---|---|
| VDD / GND | **3V3** / GND | — |
| DIN | 21 | `GAUGE_PIN` |

**Power it from 3V3, not 5V.** A WS2812 wants 0.7 × V<sub>DD</sub> on its data
pin: against a 5 V rail that is 3.5 V, and GPIO 21 delivers 3.3 — which is the
classic intermittent-first-pixel failure. On 3V3 the threshold drops to 2.31 V
with comfortable margin. At `GAUGE_BRIGHTNESS` on a single colour the six pixels
draw well under 50 mA; pushing toward 255 on all three channels approaches
360 mA and would need the 5 V rail plus a level shifter.

GPIO 21 sits at the far end of the header from both the panel block (10–14) and
the I2C pair, so the strip cable routes on its own. See
[The countdown strip](#the-countdown-strip) for what it displays.

### DFPlayer Mini
Two wires of UART1 at 9600 baud. The module reads its own microSD card and
drives the speaker itself; the ESP32 only tells it which track to play.

| DFPlayer | ESP32-S3 GPIO | Macro |
|---|---|---|
| VCC / GND | **5V** / GND | — |
| RX | 17, through 1 kΩ | `DFPLAYER_PIN_TX` |
| TX | 18 | `DFPLAYER_PIN_RX` |
| BUSY | 3 | `DFPLAYER_PIN_BUSY` |
| SPK1 / SPK2 | speaker | — |

**Power it from 5V**, not 3V3 — in practice it wants 4.2–5.0 V, and on 3V3 it
fails to answer at all. The 1 kΩ in series with our TX is
DFRobot's advice against hiss. 17 and 18 are UART1's IO MUX pins on the S3, so
the signal leaves the pad directly. Note the macro names are from *our* side:
`DFPLAYER_PIN_TX` is the ESP32 pin that transmits, wired to the module's RX.

BUSY is low while a track plays. It is optional (`DFPLAYER_PIN_BUSY=-1`), but
with it the firmware knows from the module's own output pin whether sound is
being made, rather than from serial replies. GPIO 3 is one of the strapping
pins below, but only for JTAG source selection and only once an eFuse has been
burned to enable that, so on a stock chip it is an ordinary input. The S3's
pins are not 5 V tolerant, but the datasheet (section 6) gives the module's I/O
as 3.3 V TTL even on a 5V supply, so BUSY and TX connect directly. Worth one
multimeter reading on a new module all the same.

A swapped TX/RX pair is the classic fault, and from the ESP32 it looks exactly
like a module that is not there. The log says which pins it tried; the
[DFPlayer-test](../DFPlayer-test) project has a UART self-test for proving the
wiring on its own.

### Capacitors
These were added after some testing, particularly to the audio player module. The parallel large and small capacitors straddle the power and ground pins of the DFPlayerMini module specifically to absorb power demand spikes.
The same is true of the single capacitor on the two-pin e-Paper display power connector. 

![audio capacitors](./docs/pictures/capacitors-web.jpg)

### KiCad

[Espressif libraries for KiCad](https://github.com/espressif/kicad-libraries)
Notice the need for a second layer (see blue traces). These were implemented as patch wires soldered to the board.
<p float="left">
<img width="33%" src="./docs/pcb/pcb-layers.png" />
<img width="33%" src="./docs/pictures/populated-pcb-top-web.jpg" />
<img width="33%" src="./docs/pictures/populated-pcb-bottom-web.jpg" />
</p>

![Printed mask](./docs/pictures/pcb-mask-web.jpg)
PDFs of PCB Mask:

[Mask - positive](./docs/pcb/retro-clock-schematic.pdf)

[Mask - negative](./docs/pcb/retro_clock-rev.pdf) - what you want to use for UV-sensitive film

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
| `WIFI_MAX_TX_POWER_DBM` | `0` | Transmit power ceiling, 2–20 dBm; 0 = driver default (20). Lower it if Wi-Fi start-up trips the brownout detector |
| `WIFI_CONNECT_MAX_RETRY` | `0` | Attempts before reboot; 0 = retry forever |
| `WIFI_CONNECT_TIMEOUT_MS` | `15000` | Per-attempt deadline for assoc + DHCP |
| `WIFI_CONNECT_BACKOFF_MIN_MS` | `1000` | Initial delay between attempts |
| `WIFI_CONNECT_BACKOFF_MAX_MS` | `30000` | Backoff ceiling |
| `BOOT_LOG_DELAY_MS` | `5000` | Silent pause before the first log line |
| `NTP_SERVER` | `pool.ntp.org` | NTP server used to set the clock |
| `TIME_ZONE` | `EST5EDT,M3.2.0,M11.1.0` | Local timezone as a POSIX TZ string; **settable at runtime** |
| `NTP_SYNC_TIMEOUT_MS` | `30000` | Wait for the first NTP response before continuing |
| `NTP_SYNC_INTERVAL_MS` | `3600000` | Interval between background re-syncs |
| `EPD_PIN_SCLK` | `12` | Panel CLK |
| `EPD_PIN_MOSI` | `11` | Panel DIN |
| `EPD_PIN_CS` | `10` | Panel CS |
| `EPD_PIN_DC` | `13` | Panel DC (command/data select) |
| `EPD_PIN_RST` | `14` | Panel RST |
| `EPD_PIN_BUSY` | `4` | Panel BUSY (low = busy) |
| `EPD_PIN_PWR` | `5` | Panel PWR (power control); -1 only on 8-pin panels |
| `EPD_SPI_HOST` | `SPI2_HOST` | SPI peripheral driving the panel |
| `EPD_SPI_SPEED_HZ` | `10000000` | SPI clock; the panel is rated to 20 MHz |
| `EPD_BUSY_TIMEOUT_MS` | `60000` | Ceiling on one BUSY wait (a refresh takes ~25 s) |
| `ENC_PIN_SDA` | `8` | Encoder SDA |
| `ENC_PIN_SCL` | `9` | Encoder SCL |
| `ENC_I2C_PORT` | `I2C_NUM_0` | I2C peripheral the encoder hangs off |
| `ENC_I2C_SPEED_HZ` | `100000` | Bus clock; seesaw is an ATtiny bit-banging it |
| `ENC_I2C_ADDR` | `0x36` | seesaw address; jumpers give 0x36–0x3D |
| `ENC_POLL_INTERVAL_MS` | `50` | How often the encoder is read |
| `ENC_COUNTS_PER_DETENT` | `1` | Counts the breakout reports per physical click |
| `ENC_REVERSED` | `0` | 1 = clockwise walks the ring backwards |
| `ENC_POSITIONS` | `20` | Detents in one revolution; divided among the screens |
| `ENC_LED_PIN` | `6` | seesaw pin the knob's NeoPixel is wired to |
| `ENC_LED_BRIGHTNESS` | `40` | Out of 255; full brightness is painful |
| `ENC_FLASH_MS` | `150` | How long the knob lights up per turn |
| `ENC_BUTTON_PIN` | `24` | seesaw pin the knob's push switch is wired to |
| `ENC_PRESS_GAP_MS` | `250` | A second press inside this is switch bounce |
| `SCREEN_SETTLE_MS` | `1200` | Quiet time on the knob before the new screen is drawn |
| `GAUGE_PIN` | `21` | WS2812 countdown strip data line |
| `GAUGE_LED_COUNT` | `6` | Pixels on the strip |
| `GAUGE_BRIGHTNESS` | `40` | Out of 255, matching the knob |
| `GAUGE_TICK_MS` | `200` | How often the remaining time is re-read |
| `STATUS_LINGER_S` | `120` | How long the diagnostics screen stays up |
| `STATUS_MASK_API_KEY` | `0` | 1 = show only the first and last four characters of the key |
| `ALARM_ENABLED` | `1` | Whether a set alarm rings, until changed; **settable at runtime** |
| `ALARM_SOUND` | `"retro-alarm.wav"` | A name from `SOUNDS[]` in `alarm_sounds.c`; **settable at runtime** |
| `ALARM_RING_S` | `300` | How long the alarm loops before stopping on its own |
| `DFPLAYER_PIN_TX` | `17` | ESP32 TX, to the DFPlayer's RX |
| `DFPLAYER_PIN_RX` | `18` | ESP32 RX, from the DFPlayer's TX |
| `DFPLAYER_PIN_BUSY` | `3` | ESP32 input from the DFPlayer's BUSY; -1 if not wired |
| `DFPLAYER_VOLUME` | `20` | 0–30, the module's own scale |
| `DFPLAYER_RETRY_S` | `60` | How long to leave an unanswering player before trying again |
| `DFPLAYER_DEBUG` | `0` | 1 = print every frame to and from the module |
| `AUDIO_BOOT_TEST` | `0` | 1 = play the first sound once when the player first answers after power-up |
| `AUDIO_BOOT_TEST_MAX_S` | `30` | Longest the boot test plays before it is cut off |
| `MQTT_ENABLED` | `1` | 0 = leave the client out of the build |
| `MQTT_BROKER_URI` | *(required)* | e.g. `mqtt://10.0.0.200:1883` |
| `MQTT_USERNAME` | `""` | Empty for an anonymous broker |
| `MQTT_PASSWORD` | `""` | Secret; lives in the ignored `platformio.ini` |
| `MQTT_DIAGNOSTIC_TOPIC` | *(required)* | Topic the JSON is published to |
| `MQTT_PUBLISH_INTERVAL_S` | `300` | Seconds between publishes; **settable at runtime** |
| `MQTT_QOS` | `0` | |
| `MQTT_RETAIN` | `0` | Retain the diagnostics payload; availability and `get` are always retained |
| `MQTT_PUBLISH_API_KEY` | `0` | 1 = the key in the payload and on its `get` topic |
| `MQTT_AVAILABILITY_TOPIC` | `homeassistant/sensor/<WIFI_HOSTNAME>/availability` | Where online/offline goes |
| `MQTT_KEEPALIVE_S` | `30` | How fast the broker notices a dead device |
| `OWM_API_KEY` | *(required)* | OpenWeather API key; **settable at runtime** |
| `OWM_FORECAST_PATH` | `/data/2.5/forecast` | Endpoint; `/data/4.0/onecall` requires a credit card on file |
| `OWM_LATITUDE` | *(required)* | Latitude to forecast for, decimal degrees, quoted; **settable at runtime** |
| `OWM_LONGITUDE` | *(required)* | Longitude, decimal degrees, quoted; **settable at runtime** |
| `OWM_UNITS` | `metric` | `standard`, `metric` or `imperial` |
| `OWM_UNITS_ENUM` | `WX_UNITS_METRIC` | The same choice for the code, which cannot compare strings |
| `OWM_POLL_INTERVAL_S` | `1800` | Seconds between fetches; 1800 is 48 calls a day |
| `OWM_MAX_CALLS_PER_DAY` | `200` | Daily cap, well under the 1000/day free tier |
| `OWM_MIN_CALL_INTERVAL_S` | `600` | Floor between attempts; what stops a reboot loop |
| `WEATHER_MOCK` | `0` | 1 = render a canned response and make no network call |

Omitting `WIFI_SSID`, `WIFI_PASSWORD`, `OWM_API_KEY`, `OWM_LATITUDE` or
`OWM_LONGITUDE` fails the build with an `#error` rather than silently producing
firmware that cannot connect or cannot forecast. Latitude and longitude are
quoted strings, not numbers: they are only ever pasted into a URL, and that
sidesteps any argument about how a float is formatted.

To keep credentials out of version control, comment those two flags out and
export them at build time instead:

```sh
export PLATFORMIO_BUILD_FLAGS='-D WIFI_SSID=\"my-ssid\" -D WIFI_PASSWORD=\"my-pass\"'
pio run -t upload -t monitor
```


## Firmware Organization
Bootstraps the ESP32-S3 (NVS, TCP/IP stack, default event loop), brings up the
e-paper panel, joins a Wi-Fi network as a station, sets the real-time clock from
NTP, and hands the panel to the screen manager. Turning the rotary encoder walks
the ring of screens; pressing it silences the alarm.

- [src/main.c](src/main.c) — boot banner, bring-up, link/clock heartbeat
- [src/wifi_sta.c](src/wifi_sta.c) — station bring-up, retry/backoff, auto-reconnect
- [src/time_sync.c](src/time_sync.c) — SNTP client, timezone, background re-sync
- [src/epd_5in83g.c](src/epd_5in83g.c) — e-paper panel driver over SPI
- [src/epd_paint.c](src/epd_paint.c) — frame buffer shapes and a built-in 5x7 font
- [src/epd_pattern.c](src/epd_pattern.c) — the bring-up test pattern
- [src/epd_clockface.c](src/epd_clockface.c) — the analog clock face
- [src/epd_font.c](src/epd_font.c) — bitmap text; faces generated from the TTF
- [src/epd_weather.c](src/epd_weather.c) — the daily weather screen
- [src/epd_forecast.c](src/epd_forecast.c) — the two-day forecast screen
- [src/epd_status.c](src/epd_status.c) — the diagnostics screen
- [src/epd_alarm.c](src/epd_alarm.c) — the alarm screen
- [src/alarm.c](src/alarm.c) — rings at the set time; what the knob's button does
- [src/alarm_sounds.c](src/alarm_sounds.c) — sound names to DFPlayer track numbers
- [src/audio_player.c](src/audio_player.c) — the DFPlayer Mini, owned by one task
- [components/DFRobotDFPlayerMini](components/DFRobotDFPlayerMini) — DFRobot's driver, ported to ESP-IDF; vendored from DFPlayer-test
- [src/diagnostics.c](src/diagnostics.c) — the fields both the screen and MQTT report
- [src/settings.c](src/settings.c) — build flags that can be overridden at runtime
- [src/mqtt_report.c](src/mqtt_report.c) — diagnostics published as JSON
- [src/weather_daily.c](src/weather_daily.c) — three-hourly samples reduced to a day apiece
- [src/weather_aggregate.c](src/weather_aggregate.c) — hourly forecast into four parts of a day
- [src/weather_api.c](src/weather_api.c) — the HTTPS fetch
- [src/weather_parse25.c](src/weather_parse25.c) — the free 2.5 forecast
- [src/weather_moon.c](src/weather_moon.c) — moon phase from the date
- [src/weather_display.c](src/weather_display.c) — poll, compare, redraw
- [src/weather_quota.c](src/weather_quota.c) — API call budget in NVS
- [src/clock_display.c](src/clock_display.c) — five-minute update task
- [src/screen_manager.c](src/screen_manager.c) — which screen owns the panel, and when it may be written
- [src/rotary_encoder.c](src/rotary_encoder.c) — the seesaw encoder over I2C
- [src/refresh_gauge.c](src/refresh_gauge.c) — the WS2812 countdown strip over RMT
- [src/refresh_count.c](src/refresh_count.c) — lifetime refresh counter in NVS

## Display
The panel is a Waveshare 5.83" e-Paper (G): 648 x 480, four inks
(black/white/yellow/red), 4-wire SPI. The driver is a port of Waveshare's
[reference code](https://github.com/waveshareteam/e-Paper/tree/master/E-paper_Separate_Program/5in83_e-Paper_G)
onto the ESP-IDF SPI master driver, streaming the frame in DMA-sized bursts
instead of one chip-select per byte.

Frames are 2 bits per pixel, four pixels per byte, leftmost pixel in the high
bit pair — the panel's own packing, so `epd_display()` ships the buffer
verbatim. A full frame is 76 KB and is allocated from PSRAM when there is any.

Only a **478 x 460** window of the glass is usable — `EPD_USABLE_WIDTH` and
`EPD_USABLE_HEIGHT` in [src/epd_5in83g.h](src/epd_5in83g.h). It is flush to the
panel's left and bottom edges (`EPD_USABLE_X` 0, `EPD_USABLE_Y` 20), so the dead
margin is the 170 columns on the right and the 20 rows along the top.

Frame buffers stay the panel's full 648 x 480, because that is what the panel
clocks in, but the paint primitives take coordinates *relative to the usable
box* — (0,0) is its top-left, not the panel's — and clip to it, so the dead
margin can only ever carry the `paint_clear()` colour. Layout code works in the
box and never has to know about the rest.

`app_main()` brings the panel up before Wi-Fi, so a wiring fault surfaces
immediately rather than behind a network timeout. Neither an init failure nor a
BUSY timeout is fatal — the clock keeps time without a screen. Nothing is drawn
until the screen manager starts; calling `epd_pattern_show()` there instead
boots the test pattern. A full refresh takes roughly 25 seconds and blocks
whichever task runs it.

### Choosing a screen
[src/screen_manager.c](src/screen_manager.c) is the only thing that writes to the
panel. The screens — clock, alarm, weather dial, two-day forecast, status — are
laid out around **one revolution of the knob**. The encoder has twenty detents,
so each of the five screens occupies four of them:

```
position   1  2  3  4 | 5  6  7  8 | 9 10 11 12 |13 14 15 16 |17 18 19 20
screen     \_ clock _/ \_ alarm _/ \_ weather/ \_forecast/ \_ status _/
```

Turning *within* a screen's four changes nothing — the dial has somewhere to be,
rather than every click moving the panel. The order in `SCREENS[]` is the order
round the dial, and adding a screen means a line in that table and a draw
function, nothing else: the division is `position * SCREEN_COUNT /
ENC_POSITIONS`, so the alarm screen re-divided the dial from fives into fours on its own.
Counts that do not divide twenty spread the remainder rather than leaving a gap
— three screens take 7, 7 and 6. The bring-up test pattern is deliberately not
on the dial.

The position is software's notion, not the encoder's: a quadrature encoder has
no absolute reference and seesaw reports a running count, so this is that count
folded into a revolution. Which is also what lets it be **re-seated** when the
screen changes without the knob — the diagnostics screen showing itself out
moves the dial to the middle of the clock's span, so the next click behaves
rather than reading as "no change" while the glass shows something else.

Nothing else needs to be a screen manager, and that is the point: a refresh
blocks for 25 seconds and the datasheet asks for 180 between them, so *when
anything may be drawn at all* is one global question. Both content tasks
([clock_display.c](src/clock_display.c), [weather_display.c](src/weather_display.c))
stopped drawing and now only say when what they would draw has changed; a change
to a screen that is not showing is remembered and drawn when the knob arrives
there.

Two rules follow from those 25 seconds, and neither is optional:

- **A turn is not acted on at once.** The ring has to stand still for
  `SCREEN_SETTLE_MS` first. Spinning past three screens is one refresh, on the
  screen the hand stopped at, rather than three refreshes ending in the same
  place.
- **A turn made mid-refresh is discarded, not queued.** Queueing it would move
  the screen half a minute after the knob stopped, and a knob idly spun through
  a refresh would leave a run of them waiting. The encoder task keeps reading
  during a refresh precisely so those counts are consumed and thrown away rather
  than sitting in the seesaw waiting to fire.

The 180-second floor applies to a knob turn like any other refresh, so a second
switch inside three minutes waits. In practice the settle window absorbs most of
that: the case it bites is two deliberate turns a minute apart, and the panel is
rated for a finite number of refreshes.

#### What the knob's light means
Between the settle delay, the 180-second floor and a 25-second refresh, a turn
can be accepted and still change nothing visible for minutes. On a screen that
slow, silence is indistinguishable from a broken encoder, so the NeoPixel under
the knob answers immediately — traffic lights, because that is what the hand on
the knob is actually asking:

| | Meaning |
|---|---|
| 🟢 **green** | Accepted, and drawing as soon as you let go of the knob |
| 🟡 **amber** | Accepted, but the panel's 180-second floor has not elapsed — the [countdown strip](#the-countdown-strip) shows how much longer |
| ⚪ **white** | Counted, but the dial is still within the screen it was already on — keep turning |
| 🔴 **red** | Discarded: the panel is mid-refresh |

White is off to one side of the traffic-light scheme on purpose, because it is
neither a go nor a stop. It matters more than it looks: with four detents to a
screen, **most clicks land on it**, and a green flash followed by a panel that
never changes would read as a fault.

The verdict is decided by `screen_manager_step()` at the instant of the turn and
handed back to the encoder driver, which owns the pixel because it is on the same
seesaw. Nothing outside [rotary_encoder.c](src/rotary_encoder.c) knows a NeoPixel
exists. Green accounts for the settle window in its arithmetic — that time counts
towards the floor, so a turn a second short of 180 is honestly green rather than
amber-then-immediate.

The flash is re-armed rather than queued, so keeping the knob turning during a
refresh reads as steady red rather than a stutter, and it is timed out by the
poll loop rather than slept through — a turn is never waiting behind a blink.

#### The countdown strip
Amber says you are waiting. Six WS2812s say **how much longer**. When a screen
change is accepted but held off by the 180-second floor, the strip fills and then
empties as the wait runs down, all six going dark as the refresh begins.

The colour is the screen that is *coming*, not the one showing — 🟡 yellow for
the clock, 🔴 red for the alarm, 🔵 blue for the weather dial, cyan for the
two-day forecast and violet for status. Those live in `SCREENS[]` in
[screen_manager.c](src/screen_manager.c) beside the name and draw function, on the
same principle that put the knob's verdict colours inside the encoder driver:
whoever owns the vocabulary owns the mapping. The gauge knows only red, green and
blue.

If there is no current refresh delay on the display (because it hasn't been updated in more than 3 minutes), then the LED countdown will never light up, as there is no need for it, since the screen change will happen immediately. 

The scale is **relative** — always six at the start, emptying over whatever the
wait happens to be. Six means "the wait has begun", not "180 seconds". It only
appears for a deferred *screen change*: a content refresh held off by the same
floor (the hands stepping, a new forecast) waits in the dark, because a strip
lighting up untouched would read as the device acting on its own.

Two details in [screen_manager.c](src/screen_manager.c) are load-bearing and easy
to undo by accident:

- **The countdown is anchored in module state, not measured afresh each time the
  wait is entered.** The clock face pokes the screen manager every five minutes
  whether or not it is showing, so a three-minute wait is *more likely than not*
  to be interrupted by something unrelated to the knob. The manager settles, finds
  the same change still pending, and resumes — and a countdown restarted at that
  moment would refill the strip. The user would watch 6, 5, 4, 6, 5, 4. Anchored,
  it resumes at 4.
- **An interruption puts the strip out but keeps the countdown.** So the visible
  behaviour is a blink: dark for one settle window, then relit at the level it had
  reached, or in a new colour if the knob moved somewhere else.

The driver in [refresh_gauge.c](src/refresh_gauge.c) is hand-rolled on `esp_driver_rmt`
rather than pulling in the `led_strip` managed component, which would have been the
first thing in this tree not built from framework-espidf. Six pixels is eighteen
bytes. Three of its constants are not arbitrary: the RMT channel takes **all four**
TX memory blocks so the whole frame clocks out of hardware with no refill interrupt
racing the Wi-Fi stack; the bit timings are cut at 20 MHz rather than 10 so they
land inside the WS2812, WS2812B *and* SK6812 windows; and every frame is preceded by
a 300 µs low gap. That last one looks redundant — frames are a fifth of a second
apart — but on the one path where a countdown is interrupted, the clear follows its
own show by microseconds, and without the gap the strip reads the second frame as a
continuation of the first and stays lit with no way to put it out.

### Test pattern
The boot pattern is a bring-up check, laid out inside the 478 x 460 usable box
and readable at a glance:

- **All four inks** — labelled swatches, one per colour code (0–3).
- **Orientation** — every edge is named, a red arrow points at the top edge,
  and each corner carries both a coordinate label and a distinct shape (red
  square, yellow disc, black triangle, four-ink checker). Reading the text also
  catches a mirrored scan direction, which a symmetric pattern would hide.
- **Geometry** — a frame flush to all four sides of the usable box plus edge
  ticks spanning corner to corner; a row-stride error skews them, a wrong
  resolution register stops them short. Corner labels read `(477,0)` and
  `(477,459)` — box coordinates, not the panel's — so a frame that does not sit
  hard against the visible edges, or a strip of white above the top one, means
  the crop is wrong.

_The full panel size is not used!_
This project needed a smaller panel size that could not be found, so a larger one was used instead. 

![test pattern](./docs/pictures/pattern460.png)

### Clock face
[src/epd_clockface.c](src/epd_clockface.c) draws the dial inspired by
[docs/pictures/clock-face.png](docs/pictures/clock-face.png): outlined numerals
outside a two-line chapter ring, an inner minute track with wedges on the fives,
cross lines running out to the edges.

The hands follow the pair in the photograph: solid black tapered wedges, broad
at the boss and narrowing to a blunt tip, with the boss drawn over both. No
second hand — see the cadence below for why there could not be one.

![real clock](./docs/pictures/clock-face.png)
![clock](./docs/pictures/hclock.png)

#### Type
Both screens set type from `fonts/Aesthetic Rigelsia Free.ttf`, rasterised to
bitmaps by [tools/make_font.py](tools/make_font.py) into the generated
[src/epd_font_data.c](src/epd_font_data.c). The panel has no font engine, so the
glyphs are baked at the sizes they are drawn:

| Face | Size | Charset | Used by |
|---|---|---|---|
| `FONT_DIAL` | pt60 | digits | clock numerals |
| `FONT_TITLE` | pt40 | mixed case | the weather heading |
| `FONT_LABEL` | pt26 | caps and digits | everything smaller |

Rasterising uses ImageMagick, which is the only thing to hand that reads a
TrueType outline. Per glyph it renders at a known pen position and finds the ink
box — that yields the offset from the *baseline*, without which ascenders and
descenders cannot be aligned — and measures the advance as
`width("cc") - width("c")`, since ImageMagick reports a label's extent but not a
glyph's advance. Sizes are fixed rather than scaled: a thin Deco face carries its
character in two- and three-pixel strokes, and scaling a bitmap that fine
destroys the thing worth keeping. Below about pt24 the strokes start breaking up
when thresholded to one bit, which is what sets `FONT_LABEL`.

All type is black. The dial used to set its numerals in yellow behind a black
outline, which suited the blocky face it had then; a one-pixel outline on a
three-pixel stem leaves nothing.

## Alarm
One alarm, on a 24-hour clock, set over MQTT and switched on and off with the
knob. When the local time reaches it the DFPlayer loops the chosen sound for
five minutes (`ALARM_RING_S`) and then stops on its own.

![alarm](./docs/pictures/alarm.png)

### Setting it
Three parameters, with the same `get`/`set` topics as every other
[runtime parameter](#changing-parameters-over-mqtt) and kept in NVS the same way:

| Parameter | Topic | Values |
|---|---|---|
| `ALARM_TIME` | `homeassistant/text/<host>/ALARM_TIME/<get\|set>` | `HH:MM`, 24-hour; empty = no alarm |
| `ALARM_ENABLED` | `homeassistant/switch/<host>/ALARM_ENABLED/<get\|set>` | `ON` / `OFF` |
| `ALARM_SOUND` | `homeassistant/text/<host>/ALARM_SOUND/<get\|set>` | a file name from `SOUNDS[]` |

```sh
mosquitto_pub -h 10.0.0.200 -u mqtt-user -P ...  \
    -t 'homeassistant/text/retro-clock-radio/ALARM_TIME/set' -m 06:45
```

`ALARM_TIME` breaks the rule every other parameter follows. Elsewhere a value
that does not validate is **rejected** and the old one stays. Here, an empty
payload, `null`, or **anything that does not parse as a 24-hour time unsets the
alarm**. `6:45`, `06:45` and `06:45:00` (what a Home Assistant time helper sends)
are all accepted and stored as `06:45`; `24:00`, `6:45pm` and `0645` all mean
"no alarm". The get topic then goes empty, which also clears its retention.

`ALARM_ENABLED` takes `ON`/`OFF` and the usual spellings of them (`on`, `true`,
`1`…); anything else is rejected. It **follows the alarm time**: while
`ALARM_TIME` is empty it is held `OFF` — a set of `ON` is rejected, and the get
topic is republished as `OFF` to say so. Clearing the time switches it `OFF`;
setting a time where there was none switches it to its default
(`ALARM_ENABLED=1` means `ON`). Changing an already-set time leaves it alone. `ALARM_SOUND` accepts `retro-alarm.wav` or
`./sfx/retro-alarm.wav` alike, and rejects a name not in the table. There is no
build flag for the time itself: "no alarm" has to be what an empty payload goes
back to, and a compiled-in time would make that impossible.

### The knob's button
The button did nothing before the alarm, and on most screens still does not.
Two exceptions, in this order:

1. **While the alarm is ringing**, a press stops it — on any screen. This is
   checked first, so the press that silences the alarm on the alarm screen
   never also switches tomorrow's off.
2. **With the alarm screen showing**, a press switches the alarm on or off —
   unless no time is set, in which case it stays off and the knob stays dark. The
   change goes through `settings_set()`, so it is stored, published on the get
   topic, and the screen redraws through the usual path (the 180-second floor
   still applies).

A press that did something flashes the knob green; one that did nothing leaves
it dark, exactly as before. Switching the alarm off over MQTT while it is
ringing stops it too. "Showing" means where the knob points, so a press during
the wait for the alarm screen to be drawn counts.

The alarm rings once per matching minute. Silencing it inside that minute does
not set it off again, and an alarm set for the minute it already is goes off
at once — the least surprising reading of "set it for now".

### The sound card
The DFPlayer plays by **number**, not by name. The card follows the layout in
the module's [datasheet](docs/DFPlayer__DFR0299_mp3-player-module_datasheet_V1.0.pdf)
(section 3.4.4): folders in the root named with two digits, `01`–`99`, holding
files named with three digits and nothing else, `001.mp3`–`255.mp3`. The
firmware asks for "folder 01, track 3" (`DF_playFolder()`, command `0x0F`) and
the module plays `/01/003.mp3`:

```
/01/001.mp3     retro-alarm.wav
/01/002.mp3     phony-buzzing-alarm-fx_99bpm.wav
...
```

The bare numbers say nothing about which sound is which, so the script prints
the mapping as it writes the card.

Which number is which sound is `SOUNDS[]` in [alarm_sounds.c](src/alarm_sounds.c),
and nowhere else. [tools/make_sd_card.py](tools/make_sd_card.py) reads that
table and writes [sfx/](sfx/) onto a FAT16/FAT32 card (32 GB at most) to match.

**The sounds go on the card as MP3, not WAV.** The sources in `sfx/` are WAV and
stay that way, but the script converts each one with ffmpeg as it writes it. On
this radio's module a WAV "played" for its full length — the module confirmed
it was playing, and the pass took exactly the file's duration — in complete
silence, while MP3s played fine. The datasheet lists MP3 (and WMA) decoding and
never mentions WAV. The MP3s are the plainest
kind there is: constant 128 kbps, 44.1 kHz stereo, no ID3 tags, no VBR header.

The script also replaces any other file claiming one of our numbers — such as
`001-retro-alarm.wav` from an older layout — and removes
macOS `._` and `.DS_Store` files, which the module can mistake for tracks.
`ffmpeg` must be installed.

```sh
python3 tools/make_sd_card.py /media/$USER/SDCARD
```

To add a sound, put it in `sfx/`, add a line to `SOUNDS[]`, rebuild, and rerun
the script.

### The player
DFRobot's driver ([components/DFRobotDFPlayerMini](components/DFRobotDFPlayerMini),
vendored from the DFPlayer-test project) is a port of their Arduino library and
keeps its habits: all of its state is global, it **busy-waits** for each
acknowledgement, and its serial layer hands every transmission back to whichever
task started the UART. So [audio_player.c](src/audio_player.c) gives it one task
of its own, at priority 1, which makes every `DF_*` call; everyone else posts
requests to a queue. Resetting the module blocks that task for about two
seconds, which costs nothing anyone else wanted.

The sound loops by being restarted each time a pass ends. With BUSY wired, a
pass has ended when BUSY has gone low since the start and is high again. Without
it, the firmware falls back to the module's serial "finished" message, which
arrives **twice** for every track, so a finish within a second of a start is
ignored as the echo. A missing track or a pulled card stops the loop
rather than retrying it for five minutes.

Because "the module accepted the command" and "sound is coming out" are
different things, the log reports what the module itself says:

```
I audio: DFPlayer online on TX 17 / RX 18: volume 20 of 30 (asked for 20), 5 file(s) in /01
I audio: BUSY on GPIO 3 reads idle, as it should
I audio: BUSY confirms /01/001 is playing
I audio: first pass of /01/001 finished after 4023 ms (BUSY went high); looping
```

No files in `/01` means the card layout is wrong. "NOT playing" a moment after
the start, or a first pass that finishes in a fraction of the sound's length,
means the file is in a form the module cannot decode. A confirmed play that
runs the right length with no sound is the WAV failure described above — or, with
an MP3, the speaker wiring or the volume.

The library's queries take *whatever message is waiting* as their answer, so
one stray message leaves every later answer one behind — which is how the
volume once read `-1` and the file count read back as the volume. Each query is
preceded by a short pause and a drain of anything pending for that reason.

No player is not a fault. It is retried every `DFPLAYER_RETRY_S` and whenever
the alarm goes off; until it answers, the alarm is silent and the log says why.

## Status screen
A diagnostics screen: what the radio joined, what it answers on, what it
forecasts with, how much life the panel has left and how long it has been up.

![status](./docs/pictures/status.png)

Set in the plain **5x7 font**, not the Deco face the other screens use. That is
not a shortcut — this is the one screen read character by character, and a
decorative face makes `8` and `B`, `0` and `O` harder to tell apart, which is
exactly the distinction a MAC address or an API key turns on.

The SSID is the network **actually associated with**, read from
`esp_wifi_sta_get_ap_info()` rather than from the `WIFI_SSID` the firmware was
built with. On a screen whose job is to answer "what is this thing talking to",
the configured value is the one answer that cannot be trusted. The MAC comes
from efuse, so it is printable before the radio has ever come up.

The three MQTT rows are compiled in and out with the client, so a build with
`MQTT_ENABLED=0` does not carry three lines about a broker it will never talk
to. Where a field is one of the [runtime-settable
parameters](#changing-parameters-over-mqtt), the screen shows the **live**
value, not the one the firmware was flashed with — which is the point of having
it on a diagnostics screen at all. The broker URI is printed as configured — worth remembering that a URI of
the form `mqtt://user:pass@host` would put those credentials on the glass, and
that e-paper keeps its last image with no power at all.

A value too wide for its column runs on under the label instead, in as many
lines as it takes. The topic is the reason: at fifty characters it does not fit
across the box even at full width, so it takes two. The split is blind rather
than on word boundaries, because URIs, topics and keys have no words to break
at. Dropping to scale 1 would fit them whole and is not an option — this panel
renders a one-pixel stroke grey, which is why the type here is scale 2 in the
first place.

Eleven rows and fourteen lines is the worst case, with every MQTT field present
and both the key and the topic wrapped. That fits with about twenty pixels to
spare, which is what `ROW_STEP` is set from; adding a field means checking it
again.

### It shows itself out
A clock radio left showing a diagnostics page is a clock radio nobody can read
the time on, so this screen hands the panel back on its own. `SCREENS[]` in
[screen_manager.c](src/screen_manager.c) gained a `linger_s` and a `then`, zero
meaning "stay until the knob says otherwise" — which is what every other screen
wants, and which keeps the behaviour in the same table as everything else about
a screen rather than special-cased in the task.

The timer is armed **after** the refresh completes, not when the screen is
asked for, so `STATUS_LINGER_S` is time the screen is readable rather than time
half-eaten by its own 25-second refresh. Turning the knob cancels it: a hand on
the dial outranks a timer.

One honest wrinkle: the panel's 180-second floor outranks both. The switch is
*requested* at 120 s and the panel actually changes at 180 s, with the countdown
strip showing yellow for the clock it is on its way to. Set `STATUS_LINGER_S` to
180 if you would rather the two numbers agree.

### The API key is on there
It is printed in full by default, because a diagnostics screen that cannot tell
you which key was flashed is not doing its job. Worth knowing before leaving it
that way: **e-paper holds its last image with no power at all.** The key stays
legible on the glass after the radio is unplugged, and survives into any
photograph of the device. `STATUS_MASK_API_KEY=1` prints the first and last four
characters instead, which is still enough to tell two keys apart.

## MQTT
Twenty-two topics, all under `homeassistant/`, with `retro-clock-radio` throughout
coming from `WIFI_HOSTNAME`. **D→B** is published by the device, **B→D** is
subscribed to by it, and **broker** is published by the broker on the device's
behalf.

| Topic | Dir | Retained | Effect | Payload |
|---|:--:|:--:|:--:|---|
| `homeassistant/sensor/retro-clock-radio/diagnostics` | D→B | per `MQTT_RETAIN` | n/a | JSON, every `MQTT_PUBLISH_INTERVAL_S` |
| `homeassistant/sensor/retro-clock-radio/availability` | D→B | yes | n/a | `online`, on every connect |
| `homeassistant/sensor/retro-clock-radio/availability` | broker | yes | n/a | `offline`, as the last will |
| `homeassistant/number/retro-clock-radio/OWM_LATITUDE/get` | D→B | yes | current | e.g. `43.9` |
| `homeassistant/number/retro-clock-radio/OWM_LATITUDE/set` | B→D | — | immediate | a decimal, −90 to 90 |
| `homeassistant/number/retro-clock-radio/OWM_LONGITUDE/get` | D→B | yes | current | e.g. `-79.3` |
| `homeassistant/number/retro-clock-radio/OWM_LONGITUDE/set` | B→D | — | immediate | a decimal, −180 to 180 |
| `homeassistant/text/retro-clock-radio/TIME_ZONE/get` | D→B | yes | current | e.g. `EST5EDT,M3.2.0,M11.1.0` |
| `homeassistant/text/retro-clock-radio/TIME_ZONE/set` | B→D | — | immediate | POSIX TZ, 3–63 printable, no spaces |
| `homeassistant/text/retro-clock-radio/OWM_API_KEY/get` | D→B | yes | current | masked unless `MQTT_PUBLISH_API_KEY=1` |
| `homeassistant/text/retro-clock-radio/OWM_API_KEY/set` | B→D | — | immediate | 16–63 printable, no spaces |
| `homeassistant/number/retro-clock-radio/MQTT_PUBLISH_INTERVAL_S/get` | D→B | yes | current | e.g. `300` |
| `homeassistant/number/retro-clock-radio/MQTT_PUBLISH_INTERVAL_S/set` | B→D | — | immediate | whole seconds, 5 to 86400 |
| `homeassistant/text/retro-clock-radio/WIFI_SSID/get` | D→B | yes | returns any override value only | the override, or empty |
| `homeassistant/text/retro-clock-radio/WIFI_SSID/set` | B→D | — | after reboot | 1–32 printable |
| `homeassistant/text/retro-clock-radio/WIFI_PASSWORD/get` | D→B | yes | returns any override value only | the override, or empty |
| `homeassistant/text/retro-clock-radio/WIFI_PASSWORD/set` | B→D | — | after reboot | 8–63 printable (WPA2) |
| `homeassistant/text/retro-clock-radio/MQTT_BROKER_URI/get` | D→B | yes | returns any override value only | the override, or empty |
| `homeassistant/text/retro-clock-radio/MQTT_BROKER_URI/set` | B→D | — | after reboot | `mqtt://`, `mqtts://`, `ws://` or `wss://` + host |
| `homeassistant/text/retro-clock-radio/MQTT_USERNAME/get` | D→B | yes | returns any override value only | the override, or empty |
| `homeassistant/text/retro-clock-radio/MQTT_USERNAME/set` | B→D | — | after reboot | 1–63 printable |
| `homeassistant/text/retro-clock-radio/MQTT_PASSWORD/get` | D→B | yes | returns any override value only | the override, or empty |
| `homeassistant/text/retro-clock-radio/MQTT_PASSWORD/set` | B→D | — | after reboot | 1–63 printable |

Note that some parameters are set in non-volatile memory and are only effective after device reboot. 
The status display will show the current value (which may be different from the stored value). This stored value only becomes the current value after reboot. 

Watch the lot:

```sh
mosquitto_sub -h 10.0.0.200 -u mqtt-user -P ... -v \
    -t 'homeassistant/+/retro-clock-radio/#'
```

### Diagnostics
The status screen's fields, plus the Wi-Fi signal strength, published as a JSON
object every `MQTT_PUBLISH_INTERVAL_S`:

```json
{"ssid":"ETHERCI5","ip":"192.168.137.214","mac":"a0:85:e3:4b:19:7c",
 "rssi":-57,"timezone":"EST5EDT,M3.2.0,M11.1.0",
 "latitude":"43.9","longitude":"-79.3","owm_api_key":"...",
 "screen_refreshes":48217,"uptime_minutes":4584,"uptime":"3d 04h 24m",
 "version":"1.0.0"}
```

Uptime appears twice on purpose: `uptime_minutes` is for whatever is graphing
it, `uptime` is for a person reading the topic by hand.

The payload is assembled with cJSON rather than `snprintf`. That is not
fastidiousness — an SSID is arbitrary bytes, and one containing a quote or a
backslash would otherwise produce a payload no subscriber could parse. Round
tripping an SSID of `ETHER"CI5\x` through the broker is part of how this was
checked.

[src/diagnostics.c](src/diagnostics.c) gathers the fields and both consumers
read from it, so the panel and the broker cannot come to disagree about what
the device is, and the API key is masked by one rule wherever it appears.

### Availability, and who publishes "offline"
A second topic carries nothing but the words `online` and `offline`:

```
homeassistant/sensor/<WIFI_HOSTNAME>/availability
```

The device publishes only the first of those. The second is a **last will**:
handed to the broker inside the CONNECT packet, before this client has sent
anything at all, and published by the broker *on the device's behalf* when it
stops answering. That is the whole point of the mechanism — a radio that has
crashed, been unplugged or fallen off the Wi-Fi cannot announce it, so the
broker is asked in advance to do it.

Both are **retained**, so a subscriber connecting later is told the current
state immediately rather than waiting for it to change.

`online` is published from the connect event rather than the reporting task, so
it goes out on every reconnect and goes out at once: waiting for the next
interval would leave the device reading `offline` for minutes after it had
plainly come back.

Two things about last wills that are easy to be caught by:

- **A clean disconnect does not trigger one.** If a client sends a proper
  DISCONNECT the broker drops the will unsent, by design. This firmware never
  stops its client, so the only way out is the ungraceful one — which is the
  case worth detecting anyway.
- **"Stops answering" is measured by the keepalive.** esp-mqtt defaults to
  120 s, so a device that died a moment after checking in would read `online`
  for two and a half minutes. `MQTT_KEEPALIVE_S` is set to 30 instead: the
  client checks in every 15, and the broker gives up within about 45. The cost
  is a couple of two-byte packets a minute.

### Changing parameters over MQTT
Thirteen parameters can be changed without a reflash, each with a `get` topic the
device publishes to and a `set` topic it listens on:

```
homeassistant/<number|text|switch>/<WIFI_HOSTNAME>/<PARAM>/<get|set>
```

The component in the second position follows the parameter's type — `number`
for the numeric ones, `text` for the alphanumeric, `switch` for the one
boolean, `ALARM_ENABLED`. Adding a parameter is a line in `TABLE[]` in
[settings.c](src/settings.c) and the topics fall out of it.

Update a parameter:
```sh
mosquitto_pub -h 10.0.0.200 -p 1883 -u mqtt-user -P ...  \
    -t 'homeassistant/number/retro-clock-radio/MQTT_PUBLISH_INTERVAL_S/set' -m 600
```

Read a parameter:
```sh
mosquitto_sub -h 10.0.0.200 -p 1883 -u mqtt-user -P ...  \
    -t 'homeassistant/number/retro-clock-radio/MQTT_PUBLISH_INTERVAL_S/get'
# 600
```

Every `get` topic is **retained**, so a subscriber connecting later is told the
current value without asking. A `set` is answered by republishing the `get`
whether or not it was accepted — a rejected value leaves the sender believing
something untrue, and republishing is the only acknowledgement this protocol
has. An **empty payload resets** a parameter to its compile-time default, which
is also what clearing a retained `set` topic looks like, so the two agree.

Overrides live in NVS beside the refresh counter, which means they **survive a
reflash as well as a reboot**. That is the point, and it is also the thing to
remember when a changed build flag appears to do nothing: the stored value wins
until it is reset.

### Two kinds of parameter
The first eight — the five above and the three [alarm](#alarm) parameters — are
**live**: a new value is adopted as it arrives. Five more are
**boot settings** — `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_BROKER_URI`,
`MQTT_USERNAME`, `MQTT_PASSWORD` — and those are only written down. They are
picked up at the next start.

That is not laziness. They are how the radio reaches the network and the broker
in the first place, so applying one live would mean tearing down the connection
the instruction arrived on. The useful consequence is that **"in use" and
"stored" are two different things for the length of an uptime**, and the get
topic answers the second:

| | `settings_get()` — in use | `settings_reported()` — the get topic |
|---|---|---|
| fresh flash | the build flag | *(empty)* |
| after `SSID/set = B` | still A | B |
| after a restart | B | B |
| after `SSID/set = ""` | still B | *(empty)* |
| after a restart | the build flag | *(empty)* |

An **empty get topic means "not overridden"** — this one is still on whatever
was flashed. Because it is published as a zero-length retained message, the
retention is cleared too, so nothing really is nothing.

The status screen deliberately shows the **in-use** values, so after setting a
new SSID the screen and the getter will disagree until the radio restarts. That
is the design, not a bug.

The `WIFI_PASSWORD` and `MQTT_PASSWORD` get topics are **unmasked**. They are
only ever non-empty when someone set them over this same broker, so masking
would hide a value from the one party already known to have it. `OWM_API_KEY` is
different — it comes from the firmware, so it can leak something the broker was
never told, and it follows `MQTT_PUBLISH_API_KEY`.

### Locking yourself out
Setting `WIFI_SSID` or `WIFI_PASSWORD` to something wrong gives you a radio that
reboots, fails to join, and **cannot be told so over MQTT** — the only channel
for fixing it needs the network it can no longer reach. The broker URI has the
same shape of risk.

Validation covers the obvious slips: a URI must carry a scheme esp-mqtt knows
plus a host, an SSID must be 1–32 characters, a passphrase 8–63. It cannot
check that a network exists or that a password is right.

Recovery is physical:

```sh
pio run -t erase       # wipes NVS, so every override goes with it
pio run -t upload
```

Note that erasing takes the refresh counter with it, which is the one piece of
state in there meant to outlive everything.

### Taking effect instantly
Storing a value is the easy half. Each parameter also has to reach whatever was
already using it:

- **`TIME_ZONE`** is applied with `setenv`/`tzset` inside `settings_set()`, so
  every later call to `localtime_r` is already right.
- **`OWM_LATITUDE`, `OWM_LONGITUDE`, `OWM_API_KEY`** go into the forecast URL,
  which stopped being a compile-time concatenation and is now built per fetch.
  Moving the coordinates also wakes the polling task: a new location is no use
  if the glass stays half an hour behind the move. The call budget still
  applies, which is what stops that being abusable.
- **`MQTT_PUBLISH_INTERVAL_S`** is read each pass of the publishing loop, and
  the loop sleeps on a notification rather than a plain delay so a change cuts
  the current sleep short. Going from an hour to ten seconds is otherwise the
  difference between "instantly" and "in an hour".
- **The screens** that show these values are marked stale, so they redraw
  through the usual path — which means the panel's 180-second floor still
  applies and a flurry of changes still costs one refresh.

### These are state topics, not discovery topics
Everything here sits under Home Assistant's `homeassistant/` prefix, and none of
it is a discovery message — HA looks for `.../config` there and will not create
an entity from any of these on its own. Point `mqtt` sensor, number and text
entities at the topics, or publish discovery payloads separately. Worth knowing
before wondering why nothing appeared.

### Where the API key ends up
One switch, `MQTT_PUBLISH_API_KEY`, governs both places the key could leave the
device. It is **0** in this build, so:

| | With `0` (this build) | With `1` |
|---|---|---|
| `diagnostics` payload | field omitted | the key |
| `OWM_API_KEY/get` | `6f1e...8a7b` | the key |
| `OWM_API_KEY/set` | works | works |

Setting it still works either way — reading and writing are separate concerns,
and it is only the reading that broadcasts. `STATUS_MASK_API_KEY=1` shortens the
key on the status screen as well.

The reason for the care: port 1883 is **plaintext on the wire**, a retained
topic holds its value until something replaces it, and a Home Assistant that
ingests a payload files it in the recorder database and shows it in history. A
get topic that said nothing at all would be worse than a masked one, which is
why `0` masks rather than omits there.

And the shape of the feature as a whole, plainly: anyone who can publish to this
broker can change where the device thinks it is, which key it bills against, and
what time it believes it is. That is what was asked for. The broker's own
authentication is the only thing standing in front of it.

## Weather

Weather data is supplied by the free tier at [OpenWeather.org](https://openweathermap.org/).
This uses the version [2.5 of the API](https://openweathermap.org/api/current?collection=current_forecast), which does not require any kind of payment or even a credit card on file. You simply register for an API key.
*This is the version used by this application.*

The latest [version 4.0 of the One Call API](https://openweathermap.org/api/one-call-4?collection=one_call_api) has more features and it is also free up to a sufficient number of daily requests, but requires a credit card on file (even if the free usage is not exceeded).

---
[src/epd_weather.c](src/epd_weather.c) draws the screen:
the date across the top, a circle quartered into the four parts of the day, each
with an icon, a condition and a temperature, sunrise and sunset in the margins,
and the moon phase at the foot.

![weather dial](./docs/pictures/anglesc.png)

The circle is a **twenty-four hour dial**: noon at the top, midnight at the foot,
running clockwise, so morning is upper-left, afternoon upper-right, evening
lower-right and night lower-left.

Noon and midnight never move, so that divider is always vertical. Sunrise and
sunset are drawn **where they actually fall**. A six o'clock sunrise lands due
left and a six o'clock sunset due right, giving the flat horizontal of the
mock-up; anything else tilts. A late sunrise swings the left divider up above the
horizontal, an early one dips it below, and the sunset divider does the same on
the right. The four parts of the day are therefore drawn at the size they really
are — a long June evening looks long — and the sunrise and sunset icons ride
their own dividers up and down with them.

Sectors are inked by whether the sun is up: **yellow while it is, black once it
is not**, with the text and icons black on yellow and white on black. Two
consequences worth knowing:

- The proportion of yellow *is* the day length. A December dial is mostly black,
  a June one mostly yellow, and that reads before any of the words do.
- The noon-to-midnight divider is drawn black above the centre and white below.
  Above, it separates two yellow sectors; below, two black ones, where a black
  line would not exist. The sunrise and sunset dividers are not drawn at all —
  they separate a yellow sector from a black one, and that edge is already the
  sharpest thing on the screen.

The moon sits wholly inside the rim rather than straddling it as the mock-up
shows, because the foot of the dial is now black: the glyph is drawn white and
has to stay on that background. Its label drops below the rim onto the page.

Each sector's contents sit out along the middle of its arc, at a radius computed
from the width of that sector's own label. A wedge widens as it goes out, so a
narrow one pushes its contents outward to find room; the test is against the
group's *innermost corner*, which is the first thing a divider reaches. When even
the outermost usable radius is too tight — a deep summer evening can be 45° —
the icon is dropped and the text pushed further out, because a divider drawn
through the word is worse than no picture.

| Part | Runs from | to |
|---|---|---|
| Morning | sunrise | local noon |
| Afternoon | local noon | sunset |
| Evening | sunset | local midnight |
| Night | local midnight | sunrise |

### Where the four conditions come from
A One Call `daily` entry carries **one** condition for the whole day; only its
*temperatures* are split into morning, day, evening and night. The four
conditions therefore have to be aggregated out of the 48-hour `hourly` array,
which is what [src/weather_aggregate.c](src/weather_aggregate.c) is for.

Selection is tiered rather than a straight "worst wins". OpenWeather reports
`500 light rain` for any hour with a trace of modelled precipitation, including
ones it gives a six percent chance, so ranking those straight up a severity
ladder would paint most of the year as rain and the icon would stop meaning
anything.

- **Hazards** — thunderstorm, snow, sleet, squall — take the window on a single
  hour. One storm hour in an otherwise clear afternoon is exactly what must not
  be averaged away.
- **Rain** must either persist for a quarter of the window or carry a strong
  chance of precipitation.
- **Everything else** is decided on *mean* cloudiness, re-binned on
  OpenWeather's own icon thresholds. Taking the mode of already-binned values
  makes the icon flip when three hours at 26% meet two at 24%.
- **Fog** only when it holds the majority; dawn mist that burns off by nine
  should not own the morning.

The hourly array begins at the *current* hour and holds no history, so by
evening there is nothing left to aggregate for the morning. Gaps fall back, best
first, to an earlier poll from the same day (kept in NVS) and then to the day's
own single condition. Each window records which of the three it used.

### Icons
Drawn, not downloaded — [tools/make_weather_icons.py](tools/make_weather_icons.py)
into the generated [src/weather_icons_data.c](src/weather_icons_data.c).
OpenWeather serves 100×100 anti-aliased colour PNGs, which would need a decoder
this firmware does not carry and would come out as blobs in four inks; the
mock-up is line art regardless.

Each icon is described as a solid silhouette taking an `inset`, and the outline
is the silhouette minus a smaller copy of itself — one rule applied to every
closed shape, which is what keeps the set looking like one hand drew it. Marks
with no inside (sun rays, rain, the bolt) are drawn solid at final weight.
Thirteen conditions share ten glyphs; the eight moon phases are picked from
`moon_phase`.

### Polling and refreshing
Two different rates, deliberately. The forecast is fetched every **30 minutes**
(48 calls a day), but the panel is only refreshed when the screen would actually
differ — an unchanged forecast costs nothing, which matters for a panel rated
for a million refreshes. Polling carries on regardless of which screen is
showing; a forecast that changes while the clock face is up is drawn when the
knob next arrives at the weather. The 180 s floor is enforced once, in
[screen_manager.c](src/screen_manager.c), for every screen.

The board boots on the weather screen — the `screen_manager_start()` argument in
[src/main.c](src/main.c).

### Which endpoint, and why
The firmware calls **`data/2.5/forecast`** — three-hourly, five days — because
that is what an ordinary OpenWeather key can reach.

One Call (`data/4.0/onecall`) is hourly and richer, and
[src/weather_parse.c](src/weather_parse.c) still handles it, still under test.
But it needs its own *One Call by Call* subscription and returns 401 without one:

> `{"cod":401, "message": "Please note that using One Call 3.0 requires a
> separate subscription to the One Call by Call plan..."}`

The version in the path makes no difference. The docs page is titled "One Call
4.0", but its own examples call `data/3.0/onecall`, and the server accepts
either — tested, both return that same 401 for an unsubscribed key, each echoing
back the version asked for. A wrong path would give 404. `weather_api.c` logs
the server's message verbatim on any non-200, which is what tells those apart.

To move to One Call after subscribing: point `OWM_FORECAST_PATH` at
`/data/3.0/onecall` and call `weather_parse()` instead of `weather_parse25()` in
`weather_display.c`.

### What the free endpoint costs
Three things One Call would have given, and how each is covered:

| Missing | Covered by |
|---|---|
| 1-hour granularity | 3-hour samples; a window gets 2–4 instead of 6–8 |
| `daily.temp.morn/day/eve/night` | The mean of each window's own samples |
| `moon_phase` | [src/weather_moon.c](src/weather_moon.c), computed from the date |

Deriving the temperature per window is arguably the better answer: One Call's
four periods are fixed to its own definitions, whereas a mean over the window is
by construction the temperature of the period drawn beside it. It also settles
the ambiguity in what `temp.night` refers to.

`city.sunrise`/`city.sunset` are **today's only**, which is why the screen draws
today and nothing else. Aggregating another day against them collapses the
windows — the boundary clamp keeps that safe rather than nonsensical, but the
result is meaningless.

## Two-day forecast
A second weather screen, stacked rather than radial: today on top, tomorrow
below, in two equal 478 × 230 halves. Each carries the date, the day's condition
and its name across the top, then five columns — temperature, humidity,
precipitation, wind, cloud — each an icon over its reading.

![two-day forecast](./docs/pictures/forecast.png)

### Not the daily endpoint
The obvious source is OpenWeather's **16-day daily forecast**
(`/data/2.5/forecast/daily`), which hands over exactly these figures already
reduced. It is a paid endpoint, and answers 401 on a free key — the same wall
One Call 3.0 runs into, and worth knowing about before building against it:

```
/data/2.5/forecast        200    <- what this firmware uses
/data/2.5/weather         200
/data/2.5/forecast/daily  401    {"cod":401, "message": "Invalid API key..."}
/data/3.0/onecall         401    "requires a separate subscription to the One Call by Call plan"
```

Note that the daily endpoint says **"Invalid API key"** rather than naming a
plan, which makes an unsubscribed endpoint look like a bad key. The way to tell
them apart is to try a free endpoint with the same key in the same breath.

Every field the screen shows is in the free three-hourly response anyway —
`temp_min`, `temp_max`, `humidity`, `pop`, `wind.gust`, `clouds.all`,
`rain.3h`/`snow.3h`, `weather.id` — just eight times a day instead of once.
[src/weather_daily.c](src/weather_daily.c) reduces them:

| Shown | Reduced by | Why that and not the mean |
|---|---|---|
| High / low | max of `temp_max`, min of `temp_min` | They are the day's extremes by definition |
| Humidity | mean | A representative figure is what is wanted |
| Precipitation % | **the worst hour** | One hour at 90% is a day you take a coat |
| Amount | **sum** | The day's total is what falls on you |
| Wind | **the strongest gust** | A gust is already a peak; averaging peaks means nothing |
| Cloud | mean | As humidity |
| Condition | **the most severe**, then the commonest wording at that severity | An afternoon of thunderstorms is what the day was, however calm the rest; but one "heavy intensity rain" hour should not retitle a day of "moderate rain" |

The whole screen comes out of the poll the dial screen already makes, so the
second screen costs **no additional API calls** — `weather_display.c` reduces one
response two ways and tells the screen manager separately about each, since a
change to one is no reason to redraw the other.

### What "today" means as the day runs out
The three-hourly forecast begins at the current hour and carries no history, so
the top half narrows as the day goes by. At nine in the evening there is one
sample left and the half honestly describes the hours that remain; by then the
high and the low have converged on much the same number. Tomorrow is always a
full eight samples. This is the endpoint's limitation rather than a choice, but
the behaviour is deliberate: the alternative of persisting each morning's
aggregate still shows nothing useful after an evening cold boot.

### Why this screen came out grey, and what it actually was
Worth recording because the obvious explanation was wrong and cost a detour.

The screen rendered washed out — grey rather than black, and only this screen.
The first theory was stroke width: this screen is small type end to end, and a
measurement supported it, showing 57% of its horizontal runs at two pixels or
under against the clock face's 34%. A heavier cut of the small face duly went in.

It was not the cause. The real symptom, once looked at properly, was that the
image is **correct and high-contrast when the refresh finishes** and washes out
a moment later. That is not something artwork can do.

`epd_forecast_show()` was missing the `epd_sleep()` that the other three screens
all call. `epd_display()` returns with the panel still driving; the `POWER_OFF`
inside `epd_sleep()` is what stops it. Left energised after the waveform has
finished, the panel goes on pushing pigment and dissolves the image it has just
built. Fine detail goes first, which is why a screen of small type showed it and
the clock face would not have — the stroke-width measurement was real, but it
was measuring susceptibility, not cause.

The one-line fix is in `epd_forecast_show()`. The structural problem is that the
rule "always `epd_sleep()` after `epd_display()`" lives in three copies and
nothing enforces it; it is now at least written down on `epd_display()` in
[src/epd_5in83g.h](src/epd_5in83g.h). Folding the `POWER_OFF` into
`epd_display()` — which is where Waveshare's own reference puts it, inside its
`TurnOnDisplay` — would make it unforgettable, and is the right next change.

The heavier face was kept. It is not what fixed the problem, but a panel whose
optics are coarser than its addressing does render small type better with it,
and the screen is easier to read across a room. It is one line to undo: the
`#define FACE` at the top of [src/epd_forecast.c](src/epd_forecast.c).

| Screen | ink | runs ≤ 2 px | median stroke |
|---|---|---|---|
| clock face | 5.2% | 34% | 3 px |
| forecast, light cut | 9.0% | 57% | 2 px |
| forecast, heavy cut | 10.7% | 18% | 3 px |

### Wording and units
The condition name is OpenWeather's own — "BROKEN CLOUDS", "MODERATE RAIN" —
from a table in `weather_daily.c` keyed by condition id rather than from the
response's `description` string. A day is eight samples but one description, so
carrying the string would mean carrying it on every sample to be sure of having
the right one; the ids are a fixed published list, so the table cannot drift.
Fifty of OpenWeather's fifty-five phrasings fit beside the date and the icon.
For the other five — "THUNDERSTORM WITH HEAVY DRIZZLE" is the worst — **the icon
gives way, not the words.** The glyph says nothing the words do not, so dropping
it costs nothing, whereas shortening the words to make room would lose the
difference between drizzle and hail. Only a condition id outside the published
table falls all the way back to the project's own coarser vocabulary.

Gust arrives in metres per second and is shown in **km/h**, which is what reads
naturally beside °C. The degree sign is drawn as a ring rather than set: the
faces were cut from ASCII and have no `°` in them.

### Call budget
The free One Call tier allows 1000 calls a day. The cap is not the real risk — a
crash loop is, since each restart would otherwise make a call. So
[src/weather_quota.c](src/weather_quota.c) keeps three persistent brakes in NVS:

- a **daily cap** (default 200, well under the tier);
- a **minimum interval** between attempts (default 600 s), which is the brake
  that actually stops a reboot loop;
- **reserve before spend** — the counter is committed *before* the socket opens,
  so a crash inside the TLS handshake still costs budget. There is no refund on
  failure, because a request that failed locally may well have reached the server.

The day key is **UTC**, not local: OpenWeather meters against 00:00 UTC, and a
local key in a western timezone would roll over hours late and allow up to twice
the cap inside one of the server's days. Fetches are gated on the clock being
set — an unset clock reads as UTC day zero and would reset the counter on every
cold boot. Rollover only ever moves forward, so a backwards clock step cannot
clear the count either.

### Testing without the network
`WEATHER_MOCK=1` renders from a canned response and makes no call. The payload is
baked into [src/weather_mock_data.h](src/weather_mock_data.h) from a fixture, so
device and host exercise the same bytes; it costs 20 KB of flash, so leave it off
in a real build. There is exactly one `#if` in the production path, at the top of
`weather_api_fetch()` — parse, aggregate, render and schedule are identical either
way.

The pure modules build on a host with plain gcc:

```sh
sh tools/run_host_tests.sh          # parser and aggregators
python3 tools/make_weather_fixtures.py
```

A screen can also be drawn without a panel, which is how the layouts were
settled rather than by spending a 25-second refresh on each attempt:

```sh
sh tools/hostrender/render.sh response.json out.png [now_epoch]
```

It compiles the real drawing code against three stub headers in
`tools/hostrender/stub/` — nothing else differs between there and the device.
Passing an epoch poses "what does this look like at nine in the evening", which
is how the narrowing of *today* above was checked.

Fixtures cover a baseline day, the low-confidence-drizzle trap, a device booted
after dark, a polar day with no sunrise, and a 401 body. The awkward cases need
no fake clock: the aggregator takes the day boundaries as arguments, so a test
poses "booted at eight in the evening" by passing different numbers.

