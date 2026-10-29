/*
 * main.c - integration skeleton. Owner: Gerald.
 * Creates shared objects, seeds default state, starts all tasks.
 * NOTE: written without compiling against ESP-IDF; build it first thing and fix any typos.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "shared.h"

static const char *TAG = "main";

/* ---- Shared objects ---- */
shared_state_t     g_state;
SemaphoreHandle_t  g_state_mutex;
SemaphoreHandle_t  g_spi_mutex;
QueueHandle_t      g_log_queue;
EventGroupHandle_t g_events;
QueueHandle_t      g_scan_queue[2];

/* ---- Clock services ---- */
uint32_t clock_now(void)
{
    return (uint32_t)time(NULL);
}

uint16_t clock_minutes_of_day(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return (uint16_t)(t.tm_hour * 60 + t.tm_min);
}

void clock_set(uint32_t epoch)
{
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/* ---- Logging ---- */
void log_event(event_type_t type, uint8_t source, const char *uid, const char *note)
{
    log_event_t e;
    memset(&e, 0, sizeof(e));
    e.ts = clock_now();
    e.type = type;
    e.source = source;
    if (uid)  strncpy(e.uid, uid, UID_LEN - 1);
    if (note) strncpy(e.note, note, sizeof(e.note) - 1);
    if (xQueueSend(g_log_queue, &e, 0) != pdTRUE) {
        ESP_LOGW(TAG, "log queue full, event dropped");
    }
}

/* ---- Access schedule ---- */
static bool window_contains(const access_schedule_t *s, uint16_t m)
{
    if (s->open_min <= s->close_min) {
        return m >= s->open_min && m < s->close_min;
    }
    return m >= s->open_min || m < s->close_min;   /* wraps past midnight */
}

bool access_window_open(void)
{
    uint16_t m = clock_minutes_of_day();
    STATE_LOCK();
    bool open = window_contains(&g_state.schedule, m);
    STATE_UNLOCK();
    return open;
}

bool schedule_set(uint16_t open_min, uint16_t close_min)
{
    if (open_min >= 1440 || close_min >= 1440 || open_min == close_min) {
        return false;
    }
    STATE_LOCK();
    g_state.schedule.open_min = open_min;
    g_state.schedule.close_min = close_min;
    STATE_UNLOCK();
    log_event(EVT_SCHEDULE_CHANGED, 0, NULL, "schedule updated");
    return true;
}

/* Keeps EVT_BIT_ACCESS_OPEN in step with the clock and flags people left inside after closing. */
static void clock_task(void *arg)
{
    bool was_open = false;
    bool alerted = false;
    for (;;) {
        bool open = access_window_open();
        if (open) xEventGroupSetBits(g_events, EVT_BIT_ACCESS_OPEN);
        else      xEventGroupClearBits(g_events, EVT_BIT_ACCESS_OPEN);

        if (was_open && !open) alerted = false;     /* just closed: re-arm alert */
        if (!open && !alerted) {
            STATE_LOCK();
            int inside = g_state.building_count;
            STATE_UNLOCK();
            if (inside > 0) {
                log_event(EVT_ALERT, 0, NULL, "people inside after close");
                alerted = true;
            }
        }
        was_open = open;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- Default data (replace with SD-loaded data later) ---- */
static void seed_state(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.schedule.open_min = 7 * 60;       /* 07:00 */
    g_state.schedule.close_min = 22 * 60;     /* 22:00 */
    g_state.vacancy_timeout_s = 30;

    const char *demo_uids[] = { "A1B2C3D4", "11223344", "DEADBEEF" };
    const char *demo_names[] = { "Student 1", "Student 2", "Staff 1" };
    for (int i = 0; i < 3; i++) {
        strncpy(g_state.users[i].uid, demo_uids[i], UID_LEN - 1);
        strncpy(g_state.users[i].name, demo_names[i], NAME_LEN - 1);
        g_state.users[i].active = true;
    }
}

void app_main(void)
{
    setenv("TZ", "EAT-3", 1);     /* East Africa Time, UTC+3 */
    tzset();

    /* Wokwi boots with the clock at 1970. If so, start at 30 Oct 2026 08:00 EAT
     * (inside the access window) so testing works; the serial menu can change it. */
    if (clock_now() < 1700000000u) {
        clock_set(1793336400u);
    }

    g_state_mutex = xSemaphoreCreateMutex();
    g_spi_mutex   = xSemaphoreCreateMutex();
    g_log_queue   = xQueueCreate(LOG_QUEUE_LEN, sizeof(log_event_t));
    g_events      = xEventGroupCreate();
    g_scan_queue[DOOR_ENTRY] = xQueueCreate(SCAN_QUEUE_LEN, sizeof(scan_msg_t));
    g_scan_queue[DOOR_EXIT]  = xQueueCreate(SCAN_QUEUE_LEN, sizeof(scan_msg_t));
    configASSERT(g_state_mutex && g_spi_mutex && g_log_queue && g_events &&
                 g_scan_queue[DOOR_ENTRY] && g_scan_queue[DOOR_EXIT]);

    seed_state();
    log_event(EVT_SYSTEM, 0, NULL, "boot");

    /* Storage first so early events are persisted. Priorities: higher = more urgent. */
    xTaskCreate(storage_task,  "storage",  6144, NULL, 4, NULL);
    xTaskCreate(clock_task,    "clock",    3072, NULL, 3, NULL);
    xTaskCreate(entrance_task, "entrance", 4096, NULL, 5, NULL);
    xTaskCreate(room_task,     "room1",    4096, (void *)(intptr_t)0, 4, NULL);
    xTaskCreate(room_task,     "room2",    4096, (void *)(intptr_t)1, 4, NULL);
    xTaskCreate(cli_task,      "cli",      4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "all tasks started");
}
