/*
 * epd_alarm.h - The alarm screen.
 *
 * The time the alarm is set for, on a 24-hour clock and as large as the glass
 * allows; whether it is on; and the name of the sound it will play. "--:--"
 * when no alarm is set.
 *
 * Set in the plain 5x7 font, scaled, like the status screen. The Deco dial face
 * has no colon and is cut at a size meant for numerals round a clock, not for
 * a time read across the room.
 *
 * Pressing the knob while this screen is showing switches the alarm on and
 * off; see alarm.h.
 */
#pragma once

#include "epd_5in83g.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draw and push to the panel. Blocks for a refresh, about 25 seconds. */
esp_err_t epd_alarm_show(void);

#ifdef __cplusplus
}
#endif
