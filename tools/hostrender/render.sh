#!/bin/sh
# Render a screen to a PNG from a captured API response, without a panel.
#
#   sh tools/hostrender/render.sh response.json out.png [now_epoch]
#
# The stub/ headers stand in for the three ESP-IDF headers the drawing code
# includes; nothing else about the code changes between here and the device.
set -e

CJSON="${IDF_PATH:-$HOME/.platformio/packages/framework-espidf}/components/json/cJSON"
[ -f "$CJSON/cJSON.c" ] || { echo "cannot find cJSON under $CJSON" >&2; exit 2; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -I src -I tools/hostrender/stub -I "$CJSON" \
    -o "$OUT/render" \
    tools/hostrender/render_forecast.c \
    src/epd_forecast.c src/epd_paint.c src/epd_font.c src/epd_font_data.c \
    src/weather_icons.c src/weather_icons_data.c \
    src/weather_daily.c src/weather_parse25.c src/weather_aggregate.c \
    src/weather_types.c src/weather_moon.c \
    "$CJSON/cJSON.c" -lm

TZ=${TZ:-EST5EDT} "$OUT/render" "$1" "$OUT/out.ppm" ${3:+"$3"}
convert "$OUT/out.ppm" "$2"
echo "wrote $2"
