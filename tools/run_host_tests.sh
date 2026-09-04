#!/bin/sh
# Build and run the host tests for the pure weather modules.
#
# They deliberately have no ESP-IDF dependencies, so they build with plain gcc
# against the IDF's own cJSON -- no vendored copy to drift out of step.
#
# Run from the project root:  sh tools/run_host_tests.sh
set -e

CJSON="${IDF_PATH:-$HOME/.platformio/packages/framework-espidf}/components/json/cJSON"
[ -f "$CJSON/cJSON.c" ] || { echo "cannot find cJSON under $CJSON" >&2; exit 2; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -I src -I "$CJSON" \
    -o "$OUT/test_weather" \
    test/host/test_weather.c \
    src/weather_parse.c src/weather_parse25.c src/weather_aggregate.c \
    src/weather_types.c src/weather_moon.c \
    "$CJSON/cJSON.c" -lm

TZ=EST5EDT "$OUT/test_weather"
