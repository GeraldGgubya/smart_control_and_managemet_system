#pragma once
/*
 * shared.h - the contract between all modules. Owner: Gerald.
 * Freeze this today. Changes go through Gerald so nobody's code breaks at integration.
 *
 * Architecture: ONE ESP32, one FreeRTOS task per logical "terminal":
 *   entrance_task  (Daniel)   - RFID entry/exit, doors, schedule enforcement
 *   room_task x2   (Macalvin, Rahmat) - occupancy + lighting; arg = room index 0/1
 *   storage_task   (Ariko)    - the ONLY task that touches the SD card
 *   cli_task       (Ariko)    - serial menu + admin authentication
 *   clock_task     (Gerald)   - access-window flag + after-hours alerts
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "pins.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define NUM_ROOMS        2
#define UID_LEN          16      /* up to 15 hex chars (7-byte UID = 14) + NUL */
#define NAME_LEN         16
#define MAX_USERS        32
#define LOG_QUEUE_LEN    32
#define SCAN_QUEUE_LEN    4
#define EVT_BIT_ACCESS_OPEN   (1 << 0)   /* set while building access window is open */

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */
typedef enum { DOOR_ENTRY = 0, DOOR_EXIT = 1 } door_id_t;

typedef enum {
    EVT_ENTRY_GRANTED,
    EVT_ENTRY_DENIED,
    EVT_EXIT_GRANTED,
    EVT_EXIT_DENIED,
    EVT_ROOM_OCCUPIED,
    EVT_ROOM_VACANT,
    EVT_LIGHT_ON,
    EVT_LIGHT_OFF,
    EVT_SCHEDULE_CHANGED,
    EVT_ADMIN_LOGIN_OK,
    EVT_ADMIN_LOGIN_FAIL,
    EVT_ALERT,
    EVT_SYSTEM
} event_type_t;

/* One log record. Sent through g_log_queue, written to SD by storage_task. */
typedef struct {
    uint32_t     ts;              /* epoch seconds (see clock_now) */
    event_type_t type;
    uint8_t      source;          /* door_id_t for door events, room index for room events */
    char         uid[UID_LEN];    /* empty string if not applicable */
    char         note[24];        /* short free text */
} log_event_t;

/* A simulated tag scan, pushed by cli_task ("scan entry|exit <uid>"), read by rfid_poll(). */
typedef struct {
    char uid[UID_LEN];
} scan_msg_t;

/* Permitted building access hours, minutes since midnight. May wrap past midnight. */
typedef struct {
    uint16_t open_min;
    uint16_t close_min;
} access_schedule_t;

typedef struct {
    char uid[UID_LEN];
    char name[NAME_LEN];
    bool active;    /* slot in use */
    bool inside;    /* currently inside the building (used for anti-passback + roll call) */
} user_t;

/* All shared mutable data. Access ONLY while holding g_state_mutex. */
typedef struct {
    user_t            users[MAX_USERS];
    int               building_count;
    access_schedule_t schedule;
    bool              room_occupied[NUM_ROOMS];
    bool              light_on[NUM_ROOMS];
    uint32_t          room_last_motion[NUM_ROOMS];   /* epoch seconds */
    uint16_t          vacancy_timeout_s;             /* lights off after this long without motion */
    uint32_t          light_on_seconds[NUM_ROOMS];   /* for the energy-saved estimate */
} shared_state_t;

/* ------------------------------------------------------------------ */
/* Shared objects (defined in main.c, created in app_main)             */
/* ------------------------------------------------------------------ */
extern shared_state_t     g_state;
extern SemaphoreHandle_t  g_state_mutex;   /* protects g_state */
extern SemaphoreHandle_t  g_spi_mutex;     /* protects the SPI bus (SD + RFID) */
extern QueueHandle_t      g_log_queue;     /* log_event_t items -> storage_task */
extern EventGroupHandle_t g_events;        /* EVT_BIT_* flags */
extern QueueHandle_t      g_scan_queue[2]; /* indexed by door_id_t; simulated tag scans */

/*
 * LOCKING RULES (read these, Ariko will be asked to explain them):
 *  1. Hold g_state_mutex only for short, non-blocking sections. Never do SD, SPI,
 *     serial printing, or vTaskDelay while holding it.
 *  2. If you need both mutexes, take g_state_mutex FIRST, g_spi_mutex second
 *     (but prefer not to hold both at all). Fixed order prevents deadlock.
 *  3. Only storage_task touches the SD card. Everyone else calls log_event().
 */
#define STATE_LOCK()    xSemaphoreTake(g_state_mutex, portMAX_DELAY)
#define STATE_UNLOCK()  xSemaphoreGive(g_state_mutex)

/* ------------------------------------------------------------------ */
/* Services provided by main.c (Gerald)                                */
/* ------------------------------------------------------------------ */
/* Non-blocking: stamps the time and queues the event. Safe from any task. */
void     log_event(event_type_t type, uint8_t source, const char *uid, const char *note);

uint32_t clock_now(void);                 /* epoch seconds */
uint16_t clock_minutes_of_day(void);      /* local minutes since midnight */
void     clock_set(uint32_t epoch);       /* used by the serial menu */

/* True if the current time is inside the permitted access window. */
bool     access_window_open(void);

/* Updates the schedule under lock, refreshes the event bit, logs the change.
 * Authentication is the CALLER's job (cli_task). Returns false on invalid input. */
bool     schedule_set(uint16_t open_min, uint16_t close_min);

/* Caller MUST hold g_state_mutex. Returns index into g_state.users or -1. */
static inline int user_find_locked(const char *uid)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (g_state.users[i].active && strcmp(g_state.users[i].uid, uid) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Module entry points (each owner implements their own)               */
/* ------------------------------------------------------------------ */
void entrance_task(void *arg);   /* Daniel  : access.c  */
void room_task(void *arg);       /* Macalvin/Rahmat : room.c ; arg = (void *)(intptr_t)room_index */
void storage_task(void *arg);    /* Ariko   : storage.c */
void cli_task(void *arg);        /* Ariko   : cli.c     */

/* Reader abstraction (Daniel): returns true and fills uid_out when a tag is presented.
 * Implement with RC522 if Wokwi supports it, otherwise via serial/button simulation. */
bool rfid_poll(door_id_t door, char uid_out[UID_LEN]);

/* Reporting helpers (Rahmat: report.c). Called by cli_task for the status commands. */
void report_print_summary(void);   /* building count, per-room occupancy + lights, energy estimate */
void report_print_inside(void);    /* list of users currently inside the building */
