/*
 * screen_manager.c - Whose turn it is to own the glass. See the header.
 */
#include "screen_manager.h"

#include "clock_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "refresh_gauge.h"
#include "weather_display.h"

static const char *TAG = "screen";

/* ---- Compile-time parameters (platformio.ini -> build_flags) ------------- */

/*
 * How long the ring must stand still before it is drawn. Long enough that a
 * flick through several screens is one refresh, short enough that a single
 * deliberate click does not feel ignored.
 */
#ifndef SCREEN_SETTLE_MS
#define SCREEN_SETTLE_MS 1200
#endif

/*
 * How often the countdown strip re-reads the clock. Only a change in the number
 * of pixels lit costs a transmission, so this is the resolution of the
 * animation, not its cost: a three-minute wait is six frames.
 */
#ifndef GAUGE_TICK_MS
#define GAUGE_TICK_MS 200
#endif

/*
 * Out of 255. Matches the knob's pixel, so the two read as one instrument.
 */
#ifndef GAUGE_BRIGHTNESS
#define GAUGE_BRIGHTNESS 40
#endif

/* The floor the panel datasheet puts on how often it may be refreshed. */
#define MIN_REFRESH_INTERVAL_S 180

/*
 * Waits shorter than this are not worth a countdown -- the strip would flash
 * once and vanish, which reads as a fault rather than as information.
 */
#define GAUGE_WORTH_SHOWING_US (2 * 1000 * 1000)

/*
 * Every screen is drawn on this task, so it must carry the heaviest of them --
 * which is the weather screen, with its icons and three faces of type.
 */
#define TASK_STACK 8192
#define TASK_PRIO  4

/*
 * The ring. Order here is the order the knob walks; adding a screen means
 * adding a line here and a draw function, and nothing else.
 *
 * The colour is what the countdown strip shows while waiting to put this screen
 * up, in unit components scaled by GAUGE_BRIGHTNESS -- the same shape as FLASH[]
 * in rotary_encoder.c. It lives here rather than in the gauge because screens
 * are this file's vocabulary: the gauge knows only red, green and blue.
 */
static const struct {
    const char *name;
    esp_err_t (*draw)(void);
    uint8_t r, g, b;
} SCREENS[SCREEN_COUNT] = {
    [SCREEN_CLOCK]   = {"clock",   clock_display_draw,   1, 1, 0},   /* yellow */
    [SCREEN_WEATHER] = {"weather", weather_display_draw, 0, 0, 1},   /* blue   */
};

static TaskHandle_t s_task;

/*
 * `s_active` is where the knob points; `s_on_glass` is what the panel is
 * actually showing. They differ for as long as a refresh is pending, which is
 * up to three minutes, so they cannot be the same variable.
 */
static screen_id_t s_active;
static screen_id_t s_on_glass;
static bool s_drawn;                  /* whether s_on_glass means anything yet */
static bool s_stale[SCREEN_COUNT];    /* content changed since it was drawn */

static int s_pending;                 /* detents the knob owes us */
static unsigned s_generation;         /* odd while the panel is mid-refresh */
static int64_t s_last_refresh_us;

/*
 * The countdown the strip is showing, anchored here rather than worked out
 * afresh each time the wait is entered.
 *
 * That distinction is the whole reason this state exists. clock_display pokes
 * this task every five minutes whether or not its screen is showing, so a
 * three-minute wait is more likely than not to be interrupted by something that
 * has nothing to do with the knob. The task settles, finds the same screen still
 * pending, and goes back to waiting -- and a countdown measured from that moment
 * would refill the strip and start again. Watching it run 6, 5, 4, 6, 5, 4 would
 * make it worse than useless. Anchored, an interruption resumes at 4.
 *
 * Zero when no countdown is running. The deadline itself needs no variable: it
 * follows from s_last_refresh_us, which cannot move while a change is pending.
 */
static int64_t s_gauge_total_us;

/*
 * Guards s_pending, s_stale, s_generation and s_last_refresh_us, each of which
 * is touched from the encoder task as well as this one. A spinlock rather than
 * a mutex: every section under it is a handful of instructions.
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static screen_id_t ring_step(screen_id_t from, int detents)
{
    int n = ((int)from + detents) % SCREEN_COUNT;
    if (n < 0) {
        n += SCREEN_COUNT;
    }
    return (screen_id_t)n;
}

screen_id_t screen_manager_active(void)
{
    return s_active;
}

/*
 * Whether a turn accepted now could be drawn as soon as the ring settles. The
 * settle window counts towards the panel's minimum interval -- it is real time
 * passing -- so a turn a second short of the floor is honestly green.
 */
static bool could_draw_at_once(void)
{
    /* Read under the lock: this runs on the encoder task, and a 64-bit load is
     * not atomic on a 32-bit core. */
    portENTER_CRITICAL(&s_lock);
    const int64_t last = s_last_refresh_us;
    portEXIT_CRITICAL(&s_lock);

    if (last == 0) {
        return true;
    }

    const int64_t since_ms = (esp_timer_get_time() - last) / 1000;
    return (since_ms + SCREEN_SETTLE_MS) >= (MIN_REFRESH_INTERVAL_S * 1000);
}

enc_verdict_t screen_manager_step(int detents)
{
    if (detents == 0) {
        return ENC_ACCEPTED;
    }
    if (s_task == NULL) {
        return ENC_IGNORED;    /* no ring to walk: nothing will come of it */
    }

    /*
     * The test and the accumulate share the lock with the task's own
     * generation bump, so a turn cannot slip in between the two and be acted
     * on once the refresh finishes.
     */
    portENTER_CRITICAL(&s_lock);
    const bool refreshing = (s_generation & 1u) != 0;
    if (!refreshing) {
        s_pending += detents;
    }
    portEXIT_CRITICAL(&s_lock);

    if (refreshing) {
        ESP_LOGI(TAG, "ignoring %+d: the panel is mid-refresh", detents);
        return ENC_IGNORED;
    }

    xTaskNotifyGive(s_task);

    return could_draw_at_once() ? ENC_ACCEPTED : ENC_DEFERRED;
}

void screen_manager_content_changed(screen_id_t screen)
{
    if (screen >= SCREEN_COUNT || s_task == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_stale[screen] = true;
    portEXIT_CRITICAL(&s_lock);

    /* Wake even for a screen that is not showing: the task decides, not us. */
    xTaskNotifyGive(s_task);
}

/* Forget the countdown, and put the strip out. */
static void gauge_stop(void)
{
    s_gauge_total_us = 0;
    refresh_gauge_clear();
}

/*
 * Pixels to light with `remaining` of `total` left to run. Six at the start,
 * one right up to the end, and never zero -- the caller reaches zero by
 * clearing the strip, not by asking for it, so no frame is ever drawn dark
 * mid-countdown and no pixel is left behind at the end.
 */
static int gauge_pixels(int64_t remaining, int64_t total)
{
    /* 64-bit: the product runs to a billion even before anyone lengthens the floor. */
    const int64_t lit = (remaining * GAUGE_LED_COUNT + total - 1) / total;

    if (lit < 1) {
        return 1;
    }
    if (lit > GAUGE_LED_COUNT) {
        return GAUGE_LED_COUNT;
    }
    return (int)lit;
}

/*
 * Sleep off whatever is left of the panel's minimum interval, running the
 * countdown strip if `show_gauge` says this wait is one the user asked for.
 * Returns false if anything woke us, in which case the caller works out what to
 * draw again rather than putting up a screen already turned past.
 */
static bool wait_out_min_interval(bool show_gauge)
{
    if (s_last_refresh_us == 0) {
        return true;
    }

    const int64_t now = esp_timer_get_time();
    const int64_t deadline = s_last_refresh_us + (int64_t)MIN_REFRESH_INTERVAL_S * 1000000;
    if (now >= deadline) {
        return true;
    }

    ESP_LOGI(TAG, "holding %d s to keep %d s between refreshes",
             (int)((deadline - now + 999999) / 1000000), MIN_REFRESH_INTERVAL_S);

    /*
     * Anchor the countdown the first time this wait is entered, and take both
     * ends from the one clock reading -- deriving the total from a figure in
     * whole seconds while measuring what is left in microseconds is enough to
     * open the strip at two pixels instead of six.
     */
    if (!show_gauge) {
        /*
         * No countdown should survive into a wait that is not one the user
         * asked for. Nothing reaches here with one still running, but enforcing
         * it beats having to prove it again every time this file is edited.
         */
        gauge_stop();
    } else if (s_gauge_total_us == 0 && (deadline - now) >= GAUGE_WORTH_SHOWING_US) {
        s_gauge_total_us = deadline - now;
    }

    int last_lit = -1;      /* not 0, or the opening frame at six is never sent */

    while (true) {
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            break;
        }

        if (s_gauge_total_us > 0) {
            const int lit = gauge_pixels(remaining, s_gauge_total_us);
            if (lit != last_lit) {
                last_lit = lit;
                refresh_gauge_show(SCREENS[s_active].r * GAUGE_BRIGHTNESS,
                                   SCREENS[s_active].g * GAUGE_BRIGHTNESS,
                                   SCREENS[s_active].b * GAUGE_BRIGHTNESS, lit);
            }
        }

        int64_t slice_ms = remaining / 1000;
        if (slice_ms > GAUGE_TICK_MS) {
            slice_ms = GAUGE_TICK_MS;
        }

        /*
         * At least one tick. A timeout that rounds down to zero neither blocks
         * nor yields, so the last few milliseconds would spin at this priority.
         */
        TickType_t slice = pdMS_TO_TICKS(slice_ms);
        if (slice == 0) {
            slice = 1;
        }

        if (ulTaskNotifyTake(pdTRUE, slice) != 0) {
            /*
             * The strip goes out but the countdown is remembered, so that a
             * wait resumed after the settle window picks up where it left off.
             */
            refresh_gauge_clear();
            xTaskNotifyGive(s_task);   /* hand the wake-up back to the top of the loop */
            return false;
        }
    }

    gauge_stop();
    return true;
}

static void refresh(screen_id_t screen)
{
    /*
     * Clear the stale mark before drawing, not after: content that arrives
     * during the twenty-five seconds is not in the frame being sent, so it has
     * to be able to mark the screen stale again while we are inside draw().
     */
    portENTER_CRITICAL(&s_lock);
    s_stale[screen] = false;
    s_generation++;             /* odd: turns from here on are dropped, not queued */
    portEXIT_CRITICAL(&s_lock);

    const esp_err_t err = SCREENS[screen].draw();

    portENTER_CRITICAL(&s_lock);
    s_generation++;             /* even */
    if (err != ESP_OK) {
        s_stale[screen] = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not draw the %s screen: %s", SCREENS[screen].name,
                 esp_err_to_name(err));
        return;
    }

    s_on_glass = screen;
    s_drawn = true;

    portENTER_CRITICAL(&s_lock);
    s_last_refresh_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

static void screen_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /*
         * Let the knob settle. Every further wake-up restarts the window, so
         * this returns once nothing has happened for SCREEN_SETTLE_MS.
         */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SCREEN_SETTLE_MS)) != 0) {
        }

        portENTER_CRITICAL(&s_lock);
        const int detents = s_pending;
        s_pending = 0;
        const bool stale = s_stale[ring_step(s_active, detents)];
        portEXIT_CRITICAL(&s_lock);

        if (detents != 0) {
            s_active = ring_step(s_active, detents);
            ESP_LOGI(TAG, "knob moved %+d: showing the %s screen", detents,
                     SCREENS[s_active].name);
        }

        /*
         * A screen change is pending exactly when the knob points somewhere the
         * glass is not. The s_drawn term is not redundant: without it the very
         * first pass is correct only because s_on_glass and the starting screen
         * happen to be the same value.
         */
        const bool changing = s_drawn && (s_active != s_on_glass);

        if (s_drawn && s_active == s_on_glass && !stale) {
            /*
             * The glass is already right. If the knob went out and came back
             * during a countdown, that countdown is now moot.
             */
            gauge_stop();
            continue;
        }

        if (!wait_out_min_interval(changing)) {
            continue;
        }

        refresh(s_active);
    }
}

esp_err_t screen_manager_start(screen_id_t initial)
{
    s_active = (initial < SCREEN_COUNT) ? initial : SCREEN_CLOCK;

    if (xTaskCreate(screen_task, "screen", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    xTaskNotifyGive(s_task);    /* s_drawn is false, so this draws immediately */

    ESP_LOGI(TAG, "%d screens, starting on %s; the knob settles for %d ms",
             SCREEN_COUNT, SCREENS[s_active].name, SCREEN_SETTLE_MS);
    return ESP_OK;
}
