/*
 * clock_display.c - Keeps the dial showing the time. See the header.
 */
#include "clock_display.h"

#include <time.h>

#include "epd_clockface.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_manager.h"
#include "time_sync.h"

static const char *TAG = "clock-display";

#define STEP_MINUTES CLOCK_DISPLAY_STEP_MINUTES
#define STEP_SECONDS (STEP_MINUTES * 60)

/* How often to look again while the clock is still unset. */
#define UNSET_CLOCK_RETRY_S 15

/*
 * This task only watches the clock and says when the hands would move; the
 * drawing happens on the screen manager's task. It needs very little.
 */
#define TASK_STACK 3072
#define TASK_PRIO  3

/* The bare dial, drawn while the real-time clock is still unset. */
#define SLOT_NONE (-1)

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

esp_err_t clock_display_draw(void)
{
    /*
     * The slot is worked out here rather than handed over by the task, so that
     * arriving at this screen after an hour on another one shows the time now
     * and not the time the hands last moved.
     */
    const int slot = current_slot();

    return (slot == SLOT_NONE) ? epd_clockface_show()
                               : epd_clockface_show_time(slot / 60, slot % 60);
}

static void clock_display_task(void *arg)
{
    (void)arg;

    /* What was last announced, which is not necessarily what is on the glass. */
    int announced = SLOT_NONE - 1;

    while (true) {
        const int slot = current_slot();

        if (slot != announced) {
            announced = slot;
            screen_manager_content_changed(SCREEN_CLOCK);
        }

        vTaskDelay(pdMS_TO_TICKS(
            ((slot == SLOT_NONE) ? UNSET_CLOCK_RETRY_S : seconds_to_next_slot()) * 1000));
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
