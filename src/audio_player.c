/*
 * audio_player.c - The DFPlayer Mini. See the header.
 */
#include "audio_player.h"

#include "alarm_sounds.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/*
 * Last, and only here: the library's header defines delay(), millis() and a
 * dozen short names such as Busy and TimeOut as macros, and nothing else in
 * the firmware should have to live with those.
 */
#include "DFRobotDFPlayerMini.h"


static const char *TAG = "audio";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

/*
 * UART1's own IO MUX pins on the S3, so the signal leaves the pad directly
 * rather than through the GPIO matrix -- and both are free on this board.
 */
#ifndef DFPLAYER_PIN_TX
#define DFPLAYER_PIN_TX 17       /* ours, to the module's RX */
#endif
#ifndef DFPLAYER_PIN_RX
#define DFPLAYER_PIN_RX 18       /* ours, from the module's TX */
#endif

/*
 * The module's BUSY output: low while a track is playing, high otherwise. With
 * it wired, the pin -- not the serial line -- is what says whether sound is
 * being made and when a pass has ended. -1 for a module with BUSY unconnected,
 * which falls back to the module's "finished" messages.
 *
 * GPIO 3 is a strapping pin on the S3, but only for choosing the JTAG source,
 * and only once an eFuse has been burned to ask it; on a stock chip it is read
 * at reset and ignored, so an input driven by the module is harmless.
 */
#ifndef DFPLAYER_PIN_BUSY
#define DFPLAYER_PIN_BUSY 3
#endif

/* 0 to 30, the module's own scale. */
#ifndef DFPLAYER_VOLUME
#define DFPLAYER_VOLUME 20
#endif

/* Set to 1 to have the library print every frame it sends and receives. */
#ifndef DFPLAYER_DEBUG
#define DFPLAYER_DEBUG 0
#endif

/* How long to leave a player that did not answer before asking again. */
#ifndef DFPLAYER_RETRY_S
#define DFPLAYER_RETRY_S 60
#endif

/*
 * Set to 1 to play the first sound on the card once, start to finish, as soon
 * as the player first answers after power-up -- a test of the whole audio path
 * (wiring, card, amplifier, speaker) without waiting for an alarm.
 */
#ifndef AUDIO_BOOT_TEST
#define AUDIO_BOOT_TEST 0
#endif

/* Longest the boot test may play before it is cut off. */
#ifndef AUDIO_BOOT_TEST_MAX_S
#define AUDIO_BOOT_TEST_MAX_S 30
#endif

/* ---- Internals ----------------------------------------------------------- */

/*
 * How often the task looks for a request and for news from the module. The
 * module reports a finished track, which is what keeps a loop going, so this
 * is also the longest gap between one pass of the sound and the next.
 */
#define POLL_MS 50

/*
 * The module reports "play finished" twice for every track -- DFRobot's own
 * example notes it and does not know why. Restarting on the first and then
 * again on its echo would chop the start off every pass, so a finish this soon
 * after a start is taken to be the echo and ignored.
 */
#define ECHO_GUARD_US (1000 * 1000)

/*
 * How long after starting a sound to check that it really is playing. Long
 * enough for the module to have opened the file and begun decoding -- BUSY
 * falls about a tenth of a second after the command -- and short enough that
 * the answer arrives while someone is listening for it.
 */
#define STATE_CHECK_US (1500 * 1000)

#define HAVE_BUSY (DFPLAYER_PIN_BUSY >= 0)

/*
 * The library busy-waits: up to two seconds while the module resets, and up to
 * half a second for each acknowledgement. Lowest priority above idle, so that
 * a wait costs nothing anyone else wanted. Its own UART tasks run one above.
 */
#define TASK_STACK 4096
#define TASK_PRIO  1

#define QUEUE_LEN 4

/* How long to let replies already in flight land before asking a question. */
#define QUERY_SETTLE_MS 100

/*
 * How long a caller will wait for room in the queue. The task can be inside a
 * two-second reset, so a burst of presses could fill it; a short wait beats a
 * dropped "stop".
 */
#define SEND_WAIT_MS 100

typedef enum {
    CMD_LOOP,
    CMD_STOP,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    uint8_t folder;
    uint8_t track;
    uint32_t seconds;
} cmd_t;

static QueueHandle_t s_queue;

/* Set once the boot test is over, for audio_player_wait_boot_test(). */
static EventGroupHandle_t s_events;
#define BOOT_TEST_DONE BIT0

/*
 * What callers are told. Written by them as well as by the task, so that it
 * changes the instant a request is made rather than a poll later. A bool is a
 * single store on this core, so it needs no lock.
 */
static volatile bool s_playing;

/* Everything below belongs to the task alone. */
static bool s_online;
static int64_t s_next_attempt_us;
static unsigned s_failed_attempts;

static bool s_looping;
static uint8_t s_folder;
static uint8_t s_track;
static int64_t s_started_us;
static int64_t s_deadline_us;
static int64_t s_state_check_us;   /* 0 once asked, or when nothing is due */
static unsigned s_passes;          /* times the module has finished the track */
static bool s_busy_seen;           /* BUSY has gone low since the last start */

/* Whether BUSY says a track is playing. Always false with no pin wired. */
static bool busy_playing(void)
{
#if HAVE_BUSY
    return gpio_get_level(DFPLAYER_PIN_BUSY) == 0;
#else
    return false;
#endif
}

static void busy_init(void)
{
#if HAVE_BUSY
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DFPLAYER_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        /* Idle-high, so an unplugged wire reads "not playing" rather than
         * floating into a phantom track. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "could not set up BUSY on GPIO %d", DFPLAYER_PIN_BUSY);
    }
#endif
}

static void drain_events(void);
static void finish(const char *why);

/*
 * The library's queries -- DF_readVolume(), DF_readState() and the rest -- send
 * a request and then take *whatever message is waiting* as the answer. If one
 * is already waiting, the answer is that instead, and the real reply is left
 * behind to be taken as the answer to the next query: every result after the
 * first is off by one. Seen at bring-up, where the volume came back -1 and the
 * file count came back as the volume. So clear the decks before each query.
 */
static void before_query(void)
{
    /* Let anything still on the wire -- an acknowledgement, a late "card
     * online" after the reset -- arrive first, so it is drained and not
     * mistaken for the reply. A frame takes about 10 ms at 9600 baud. */
    vTaskDelay(pdMS_TO_TICKS(QUERY_SETTLE_MS));
    drain_events();
}

/*
 * Reset the module and wait to hear it say its card is online. Blocks for a
 * couple of seconds either way.
 */
static bool bring_up(void)
{
    s_online = DF_begin(DFPLAYER_PIN_TX, DFPLAYER_PIN_RX, true, true, DFPLAYER_DEBUG);

    if (!s_online) {
        s_next_attempt_us = esp_timer_get_time() + (int64_t)DFPLAYER_RETRY_S * 1000000;

        /* Loud once, then quietly: a radio with no player fitted is allowed. */
        if (s_failed_attempts++ == 0) {
            ESP_LOGW(TAG, "no DFPlayer on TX %d / RX %d, or no card in it; retrying "
                          "every %d s. Check 5 V power, TX and RX not swapped, a "
                          "FAT32 card with the sounds in /01",
                     DFPLAYER_PIN_TX, DFPLAYER_PIN_RX, DFPLAYER_RETRY_S);
        } else {
            ESP_LOGD(TAG, "still no DFPlayer (attempt %u)", s_failed_attempts);
        }
        return false;
    }

    s_failed_attempts = 0;
    DF_volume(DFPLAYER_VOLUME);

    /*
     * Read both back rather than trusting them. Communication working says
     * nothing about the card: an empty one, or a volume command that went
     * astray while the module was still mounting its card, both look exactly
     * like a working player that is somehow silent. -1 means no answer.
     */
    before_query();
    const int volume = DF_readVolume();
    before_query();
    const int tracks = DF_readFileCountsInFolder(ALARM_SOUND_FOLDER);
    ESP_LOGI(TAG, "DFPlayer online on TX %d / RX %d: volume %d of 30 (asked for %d), "
                  "%d file(s) in /%02d",
             DFPLAYER_PIN_TX, DFPLAYER_PIN_RX, volume, DFPLAYER_VOLUME, tracks,
             ALARM_SOUND_FOLDER);
    if (tracks <= 0) {
        ESP_LOGW(TAG, "the module sees no files in /%02d -- build the card with "
                      "tools/make_sd_card.py", ALARM_SOUND_FOLDER);
    }
#if HAVE_BUSY
    /* Nothing is playing yet, so BUSY should be high. */
    if (busy_playing()) {
        ESP_LOGW(TAG, "BUSY on GPIO %d reads low with nothing playing -- check "
                      "that it is wired to the module's BUSY pin", DFPLAYER_PIN_BUSY);
    } else {
        ESP_LOGI(TAG, "BUSY on GPIO %d reads idle, as it should", DFPLAYER_PIN_BUSY);
    }
#endif
    if (volume >= 0 && volume != DFPLAYER_VOLUME) {
        DF_volume(DFPLAYER_VOLUME);     /* lost the first time; once more */
    }
    return true;
}

static void play_current(void)
{
    DF_playFolder(s_folder, s_track);
    s_started_us = esp_timer_get_time();
    s_busy_seen = false;
}

/* A pass ended: say how long the first one took, and go round again. */
static void next_pass(const char *how)
{
    /*
     * The first pass is worth seeing: how long it took says whether the whole
     * sound played or the module gave up on it at once.
     */
    if (s_passes++ == 0) {
        ESP_LOGI(TAG, "first pass of /%02u/%03u finished after %lld ms (%s); looping",
                 s_folder, s_track,
                 (long long)((esp_timer_get_time() - s_started_us) / 1000), how);
    }
    play_current();
}

/*
 * Ask the module what it is doing. The answer is two bytes: the device in the
 * high one (2 for the card), and 0 stopped, 1 playing, 2 paused in the low.
 */
static void check_state(void)
{
#if HAVE_BUSY
    /* The pin is the better witness: it cannot be a stale reply. */
    if (s_busy_seen || busy_playing()) {
        ESP_LOGI(TAG, "BUSY confirms /%02u/%03u is playing", s_folder, s_track);
    } else {
        ESP_LOGW(TAG, "BUSY (GPIO %d) never went low: the module is NOT playing "
                      "/%02u/%03u -- a file it cannot decode, or BUSY not wired",
                 DFPLAYER_PIN_BUSY, s_folder, s_track);
    }
    return;
#endif

    before_query();     /* also handles a finish that is waiting, if any */
    const uint16_t state = DF_readState();

    if (state == 0xFFFF) {
        ESP_LOGW(TAG, "module did not answer a status query");
    } else if ((state & 0xFF) == 1) {
        ESP_LOGI(TAG, "module confirms /%02u/%03u is playing (state 0x%04X)",
                 s_folder, s_track, state);
    } else {
        ESP_LOGW(TAG, "module says it is NOT playing /%02u/%03u (state 0x%04X): "
                      "the file is probably in a form it cannot decode -- rebuild "
                      "the card with tools/make_sd_card.py", s_folder, s_track, state);
    }
}

static void finish(const char *why)
{
    if (s_looping) {
        ESP_LOGI(TAG, "stopping /%02u/%03u: %s", s_folder, s_track, why);
    }
    s_looping = false;
    s_playing = false;
}

static void handle(const cmd_t *cmd)
{
    switch (cmd->kind) {
    case CMD_LOOP:
        if (!s_online && !bring_up()) {
            ESP_LOGE(TAG, "cannot play /%02u/%03u: the player is not answering",
                     cmd->folder, cmd->track);
            s_looping = false;
            s_playing = false;
            return;
        }

        s_folder = cmd->folder;
        s_track = cmd->track;
        s_looping = true;
        s_playing = true;
        s_passes = 0;
        play_current();
        s_state_check_us = s_started_us + STATE_CHECK_US;
        s_deadline_us = s_started_us + (int64_t)cmd->seconds * 1000000;

        ESP_LOGI(TAG, "looping /%02u/%03u for %lu s", s_folder, s_track,
                 (unsigned long)cmd->seconds);
        break;

    case CMD_STOP:
        if (s_online) {
            DF_stop();
        }
        finish("asked to");
        break;
    }
}

/* Whatever the module has said since the last pass. */
static void drain_events(void)
{
    /* Bounded: each call parses at most one frame, and a noisy line must not
     * be able to keep this task here. */
    for (int i = 0; i < 8 && DF_available(); i++) {
        const uint8_t type = DF_readType();
        const uint16_t value = DF_read();

        switch (type) {
        case DFPlayerPlayFinished:
            /* With BUSY wired the pin decides when a pass is over, and this
             * message -- sent twice, never quite on time -- is not needed. */
            if (!HAVE_BUSY && s_looping &&
                esp_timer_get_time() - s_started_us >= ECHO_GUARD_US) {
                next_pass("module reported it");
            }
            break;

        case DFPlayerError:
            /*
             * A missing track will never appear on its own, and retrying it
             * every finish would just repeat the error for five minutes.
             */
            if (value == FileMismatch || value == FileIndexOut) {
                ESP_LOGE(TAG, "/%02u/%03u is not on the card -- see "
                              "tools/make_sd_card.py", s_folder, s_track);
                finish("track missing");
            } else {
                ESP_LOGW(TAG, "module reported error %u", value);
            }
            break;

        case DFPlayerCardRemoved:
            ESP_LOGW(TAG, "card removed");
            finish("card removed");
            break;

        case DFPlayerCardInserted:
        case DFPlayerCardOnline:
            ESP_LOGI(TAG, "card online");
            break;

        case TimeOut:
            ESP_LOGD(TAG, "module did not acknowledge a command");
            break;

        default:
            break;
        }
    }
}

#if AUDIO_BOOT_TEST
/*
 * A line-for-line copy of the DFPlayer-test project's app_main(), so that the
 * only difference left between the two is the firmware around it: reset the
 * module, volume, DF_play(1) -- the first file on the card in the module's own
 * order, whatever folder it is in -- and then print whatever the module says
 * until it has reported the track finished (twice, as it always does) or the
 * time is up.
 *
 * Runs before busy_init() and bring_up(), so neither the BUSY pin nor the
 * queries those make have happened yet. bring_up() resets the module again
 * afterwards.
 */
static void boot_test(void)
{
    const bool ok = DF_begin(DFPLAYER_PIN_TX, DFPLAYER_PIN_RX, true, true, DFPLAYER_DEBUG);
    ESP_LOGI(TAG, "boot audio test: DF_begin=%d", ok);
    if (!ok) {
        ESP_LOGW(TAG, "boot audio test skipped: the player did not answer");
        return;
    }

    ESP_LOGI(TAG, "boot audio test: volume %d, DF_play(1)", DFPLAYER_VOLUME);
    DF_volume(DFPLAYER_VOLUME);
    DF_play(1);

    const int64_t deadline = esp_timer_get_time() + (int64_t)AUDIO_BOOT_TEST_MAX_S * 1000000;
    int finished = 0;
    while (finished < 2 && esp_timer_get_time() < deadline) {
        if (DF_available()) {
            const uint8_t type = DF_readType();
            const int value = DF_read();
            DF_printDetail(type, value);
            if (type == DFPlayerPlayFinished) {
                finished++;
            }
        }
        vTaskDelay(1);
    }

    if (finished < 2) {
        DF_stop();
        ESP_LOGW(TAG, "boot audio test: cut off after %d s", AUDIO_BOOT_TEST_MAX_S);
    } else {
        ESP_LOGI(TAG, "boot audio test: done");
    }
}
#endif

static void audio_task(void *arg)
{
    (void)arg;

#if AUDIO_BOOT_TEST
    boot_test();
#endif
    xEventGroupSetBits(s_events, BOOT_TEST_DONE);

    busy_init();
    bring_up();

    while (true) {
        cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
            handle(&cmd);
        }

        const int64_t now = esp_timer_get_time();

        if (s_online) {
            drain_events();

            /*
             * A pass is over when BUSY has been seen low since the start and
             * is now high again. The guard keeps a high reading in the tenth of
             * a second before the module begins from counting as the end.
             */
            if (HAVE_BUSY && s_looping) {
                if (busy_playing()) {
                    s_busy_seen = true;
                } else if (s_busy_seen && now - s_started_us >= ECHO_GUARD_US) {
                    next_pass("BUSY went high");
                }
            }

            if (s_looping && s_state_check_us != 0 && now >= s_state_check_us) {
                s_state_check_us = 0;
                check_state();
            }

            if (s_looping && now >= s_deadline_us) {
                DF_stop();
                finish("time is up");
            }
        } else if (now >= s_next_attempt_us) {
            bring_up();
        }
    }
}

esp_err_t audio_player_start(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(cmd_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(audio_task, "audio", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_player_wait_boot_test(uint32_t timeout_ms)
{
#if AUDIO_BOOT_TEST
    if (s_events == NULL) {
        return;
    }
    if ((xEventGroupWaitBits(s_events, BOOT_TEST_DONE, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(timeout_ms)) & BOOT_TEST_DONE) == 0) {
        ESP_LOGW(TAG, "boot audio test still running after %lu ms; carrying on",
                 (unsigned long)timeout_ms);
    }
#else
    (void)timeout_ms;
#endif
}

void audio_player_loop(uint8_t folder, uint8_t track, uint32_t seconds)
{
    if (s_queue == NULL) {
        return;
    }

    const cmd_t cmd = {.kind = CMD_LOOP, .folder = folder, .track = track,
                       .seconds = seconds};
    s_playing = true;
    if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(SEND_WAIT_MS)) != pdTRUE) {
        s_playing = false;
        ESP_LOGW(TAG, "request queue full; not playing");
    }
}

void audio_player_stop(void)
{
    if (s_queue == NULL) {
        return;
    }

    const cmd_t cmd = {.kind = CMD_STOP};
    s_playing = false;
    if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(SEND_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "request queue full; could not stop");
    }
}

bool audio_player_is_playing(void)
{
    return s_playing;
}
