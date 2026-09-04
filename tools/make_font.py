#!/usr/bin/env python3
"""Rasterise fonts/Aesthetic Rigelsia Free.ttf into src/epd_font_data.c.

The panel has no font engine, so the glyphs are baked to bitmaps here. Each face
is one point size with its own charset -- carrying an alphabet the dial never
uses would just be flash spent on nothing.

Rasterising is done by ImageMagick, which is the only thing on hand that can
read a TrueType outline. Per glyph:

  * render it on a large canvas at a known pen position, threshold to 1 bit,
    and find the ink box -- that gives width, height and, crucially, the offset
    from the BASELINE, without which ascenders and descenders cannot be made to
    line up;
  * measure the advance as width("cc") - width("c"), since ImageMagick will
    report a label's extent but not a glyph's advance.

Run from the project root:  python3 tools/make_font.py
Add --preview DIR to drop a proof sheet per face.
"""
import subprocess
import sys
from pathlib import Path

FONT = Path("fonts/Aesthetic Rigelsia Free.ttf")
OUT = Path("src/epd_font_data.c")

# Every face indexes the same contiguous range, so lookup is one subtraction.
# Characters outside a face's charset are emitted as empty glyphs: they cost a
# table slot and no bitmap, which is cheaper than a sparse map is complicated.
FIRST, LAST = 0x20, 0x7E

DIGITS = "0123456789"
UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
LOWER = "abcdefghijklmnopqrstuvwxyz"
PUNCT = " ,.:%-/'"

FACES = [
    # name,        pointsize, charset,                        tracking
    ("FONT_DIAL",   60, DIGITS,                                2),
    ("FONT_TITLE",  40, UPPER + LOWER + DIGITS + PUNCT,        2),
    ("FONT_LABEL",  26, UPPER + DIGITS + PUNCT,                1),
]


def magick(args, binary=False):
    r = subprocess.run(["convert"] + args, capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode().strip()


def annotate_safe(ch):
    """ImageMagick expands percent escapes in -annotate text."""
    return "%%" if ch == "%" else ch


def label_width(ch, pt):
    return int(magick(["-font", str(FONT), "-pointsize", str(pt),
                       "label:" + ch, "-format", "%w", "info:"]))


def advance_of(ch, pt):
    """Advance is the difference a second copy makes; a label alone is trimmed."""
    if ch == " ":
        # A bare space label has no ink and ImageMagick refuses it.
        return label_width("a a", pt) - label_width("aa", pt)
    return label_width(ch + ch, pt) - label_width(ch, pt)


def read_pgm(data):
    """Minimal P5 reader: convert emits no comments, but be tolerant anyway."""
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h]


def render(ch, pt):
    """Ink box of `ch` relative to the pen, plus its rows of bits.

    Returns (w, h, bx, by, rows) where by is measured from the baseline and is
    negative above it. Returns None when the glyph leaves no ink.
    """
    canvas_w, canvas_h = pt * 6, pt * 5
    pen_x, pen_y = pt, pt * 3            # pen_y is the baseline

    pgm = magick(["-size", f"{canvas_w}x{canvas_h}", "xc:white",
                  "-font", str(FONT), "-pointsize", str(pt), "-fill", "black",
                  "-annotate", f"+{pen_x}+{pen_y}", annotate_safe(ch),
                  "-threshold", "50%", "-depth", "8", "pgm:-"], binary=True)
    w, h, pix = read_pgm(pgm)

    ink = [(x, y) for y in range(h) for x in range(w) if pix[y * w + x] < 128]
    if not ink:
        return None

    x0 = min(p[0] for p in ink)
    x1 = max(p[0] for p in ink)
    y0 = min(p[1] for p in ink)
    y1 = max(p[1] for p in ink)
    gw, gh = x1 - x0 + 1, y1 - y0 + 1

    rows = []
    for y in range(y0, y1 + 1):
        row = bytearray((gw + 7) // 8)
        for x in range(x0, x1 + 1):
            if pix[y * w + x] < 128:
                col = x - x0
                row[col >> 3] |= 0x80 >> (col & 7)
        rows.append(bytes(row))

    return gw, gh, x0 - pen_x, y0 - pen_y, rows


def build_face(name, pt, charset, tracking):
    sys.stderr.write(f"{name}: pt{pt}, {len(charset)} glyphs\n")
    glyphs, blob = [], bytearray()
    ascent = descent = 0

    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        if ch not in charset or ch == " ":
            glyphs.append(None)
            continue

        r = render(ch, pt)
        if r is None:
            glyphs.append(None)
            continue

        gw, gh, bx, by, rows = r
        ascent = max(ascent, -by)
        descent = max(descent, by + gh)

        glyphs.append({
            "ch": ch, "w": gw, "h": gh, "bx": bx, "by": by,
            "advance": advance_of(ch, pt) + tracking,
            "offset": len(blob),
        })
        for row in rows:
            blob += row

    return {
        "name": name,
        "space": advance_of(" ", pt) + tracking,
        "ascent": ascent,
        "descent": descent,
        "glyphs": glyphs,
        "blob": blob,
    }


BANNER = """/*
 * epd_font_data.c - Bitmap faces cut from Aesthetic Rigelsia Free.
 *
 * GENERATED by tools/make_font.py -- do not edit by hand. Change the face list
 * in the script and run it again from the project root.
 *
 * Rows are packed most-significant-bit first, (w + 7) / 8 bytes per row, and
 * every face shares one bitmap blob that its glyphs index into.
 */
#include "epd_font.h"

"""


def emit(faces):
    out = [BANNER]

    for f in faces:
        blob = f["blob"]
        out.append(f"static const uint8_t {f['name']}_BITS[] = {{\n")
        for i in range(0, len(blob), 16):
            chunk = ", ".join("0x%02X" % b for b in blob[i:i + 16])
            out.append(f"    {chunk},\n")
        out.append("};\n\n")

        out.append(f"static const font_glyph_t {f['name']}_GLYPHS[] = {{\n")
        for code, g in zip(range(FIRST, LAST + 1), f["glyphs"]):
            if g is None:
                out.append("    {0, 0, 0, 0, 0, 0},\n")
            else:
                out.append("    {%d, %d, %d, %d, %d, %d},  /* %s */\n" % (
                    g["w"], g["h"], g["bx"], g["by"], g["advance"], g["offset"],
                    g["ch"] if g["ch"] != "*" else "asterisk"))
        out.append("};\n\n")

        out.append(f"const font_face_t {f['name']} = {{\n")
        out.append(f"    .line_height = {f['ascent'] + f['descent']},\n")
        out.append(f"    .ascent = {f['ascent']},\n")
        out.append(f"    .space_advance = {f['space']},\n")
        out.append(f"    .first = {FIRST}, .count = {LAST - FIRST + 1},\n")
        out.append(f"    .glyphs = {f['name']}_GLYPHS,\n")
        out.append(f"    .bits = {f['name']}_BITS,\n")
        out.append("};\n\n")

    return "".join(out)


def preview(face, directory):
    """A proof sheet, so a broken face is caught before it reaches a panel."""
    text = "Wednesday, July 12  SUNNY 72F  0123456789"
    rows = []
    width = 0
    for ch in text:
        g = face["glyphs"][ord(ch) - FIRST] if FIRST <= ord(ch) <= LAST else None
        width += face["space"] if ch == " " else (g["advance"] if g else 0)

    h = face["ascent"] + face["descent"]
    canvas = [bytearray(b"\x00" * max(width, 1)) for _ in range(h)]

    pen = 0
    for ch in text:
        g = face["glyphs"][ord(ch) - FIRST] if FIRST <= ord(ch) <= LAST else None
        if ch == " " or g is None:
            pen += face["space"] if ch == " " else 0
            continue
        stride = (g["w"] + 7) // 8
        for row in range(g["h"]):
            y = face["ascent"] + g["by"] + row
            if not 0 <= y < h:
                continue
            for col in range(g["w"]):
                x = pen + g["bx"] + col
                byte = face["blob"][g["offset"] + row * stride + (col >> 3)]
                if 0 <= x < width and byte & (0x80 >> (col & 7)):
                    canvas[y][x] = 1
        pen += g["advance"]

    path = Path(directory) / f"{face['name'].lower()}.pgm"
    body = b"".join(bytes(255 - 255 * v for v in row) for row in canvas)
    path.write_bytes(b"P5\n%d %d\n255\n" % (width, h) + body)
    sys.stderr.write(f"  preview -> {path}\n")


def main():
    if not FONT.exists():
        sys.exit(f"missing {FONT}")

    faces = [build_face(*spec) for spec in FACES]
    OUT.write_text(emit(faces))

    total = sum(len(f["blob"]) for f in faces)
    print(f"wrote {OUT}: {len(faces)} faces, {total} bytes of bitmap")

    if "--preview" in sys.argv:
        directory = sys.argv[sys.argv.index("--preview") + 1]
        Path(directory).mkdir(parents=True, exist_ok=True)
        for f in faces:
            preview(f, directory)


if __name__ == "__main__":
    main()
