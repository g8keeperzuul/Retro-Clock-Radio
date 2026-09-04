#!/usr/bin/env python3
"""Generate the weather line art in src/weather_icons.c.

The mock-up is line art, and the four inks of this panel make short work of
anti-aliased colour PNGs, so the icons are drawn here rather than fetched.

Each icon is described as a solid silhouette that takes an `inset` argument.
The outline is then simply the silhouette minus the same silhouette drawn
`STROKE` pixels smaller -- one rule, applied to every closed shape, which is
what keeps the set looking like one hand drew it. Open marks that have no
inside (sun rays, rain, the lightning bolt) are drawn solid at their final
weight instead.

Run from the project root:  python3 tools/make_weather_icons.py
Add --preview FILE.pgm for a proof sheet.
"""
import math
import sys
from pathlib import Path

SIZE = 48               # every icon is square and this wide
STROKE = 2              # line weight
SUB = 3                 # supersamples per axis
OUT = Path("src/weather_icons_data.c")


# --- primitives. every shape is f(x, y, inset) -> bool ----------------------

def disc(cx, cy, r):
    return lambda x, y, i: (x - cx) ** 2 + (y - cy) ** 2 <= max(r - i, 0) ** 2


def rect(x0, y0, x1, y1):
    return lambda x, y, i: x0 + i <= x <= x1 - i and y0 + i <= y <= y1 - i


def poly(points):
    def inside(x, y, i):
        hit = False
        n = len(points)
        for k in range(n):
            ax, ay = points[k]
            bx, by = points[(k + 1) % n]
            if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
                hit = not hit
        return hit
    return inside


def union(*shapes):
    return lambda x, y, i: any(s(x, y, i) for s in shapes)


def without(a, b):
    return lambda x, y, i: a(x, y, i) and not b(x, y, -i)


def solid(shape):
    """Mark it as already-at-weight: filled, never outlined."""
    shape.solid = True
    return shape


def outline(shape):
    """The rule that makes everything here line art."""
    return lambda x, y: shape(x, y, 0) and not shape(x, y, STROKE)


# --- the pieces the icons are built from ------------------------------------

def cloud(cx, cy, scale=1.0):
    """Three lobes over a flat base, as in the mock-up."""
    r = 15 * scale
    return union(
        disc(cx - 13 * scale, cy + 2 * scale, r * 0.78),
        disc(cx, cy - 7 * scale, r),
        disc(cx + 14 * scale, cy + 1 * scale, r * 0.85),
        rect(cx - 20 * scale, cy, cx + 20 * scale, cy + 12 * scale),
    )


def rays(cx, cy, r0, r1, count=8, width=1.6, phase=0.0):
    """Straight spokes, drawn at weight rather than outlined."""
    marks = []
    for k in range(count):
        a = phase + k * 2 * math.pi / count
        dx, dy = math.cos(a), math.sin(a)
        px, py = -dy * width, dx * width
        marks.append(poly([
            (cx + dx * r0 + px, cy + dy * r0 + py),
            (cx + dx * r1 + px, cy + dy * r1 + py),
            (cx + dx * r1 - px, cy + dy * r1 - py),
            (cx + dx * r0 - px, cy + dy * r0 - py),
        ]))
    return solid(union(*marks))


def drop(cx, cy, h=11.0):
    """A teardrop: round below, pointed above. Solid -- at this size an
    outlined drop is a smudge with a hole in it."""
    return solid(union(disc(cx, cy + h * 0.30, h * 0.40),
                       poly([(cx, cy - h * 0.62), (cx + h * 0.40, cy + h * 0.40),
                             (cx - h * 0.40, cy + h * 0.40)])))


def moon_disc(cx, cy, r, phase):
    """The lit part of the moon at `phase`, 0 and 1 new, 0.5 full."""
    k = math.cos(2 * math.pi * phase)
    waxing = phase < 0.5

    def inside(x, y, i):
        rr = max(r - i, 0)
        dx, dy = x - cx, y - cy
        if dx * dx + dy * dy > rr * rr:
            return False
        xt = k * math.sqrt(max(rr * rr - dy * dy, 0.0))
        # The inset must push the terminator inwards as well as the limb.
        # Shrinking only the limb leaves both copies sharing the same straight
        # edge, and subtracting them erases the terminator entirely.
        return dx >= xt + i if waxing else dx <= -xt - i

    return inside


def star(cx, cy, r):
    """A four-pointed sparkle, the kind the mock-up puts beside the moon."""
    return solid(poly([
        (cx, cy - r), (cx + r * 0.26, cy - r * 0.26), (cx + r, cy),
        (cx + r * 0.26, cy + r * 0.26), (cx, cy + r), (cx - r * 0.26, cy + r * 0.26),
        (cx - r, cy), (cx - r * 0.26, cy - r * 0.26),
    ]))


# --- the icons ---------------------------------------------------------------

C = SIZE / 2
K = SIZE / 76.0          # the geometry below was drawn on a 76 px cell


def s_(v):
    """Scale a length drawn against the original cell to the current one."""
    return v * K


def icons():
    r = s_  # brevity: r(20) is 20 px on the original 76 px cell
    out = {}

    out["SUN"] = [disc(C, C, r(17)), rays(C, C, r(23), r(34), 12)]
    out["MOON"] = [moon_disc(C + 4, C, r(20), 0.78),
                   star(C + 22, C - 20, r(5)), star(C + 16, C - 6, r(3.5))]

    out["CLOUD"] = [cloud(C, C + 2)]
    out["CLOUDS"] = [cloud(C + 6, C + 6, 0.82), cloud(C - 8, C - 6, 0.72)]

    out["SUN_CLOUD"] = [disc(C + 12, C - 16, r(10)), rays(C + 12, C - 16, r(14), r(21), 8),
                        cloud(C - 4, C + 10, 0.82)]
    out["MOON_CLOUD"] = [moon_disc(C + 15, C - 15, r(12), 0.78),
                         cloud(C - 4, C + 10, 0.82)]

    out["RAIN"] = [cloud(C, C - 8, 0.92),
                   drop(C - 16, C + 26), drop(C, C + 30), drop(C + 16, C + 26)]

    out["STORM"] = [cloud(C, C - 10, 0.92),
                    solid(poly([(C + 4, C + 6), (C - 10, C + 26), (C - 1, C + 26),
                                (C - 6, C + 40), (C + 12, C + 18), (C + 2, C + 18)]))]

    # Six arms with a pair of branches each -- the mock-up's flake, simplified
    # to what survives at this size.
    arms = [rays(C, C, r(0), r(30), 6, 2.0, math.pi / 2)]
    for k in range(6):
        a = math.pi / 2 + k * math.pi / 3
        for reach, spread in ((19, 9), (27, 7)):
            bx, by = C + math.cos(a) * reach, C + math.sin(a) * reach
            for sign in (-1, 1):
                b = a + sign * math.pi / 3
                arms.append(solid(poly([
                    (bx - math.sin(a) * 1.6, by + math.cos(a) * 1.6),
                    (bx + math.cos(b) * spread, by + math.sin(b) * spread),
                    (bx + math.sin(a) * 1.6, by - math.cos(a) * 1.6),
                ])))
    out["SNOW"] = arms

    out["FOG"] = [solid(union(*[rect(C - 30 + (k % 2) * 6, C - 22 + k * 11,
                                     C + 30 - (k % 2) * 4, C - 18 + k * 11)
                                for k in range(5)]))]

    # Sunrise and sunset: a half disc on a horizon, rays above, and an arrow
    # saying which way it is going -- the only thing telling the two apart.
    for name, up in (("SUNRISE", True), ("SUNSET", False)):
        horizon = C + 16
        marks = [without(disc(C, horizon, r(17)), rect(-1, horizon, SIZE + 1, SIZE + 1)),
                 solid(rect(2, horizon, SIZE - 2, horizon + STROKE)),
                 rays(C, horizon, r(23), r(32), 7, 1.6, math.pi)]
        tip, tail = (C - 30, C - 12) if up else (C - 12, C - 30)
        marks.append(solid(poly([(C, tip), (C - 7, (tip + tail) / 2),
                                 (C + 7, (tip + tail) / 2)])))
        out[name] = marks

    # Eight phases, named for the octant weather_types.c reports.
    for k in range(8):
        out["MOON_%d" % k] = [disc(C, C, r(22)), moon_disc(C, C, r(22), k / 8.0)]

    return out


# --- rasterising -------------------------------------------------------------

def raster(parts):
    step = 1.0 / SUB
    need = SUB * SUB / 2
    grid = [bytearray(SIZE) for _ in range(SIZE)]

    for part in parts:
        draw = part if getattr(part, "solid", False) else outline(part)
        for py in range(SIZE):
            for px in range(SIZE):
                if grid[py][px]:
                    continue
                hits = 0
                for sy in range(SUB):
                    y = py + (sy + 0.5) * step
                    for sx in range(SUB):
                        x = px + (sx + 0.5) * step
                        if (draw(x, y, 0) if getattr(part, "solid", False)
                                else draw(x, y)):
                            hits += 1
                if hits >= need:
                    grid[py][px] = 1
    return grid


BANNER = """/*
 * weather_icons.c - Line art for the weather screen.
 *
 * GENERATED by tools/make_weather_icons.py -- do not edit by hand. Change the
 * geometry in the script and run it again from the project root.
 *
 * Every icon is %d x %d, one bit per pixel, rows packed most-significant-bit
 * first. Closed shapes are outlined at %d px by subtracting an inset copy of
 * themselves, which is what gives the set a single consistent line weight.
 */
#include "weather_icons.h"

"""


def emit(grids):
    out = [BANNER % (SIZE, SIZE, STROKE)]
    stride = (SIZE + 7) // 8

    for name, grid in grids.items():
        out.append("static const uint8_t ICON_%s[] = {\n" % name)
        blob = bytearray()
        for row in grid:
            packed = bytearray(stride)
            for x in range(SIZE):
                if row[x]:
                    packed[x >> 3] |= 0x80 >> (x & 7)
            blob += packed
        for i in range(0, len(blob), 16):
            out.append("    " + ", ".join("0x%02X" % b for b in blob[i:i + 16]) + ",\n")
        out.append("};\n\n")

    out.append("const weather_icon_t WEATHER_ICONS[WX_ICON_COUNT] = {\n")
    for name in grids:
        out.append("    [WX_ICON_%s] = {ICON_%s},\n" % (name, name))
    out.append("};\n")
    return "".join(out)


def main():
    grids = {}
    for name, parts in icons().items():
        sys.stderr.write("  %s\n" % name)
        grids[name] = raster(parts)

    OUT.write_text(emit(grids))
    print("wrote %s: %d icons, %d bytes" %
          (OUT, len(grids), len(grids) * SIZE * ((SIZE + 7) // 8)))

    if "--preview" in sys.argv:
        path = Path(sys.argv[sys.argv.index("--preview") + 1])
        cols = 6
        rows = (len(grids) + cols - 1) // cols
        w, h = cols * SIZE, rows * SIZE
        canvas = [bytearray(b"\xff" * w) for _ in range(h)]
        for n, grid in enumerate(grids.values()):
            ox, oy = (n % cols) * SIZE, (n // cols) * SIZE
            for y in range(SIZE):
                for x in range(SIZE):
                    if grid[y][x]:
                        canvas[oy + y][ox + x] = 0
        path.write_bytes(b"P5\n%d %d\n255\n" % (w, h) + b"".join(canvas))
        print("preview -> %s" % path)


if __name__ == "__main__":
    main()
