# Retro Clock-Radio

## Component List
| Part | Notes |
|---|---|
| [ESP32-S3 v1.1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html) | ESP32-S3-DevKitC-1 Development Board BT 2.4G Wifi Module for Arduino 8MB PSRAM 16MB FLASH N16R8 42Pin CH340C Type-C ESP32 S3. Verify your microcontroller details with [ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/). |
| [5.83inch E-Paper Display](https://www.waveshare.com/5.83inch-e-paper-g.htm) | E-ink Display, 648 × 480, Red/Yellow/Black/White, SPI Interface |
| [Rotary Encoder](https://www.adafruit.com/product/5880) | Adafruit I2C Stemma QT Rotary Encoder Breakout with Encoder - STEMMA QT / Qwiic |

### KiCad

[Espressif libraries for KiCad](https://github.com/espressif/kicad-libraries)

## Firmware Status
Bootstraps the ESP32-S3 (NVS, TCP/IP stack, default event loop), paints a
four-colour test pattern on the e-paper panel, joins a Wi-Fi network as a
station, then sets the real-time clock from NTP. The rotary encoder is not
driven yet.

- [src/main.c](src/main.c) — boot banner, bring-up, link/clock heartbeat
- [src/wifi_sta.c](src/wifi_sta.c) — station bring-up, retry/backoff, auto-reconnect
- [src/time_sync.c](src/time_sync.c) — SNTP client, timezone, background re-sync
- [src/epd_5in83g.c](src/epd_5in83g.c) — e-paper panel driver over SPI
- [src/epd_paint.c](src/epd_paint.c) — frame buffer shapes and a built-in 5x7 font
- [src/epd_pattern.c](src/epd_pattern.c) — the bring-up test pattern
- [src/epd_clockface.c](src/epd_clockface.c) — the analog clock face
- [src/epd_deco_font.c](src/epd_deco_font.c) — Art Deco numerals (generated)
- [src/clock_display.c](src/clock_display.c) — five-minute update task
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

`app_main()` puts the clock face up first, before Wi-Fi, so a wiring fault
surfaces immediately rather than behind a network timeout. Swapping the
`epd_clockface_show()` call for `epd_pattern_show()` boots the test pattern
instead. Neither an init failure nor a
BUSY timeout is fatal. A full refresh takes roughly 25 seconds, during which
`app_main()` blocks.

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

### Clock face
[src/epd_clockface.c](src/epd_clockface.c) draws the dial inspired by
[docs/pictures/clock-face.png](docs/pictures/clock-face.png): outlined numerals
outside a two-line chapter ring, an inner minute track with wedges on the fives,
cross lines running out to the edges.

The hands follow the pair in the photograph: solid black tapered wedges, broad
at the boss and narrowing to a blunt tip, with the boss drawn over both. No
second hand — see the cadence below for why there could not be one.

#### Numerals
The dial does not use the built-in 5x7 font — that font is blocky by design, and
no amount of scaling makes it Deco. [src/epd_deco_font.c](src/epd_deco_font.c)
holds a purpose-built set of figures instead, 30 x 44 px, generated by
[tools/make_deco_font.py](tools/make_deco_font.py).

Deco figures are geometric constructions, so that is how the script draws them:
each digit is a union of ellipse rings, slabs and diagonals, sampled 4 x 4 per
pixel. Two constants carry the style — a **7 px flank** against a **3 px crown**
— and that thick-to-thin contrast over true-circle geometry is what reads as
Deco rather than as a condensed sans. The 9 is the 6 turned through half a turn,
so the pair stay a pair.

Edit the geometry in the script and run it again from the project root; the
table itself is generated and should not be hand-edited. `--preview FILE.ppm`
drops a 4x proof sheet of all ten figures for eyeballing first.

Glyphs are trimmed to their ink and carry their own width, so numerals are set
proportionally — a `1` takes the width of its slab and nothing more, which is
what keeps `11` from sprawling. The set is a fixed size: at this stroke
contrast, scaling the bitmap would destroy the thing that makes it Deco. Labels
elsewhere still use the 5x7 font. Numerals are drawn yellow with a one-pixel
black outline, eight offset copies of the glyph — enough to hold an edge on
white without swallowing the hairlines.

### Update cadence
[src/clock_display.c](src/clock_display.c) redraws the hands every **five
minutes**, and that number is set by the panel, not by taste. A full refresh
takes ~25 s, the panel cannot do partial updates at all, and Waveshare's
datasheet asks for **at least 180 s between updates** — so a moving minute hand
is not on offer. Five minutes clears the floor and lands the minute hand on the
dial's own five-minute wedges, which makes the step look deliberate.

The hands show the **nearest** five-minute mark rather than the one just gone,
so the time displayed is never more than **±2 min 30 s** from the truth. That is
why the task wakes at 02:30, 07:30, 12:30 and so on rather than on the fives:
those are the instants the nearest mark changes. The hour hand carries between
the numerals in proportion to the minutes, as a real movement does.

The task also enforces the 180 s floor itself, which matters just after boot
where the first refresh and the first step change can otherwise fall seconds
apart. With the clock unset it puts up the bare dial and waits, adding the hands
once NTP lands.

### Refresh counter
The panel is rated for **1,000,000 updates or 5 years**, so
[src/refresh_count.c](src/refresh_count.c) keeps a lifetime count of them in
NVS. At 288 refreshes a day the five-year clause is what binds first; at one a
minute it would have been the million, in under two years.

The count is kept in the `nvs` partition at 0x9000, which a normal
`pio run -t upload` does not touch — new firmware inherits the running total.
Two things do lose it: an explicit full erase (`pio run -t erase`, or esptool's
`erase_flash`), and the `nvs_flash_erase()` recovery path in
[src/wifi_sta.c](src/wifi_sta.c), which fires only if NVS turns up corrupt or
version-incompatible. `refresh_count_init()` deliberately does *not* erase to
recover, since that would discard the very thing it is counting.

Counting hangs off a hook in the panel driver
(`epd_set_refresh_hook()`), fired once per refresh the panel actually performed.
That way every path that refreshes is counted — the clock face, the test
pattern, `epd_clear()` — without the driver knowing anything about storage.

### Wiring
Defaults follow Waveshare's own ESP32-S3 reference wiring. All six are
`build_flags` in [platformio.ini](platformio.ini) — rewire freely and update
them to match.

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

### Troubleshooting
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
| `NTP_SERVER` | `pool.ntp.org` | NTP server used to set the clock |
| `TIME_ZONE` | `EST5EDT,M3.2.0,M11.1.0` | Local timezone as a POSIX TZ string |
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

Omitting `WIFI_SSID` or `WIFI_PASSWORD` fails the build with an `#error` rather
than silently producing firmware that cannot connect.

To keep credentials out of version control, comment those two flags out and
export them at build time instead:

```sh
export PLATFORMIO_BUILD_FLAGS='-D WIFI_SSID=\"my-ssid\" -D WIFI_PASSWORD=\"my-pass\"'
pio run -t upload -t monitor
```
