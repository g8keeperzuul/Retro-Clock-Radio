/*
 * clock_display.c - Keeps the dial showing the time. See the header.
 */
#include "clock_display.h"

#include <time.h>

#include "epd_clockface.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_sync.h"

static const char *TAG = "clock-display";

#define STEP_MINUTES CLOCK_DISPLAY_STEP_MINUTES
#define STEP_SECONDS (STEP_MINUTES * 60)

/*
 * The floor the panel datasheet puts on how often it may be updated. The
 * five-minute cadence clears it comfortably; this only bites just after boot,
 * where the first refresh and the first step change can fall close together.
 */
#define MIN_REFRESH_INTERVAL_S 180

/* How often to look again while the clock is still unset. */
#define UNSET_CLOCK_RETRY_S 15

/*
 * The whole display pipeline runs on this task -- drawing, the SPI transfer,
 * and the NVS write the refresh hook does -- so it is not a small stack.
 */
#define TASK_STACK 6144
#define TASK_PRIO  4

/*
 * What the glass is showing: minutes since midnight, SLOT_NONE for the bare
 * dial, SLOT_UNDRAWN before anything has been put up at all. Distinguishing the
 * last two is what makes the bare dial get drawn once rather than never.
 */
#define SLOT_NONE    (-1)
#define SLOT_UNDRAWN (-2)

static int s_shown_slot = SLOT_UNDRAWN;
static int64_t s_last_refresh_us;

/*
 * The five-minute mark nearest the current time -- nearest, not the one just
 * gone, so the hands are never more than half a step from the truth in either
 * direction. Returns SLOT_NONE while the clock is unset.
 */
static int current_slot(void)
{
    if (!time_sync_is_valid()) {
        return SLOT_NONE;
    }

    const time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    const int second_of_day = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    const int slot = ((second_of_day + STEP_SECONDS / 2) / STEP_SECONDS) * STEP_MINUTES;

    return slot % (24 * 60);   /* the last step of the day rounds up to midnight */
}

/*
 * Seconds until the nearest five-minute mark becomes a different one, which
 * happens at the half-step points: 02:30, 07:30, 12:30 and so on.
 */
static int seconds_to_next_slot(void)
{
    const time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    const int into_step = (local.tm_min % STEP_MINUTES) * 60 + local.tm_sec;
    const int half = STEP_SECONDS / 2;

    return (into_step < half) ? (half - into_step) : (STEP_SECONDS + half - into_step);
}

/* Hold off until the panel's minimum interval has elapsed since the last refresh. */
static void respect_min_interval(void)
{
    if (s_last_refresh_us == 0) {
        return;
    }

    const int64_t since_s = (esp_timer_get_time() - s_last_refresh_us) / 1000000;
    if (since_s >= MIN_REFRESH_INTERVAL_S) {
        return;
    }

    const int wait_s = (int)(MIN_REFRESH_INTERVAL_S - since_s);
    ESP_LOGI(TAG, "holding %d s to keep %d s between refreshes", wait_s,
             MIN_REFRESH_INTERVAL_S);
    vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
}

static void clock_display_task(void *arg)
{
    (void)arg;

    while (true) {
        respect_min_interval();

        const int slot = current_slot();

        if (slot != s_shown_slot) {
            const esp_err_t err = (slot == SLOT_NONE)
                                      ? epd_clockface_show()
                                      : epd_clockface_show_time(slot / 60, slot % 60);
            if (err == ESP_OK) {
                s_shown_slot = slot;
                s_last_refresh_us = esp_timer_get_time();
            } else {
                /* Leave s_shown_slot alone so the next pass tries again. */
                ESP_LOGE(TAG, "could not update the display: %s", esp_err_to_name(err));
            }
        }

        /*
         * A refresh takes about 25 s, so the wait is computed after it, not
         * before: the next step is however far away it is by then.
         */
        const int wait_s = (slot == SLOT_NONE) ? UNSET_CLOCK_RETRY_S : seconds_to_next_slot();
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
}

esp_err_t clock_display_start(void)
{
    const BaseType_t ok = xTaskCreate(clock_display_task, "clock-display", TASK_STACK, NULL,
                                      TASK_PRIO, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "hands step every %d minutes, so the time shown is within %d s of true",
             STEP_MINUTES, STEP_SECONDS / 2);
    return ESP_OK;
}
