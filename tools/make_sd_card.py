#!/usr/bin/env python3
"""Lay the alarm sounds out on a microSD card the way the DFPlayer wants them.

    python3 tools/make_sd_card.py /media/$USER/SDCARD
    python3 tools/make_sd_card.py --dry-run /media/$USER/SDCARD

Follows the layout in the module's datasheet
(docs/DFPlayer__DFR0299_mp3-player-module_datasheet_V1.0.pdf, section 3.4.4):
folders in the root of the card named with two digits, 01 to 99, holding files
named with three digits and nothing else, 001.mp3 to 255.mp3:

    /01/001.mp3     retro-alarm.wav
    /01/002.mp3     phony-buzzing-alarm-fx_99bpm.wav
    ...

The firmware asks for "folder 01, track N" (DF_playFolder, command 0x0F), and
the module plays /01/<NNN>.mp3. The names carry no hint of which sound is
which, so the script prints the mapping as it goes. Which number goes with which sound is decided
by SOUNDS[] in src/alarm_sounds.c, and this script reads it from there rather
than keeping a copy -- so the card it builds and the firmware that plays it
cannot come to disagree.

Every sound that is not already an MP3 is converted to one with ffmpeg. The
datasheet lists MP3 (and WMA) decoding and says nothing of WAV, and on this
radio's module a WAV "played" for its full length in silence while MP3s were
fine. The conversion is constant bitrate,
44.1 kHz stereo, with no ID3 tags and no VBR header frame: the plainest MP3
there is, which is the one these decoders are least likely to stumble on. The
originals in sfx/ are left as they are.

The card must be FAT16 or FAT32, 32 GB at most. Anything already in /01 that is
not one of ours is left alone, but a file carrying a number we use is replaced
-- 001-retro-alarm.wav from an older layout would otherwise compete with
001.mp3 -- and macOS "._" and .DS_Store droppings are removed, since the module
can mistake them for tracks (DFRobot suggests `dot_clean` for the same reason).
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLE = ROOT / "src" / "alarm_sounds.c"
SFX = ROOT / "sfx"
FOLDER = "01"    # ALARM_SOUND_FOLDER in src/alarm_sounds.h

ENTRY = re.compile(r'^\s*\{\s*(\d+)\s*,\s*"([^"]+)"\s*\},?\s*$')


def read_table():
    sounds = []
    for line in TABLE.read_text().splitlines():
        m = ENTRY.match(line)
        if m:
            sounds.append((int(m.group(1)), m.group(2)))
    if not sounds:
        sys.exit(f"no sound entries found in {TABLE}")

    tracks = [t for t, _ in sounds]
    if len(set(tracks)) != len(tracks):
        sys.exit(f"duplicate track numbers in {TABLE}")
    bad = [t for t in tracks if not 1 <= t <= 255]
    if bad:
        # The datasheet's limit for a folder, and the firmware's (a uint8_t).
        sys.exit(f"track numbers must be 1-255, got {bad}")
    return sorted(sounds)


def to_mp3(src, dst, bitrate):
    """Convert anything ffmpeg can read into a plain CBR MP3."""
    subprocess.run(
        ["ffmpeg", "-nostdin", "-loglevel", "error", "-y",
         "-i", str(src),
         "-map", "0:a:0",            # the audio, and nothing else
         "-map_metadata", "-1",      # no tags carried over...
         "-id3v2_version", "0",      # ...and no ID3v2 header written
         "-write_xing", "0",         # no VBR/Xing info frame
         "-codec:a", "libmp3lame", "-b:a", bitrate,
         "-ar", "44100", "-ac", "2",
         str(dst)],
        check=True)


def remove_droppings(dest, dry_run):
    for junk in list(dest.glob("._*")) + list(dest.glob(".DS_Store")):
        print(f"  {'would remove' if dry_run else 'removing'} {junk.name}: "
              f"macOS metadata the module could take for a track")
        if not dry_run:
            junk.unlink()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("card", type=pathlib.Path, help="where the card is mounted")
    ap.add_argument("--dry-run", action="store_true", help="say what would be done")
    ap.add_argument("--bitrate", default="128k",
                    help="MP3 bitrate for converted files (default 128k)")
    args = ap.parse_args()

    if not args.card.is_dir():
        sys.exit(f"{args.card} is not a directory -- is the card mounted?")
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg is needed to convert the sounds to MP3; install it first")

    dest = args.card / FOLDER
    if not args.dry_run:
        dest.mkdir(exist_ok=True)
    if dest.is_dir():
        remove_droppings(dest, args.dry_run)

    problems = 0

    # In track order: the module plays /01 by number, but DFRobot notes that
    # copy order sets the order for anything that plays by position instead.
    for track, name in read_table():
        src = SFX / name
        if not src.is_file():
            print(f"  MISSING  {src}")
            problems += 1
            continue

        prefix = f"{track:03d}"
        target = dest / f"{prefix}.mp3"
        convert = src.suffix.lower() != ".mp3"

        if not args.dry_run:
            # Anything else carrying this number would be played instead.
            for stale in dest.glob(f"{prefix}*"):
                if stale != target:
                    print(f"  removing {stale.name}: it claims track {track}")
                    stale.unlink()
            try:
                if convert:
                    to_mp3(src, target, args.bitrate)
                else:
                    shutil.copyfile(src, target)
            except subprocess.CalledProcessError:
                print(f"  FAILED   {name}: ffmpeg could not convert it")
                problems += 1
                continue

        verb = "convert" if convert else "copy"
        print(f"  {'would ' + verb if args.dry_run else verb + 'ed'}  "
              f"{name} -> /{FOLDER}/{target.name}")

    if (args.card / "mp3").is_dir():
        print("  note: /mp3 is from an older layout and is no longer played; "
              "delete it if nothing else uses it")

    if problems:
        sys.exit(f"{problems} sound(s) could not be put on the card")
    if not args.dry_run:
        print("done; unmount the card before pulling it")


if __name__ == "__main__":
    main()
