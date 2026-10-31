/*
 * room.c - Lecture room occupancy monitoring + automatic lighting.
 * Owner: Wamala Macalvin
 *
 * ONE function (room_task) is started TWICE by main.c:
 *   arg = 0 -> Lecture Room 1,  arg = 1 -> Lecture Room 2.
 * Each copy runs as its own FreeRTOS task with its own local variables,
 * so the two rooms work independently.
 *
 * What each copy does, forever:
 *   1. Read the room's PIR sensor and debounce it.
 *   2. Motion  -> room occupied, light ON, remember the time of the motion.
 *   3. No motion for vacancy_timeout_s seconds -> room vacant, light OFF.
 *   4. Keep g_state up to date (only while holding the mutex).
 *   5. Log an event ONLY when something changes.
 */

#include "shared.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "room";

/* Pins per room, indexed by room number (0 = Room 1, 1 = Room 2).
 * One function + two arrays means no copy-pasted code for Room 2. */
static const int PIR_PIN[NUM_ROOMS]   = { PIN_ROOM1_PIR,   PIN_ROOM2_PIR };
static const int LIGHT_PIN[NUM_ROOMS] = { PIN_ROOM1_LIGHT, PIN_ROOM2_LIGHT };

#define POLL_MS            100  /* how often we read the sensor (ms)                    */
#define DEBOUNCE_READS     3    /* a reading must repeat this many times to be believed */
#define DEFAULT_TIMEOUT_S  30   /* used only if vacancy_timeout_s was left at 0         */

void room_task(void *arg)
{
    /* The room number travels inside the void* argument. Turn it back into an int. */
    int room = (int)(intptr_t)arg;

    if (room < 0 || room >= NUM_ROOMS) {
        ESP_LOGE(TAG, "bad room index %d, stopping task", room);
        vTaskDelete(NULL);
        return;
    }

    /* ---------- 1. Set up the pins ---------- */
    gpio_reset_pin(PIR_PIN[room]);
    gpio_set_direction(PIR_PIN[room], GPIO_MODE_INPUT);

    gpio_reset_pin(LIGHT_PIN[room]);
    gpio_set_direction(LIGHT_PIN[room], GPIO_MODE_OUTPUT);
    gpio_set_level(LIGHT_PIN[room], 0);              /* light starts OFF */

    /* Start from a known state in the shared data. */
    STATE_LOCK();
    g_state.room_occupied[room] = false;
    g_state.light_on[room]      = false;
    STATE_UNLOCK();

    /* Local (private) variables: only this task copy can see them. */
    int      last_raw   = 0;   /* previous raw sensor reading               */
    int      same_count = 0;   /* how many times in a row we saw last_raw   */
    int      motion     = 0;   /* debounced motion state (0 or 1)           */
    uint32_t ms_light_on = 0;  /* milliseconds counter for light_on_seconds */

    ESP_LOGI(TAG, "room %d task started", room + 1);

    for (;;) {
        /* ---------- 2. Read the sensor and debounce ---------- */
        int raw = gpio_get_level(PIR_PIN[room]);

        if (raw == last_raw) {
            if (same_count < DEBOUNCE_READS) {
                same_count++;
            }
        } else {                      /* reading changed: start counting again */
            last_raw   = raw;
            same_count = 1;
        }

        if (same_count >= DEBOUNCE_READS) {
            motion = raw;             /* only now do we trust the reading */
        }

        /* ---------- 3. Decide what the room's state should be ---------- */
        uint32_t now = clock_now();   /* ask for the time BEFORE taking the lock */
        bool was_occupied;
        bool occupied;

        STATE_LOCK();                 /* keep this section short, no delays, no printing */

        uint16_t timeout = g_state.vacancy_timeout_s;
        if (timeout == 0) {
            timeout = DEFAULT_TIMEOUT_S;
        }

        was_occupied = g_state.room_occupied[room];
        occupied     = was_occupied;

        if (motion) {
            g_state.room_last_motion[room] = now;     /* remember when we last saw movement */
            occupied = true;
        } else if (was_occupied) {
            uint32_t last = g_state.room_last_motion[room];
            uint32_t idle = (now >= last) ? (now - last) : 0;  /* guard: clock set backwards */
            if (idle >= timeout) {
                occupied = false;                     /* nobody moved for long enough */
            }
        }

        g_state.room_occupied[room] = occupied;
        g_state.light_on[room]      = occupied;       /* light simply follows occupancy */

        /* Energy bookkeeping: count whole seconds the light has been on. */
        if (occupied) {
            ms_light_on += POLL_MS;
            if (ms_light_on >= 1000) {
                ms_light_on -= 1000;
                g_state.light_on_seconds[room]++;
            }
        }

        STATE_UNLOCK();

        /* ---------- 4. Act, outside the lock ---------- */
        gpio_set_level(LIGHT_PIN[room], occupied ? 1 : 0);

        /* ---------- 5. Log only when something CHANGED ---------- */
        if (occupied != was_occupied) {
            log_event(occupied ? EVT_ROOM_OCCUPIED : EVT_ROOM_VACANT,
                      (uint8_t)room, "",
                      occupied ? "motion detected" : "no motion timeout");
            log_event(occupied ? EVT_LIGHT_ON : EVT_LIGHT_OFF,
                      (uint8_t)room, "",
                      occupied ? "light on" : "light off");
            ESP_LOGI(TAG, "room %d is now %s, light %s",
                     room + 1,
                     occupied ? "OCCUPIED" : "VACANT",
                     occupied ? "ON" : "OFF");
        }

        /* A task must always sleep, otherwise it hogs the CPU and the watchdog resets the chip. */
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}