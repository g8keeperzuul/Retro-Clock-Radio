/*
 * alarm_sounds.c - Which sound is which track on the DFPlayer's card. See the
 * header.
 */
#include "alarm_sounds.h"

#include <string.h>

/*
 * One line per file in sfx/. tools/make_sd_card.py parses these lines to build
 * the card, so keep each entry on a line of its own in exactly this shape.
 * Track numbers must be unique and between 1 and 255; they need not be dense.
 */
static const struct {
    uint8_t track;
    const char *name;
} SOUNDS[] = {
    { 1, "retro-alarm.wav" },
    { 2, "phony-buzzing-alarm-fx_99bpm.wav" },
    { 3, "bouncy-buzzer-lead-melody_121bpm_A#_minor.wav" },
    { 4, "mysterious-vibraphone-melody-with-pad_140bpm_A_minor.wav" },
    { 5, "synth-bells-melody-processed-wet_130bpm_D_minor.wav" },
};

#define SOUND_COUNT (sizeof(SOUNDS) / sizeof(SOUNDS[0]))

size_t alarm_sound_count(void)
{
    return SOUND_COUNT;
}

const char *alarm_sound_name(size_t i)
{
    return (i < SOUND_COUNT) ? SOUNDS[i].name : NULL;
}

uint8_t alarm_sound_track(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    for (size_t i = 0; i < SOUND_COUNT; i++) {
        if (strcmp(SOUNDS[i].name, name) == 0) {
            return SOUNDS[i].track;
        }
    }
    return 0;
}
