#!/usr/bin/env python3
"""Build One Call 3.0 fixtures for the weather tests.

Synthesised rather than captured, so the interesting cases can be posed exactly:
a storm confined to one afternoon hour, drizzle the forecaster barely believes
in, a device that booted after dark. Each file is shaped like a real response --
same keys, same nesting, same units -- so the parser is exercised properly.

Run from the project root:  python3 tools/make_weather_fixtures.py
"""
import json
from pathlib import Path

OUT = Path("test/fixtures")

# 2026-07-15, a Wednesday. EDT is UTC-4, so local midnight is 04:00Z.
DAY_START_UTC = 1784088000          # 2026-07-15 00:00 EDT
HOUR = 3600
SUNRISE = DAY_START_UTC + 5 * HOUR + 45 * 60   # 05:45 local
SUNSET = DAY_START_UTC + 20 * HOUR + 30 * 60   # 20:30 local


def hour(dt, cond_id, temp, pop=0.0, clouds=0):
    return {
        "dt": dt, "temp": temp, "feels_like": temp - 1.0, "pressure": 1015,
        "humidity": 60, "dew_point": temp - 8.0, "uvi": 4.2, "clouds": clouds,
        "visibility": 10000, "wind_speed": 3.1, "wind_deg": 200, "wind_gust": 5.0,
        "pop": pop,
        "weather": [{"id": cond_id, "main": "X", "description": "x", "icon": "01d"}],
    }


def day(dt, cond_id, morn, dayt, eve, night, sunrise=SUNRISE, sunset=SUNSET,
        moon=0.28, omit_sun=False):
    entry = {
        "dt": dt,
        "moonrise": dt - 3 * HOUR, "moonset": dt + 9 * HOUR, "moon_phase": moon,
        "summary": "Expect a day of partly cloudy with clear spells",
        "temp": {"day": dayt, "min": morn - 4, "max": dayt + 2,
                 "night": night, "eve": eve, "morn": morn},
        "feels_like": {"day": dayt, "night": night, "eve": eve, "morn": morn},
        "pressure": 1014, "humidity": 55, "dew_point": 12.0, "wind_speed": 4.0,
        "wind_deg": 210, "wind_gust": 7.5,
        "weather": [{"id": cond_id, "main": "X", "description": "x", "icon": "01d"}],
        "clouds": 40, "pop": 0.3, "uvi": 7.1,
    }
    if not omit_sun:
        entry["sunrise"] = sunrise
        entry["sunset"] = sunset
    return entry


def envelope(hourly, daily):
    return {
        "lat": 43.6532, "lon": -79.3832,
        "timezone": "America/Toronto", "timezone_offset": -14400,
        "hourly": hourly, "daily": daily,
    }


def full_day_hours(start_offset=0, storm_hours=(), drizzle_hours=(),
                   clouds_by_hour=None):
    """48 hours from local midnight + start_offset, EDT."""
    out = []
    for i in range(start_offset, start_offset + 48):
        dt = DAY_START_UTC + i * HOUR
        clouds = (clouds_by_hour or {}).get(i, 0)
        if i in storm_hours:
            out.append(hour(dt, 211, 24.0, 0.85, 95))
        elif i in drizzle_hours:
            # Light rain the model barely believes in: must NOT read as RAIN.
            out.append(hour(dt, 500, 20.0, 0.08, 70))
        else:
            cond = 800 if clouds <= 10 else 801 if clouds <= 25 else 802 if clouds <= 50 else 804
            out.append(hour(dt, cond, 18.0 + i % 10, 0.0, clouds))
    return out


def write(name, doc):
    path = OUT / name
    path.write_text(json.dumps(doc, indent=1))
    print(f"  {path}  ({path.stat().st_size} bytes)")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    days = [day(DAY_START_UTC + 12 * HOUR, 802, 16.0, 26.0, 22.0, 14.0),
            day(DAY_START_UTC + 36 * HOUR, 800, 17.0, 27.0, 23.0, 15.0,
                sunrise=SUNRISE + 24 * HOUR, sunset=SUNSET + 24 * HOUR, moon=0.31)]

    # Baseline: clear night, clear morning, one storm hour at 15:00, cloudy evening.
    clouds = {h: 0 for h in range(0, 12)}
    # From sunset on. Hour 20 is included because its midpoint falls after
    # 20:30, so it belongs to the evening window and would otherwise drag the
    # average below the overcast threshold.
    clouds.update({h: 70 for h in range(20, 24)})
    write("onecall_clear.json",
          envelope(full_day_hours(storm_hours=(15,), clouds_by_hour=clouds), days))

    # Drizzle trap: four low-confidence "light rain" hours across the afternoon.
    write("onecall_drizzle.json",
          envelope(full_day_hours(drizzle_hours=(13, 14, 15, 16)), days))

    # Booted at 20:00: the hourly array starts there, so morning and afternoon
    # have no samples at all and must be filled some other way.
    write("onecall_evening.json",
          envelope(full_day_hours(start_offset=20), days))

    # Polar: no sunrise or sunset keys at all.
    polar = [day(DAY_START_UTC + 12 * HOUR, 804, 2.0, 5.0, 3.0, 1.0, omit_sun=True)]
    write("onecall_polar.json", envelope(full_day_hours(), polar))

    # What the server sends when the key is wrong.
    write("onecall_err_401.json",
          {"cod": 401, "message": "Invalid API key."})


if __name__ == "__main__":
    print("writing fixtures:")
    main()


def emit_mock_header(fixture="forecast25_live.json",
                     out=Path("src/weather_mock_data.h")):
    """Bake a fixture into a C array for -D WEATHER_MOCK=1 builds.

    Must match whatever OWM_FORECAST_PATH points at -- the mock feeds the same
    parser the live path does, which is the whole point of it.

    A byte array rather than a string literal: C only guarantees 4095
    characters in a string, and this payload is five times that.
    """
    data = (OUT / fixture).read_bytes()
    lines = ["/*\n * weather_mock_data.h - A canned One Call response.\n *\n"
             " * GENERATED by tools/make_weather_fixtures.py from\n"
             " * test/fixtures/%s -- do not edit by hand.\n *\n"
             " * Only compiled in when WEATHER_MOCK is set, so it costs nothing\n"
             " * in a normal build.\n */\n#pragma once\n\n"
             "static const char WEATHER_MOCK_JSON[] = {\n" % fixture]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",\n")
    lines.append("    0x00\n};\n")
    out.write_text("".join(lines))
    print(f"  {out}  ({len(data)} bytes of payload)")


emit_mock_header("forecast25_live.json")
