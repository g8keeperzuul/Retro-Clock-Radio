/*
 * alarm_sounds.h - Which sound is which track on the DFPlayer's card.
 *
 * The DFPlayer plays by number, not by name: it reads a FAT-formatted microSD
 * card of its own, and the ESP32 can only say "folder 01, track 3". So the
 * names the rest of the firmware deals in -- the ALARM_SOUND setting, the alarm
 * screen -- are translated to track numbers here, and nowhere else.
 *
 * The card has to agree with this table. tools/make_sd_card.py reads the table
 * straight out of alarm_sounds.c, converts sfx/<name> to MP3 and writes it to
 * /01/<NNN>.mp3 on the card -- the layout in the module's datasheet: two-digit
 * folders 01-99, three-digit files 001-255. See README.md "Alarm".
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The folder on the card the tracks live in: /01. */
#define ALARM_SOUND_FOLDER 1

/* Number of sounds in the table. */
size_t alarm_sound_count(void);

/* The i'th sound's file name, "retro-alarm.wav". NULL past the end. */
const char *alarm_sound_name(size_t i);

/*
 * The track number a file name is stored under in ALARM_SOUND_FOLDER, or 0 if
 * the table does not have it. The name is matched exactly, extension included.
 */
uint8_t alarm_sound_track(const char *name);

#ifdef __cplusplus
}
#endif
