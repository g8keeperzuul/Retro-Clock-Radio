/*
 * audio_player.h - The DFPlayer Mini, and the one task allowed to talk to it.
 *
 * The DFPlayer is a self-contained MP3/WAV player on UART1 with its own
 * microSD card; the ESP32 never touches the audio, it only says which track to
 * play. The driver in components/DFRobotDFPlayerMini is a port of DFRobot's
 * Arduino library and carries its habits with it: all of its state is global,
 * it busy-waits for acknowledgements, and its serial layer hands each
 * transmission to the task that started the UART. So every DF_* call is made
 * from this module's own task, at the lowest priority there is, and everyone
 * else posts it a request.
 *
 * A player that is unplugged, unpowered or has no card is not a fault: the
 * radio is still a radio. It is retried in the background and whenever a sound
 * is asked for.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the task. Returns at once; the player is brought up in the background. */
esp_err_t audio_player_start(void);

/*
 * Block until the boot audio test (AUDIO_BOOT_TEST in platformio.ini) is over:
 * played, skipped, or abandoned because the player did not answer at boot.
 * Returns at once when the test is not built in. Gives up after `timeout_ms`.
 */
void audio_player_wait_boot_test(uint32_t timeout_ms);

/*
 * Play file `track` of folder `folder` on the card -- /01/003.mp3 is folder 1,
 * track 3 -- over and over, for `seconds`, then stop. Replaces whatever was
 * playing. Safe to call from any task.
 */
void audio_player_loop(uint8_t folder, uint8_t track, uint32_t seconds);

/* Stop playback now. Harmless when nothing is playing. Safe from any task. */
void audio_player_stop(void);

/*
 * Whether a sound is playing, or has been asked for and is about to. True from
 * the moment audio_player_loop() returns, so a button pressed in the instant
 * after an alarm goes off is already a press that stops it.
 */
bool audio_player_is_playing(void);

#ifdef __cplusplus
}
#endif
