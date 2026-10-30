/*
 * report.c - status reports and energy-saved estimate.
 * Role: Namayanja Rahmat (reports + innovation feature). Called from cli_task.
 *
 * Design notes:
 *  - Every function COPIES what it needs out of g_state while holding the state mutex,
 *    releases the mutex, and only THEN prints. Printing is slow, and holding the lock
 *    while printing would block the entrance and room tasks (locking rule 1 in shared.h).
 *  - Energy estimate (innovation): compare how long each room's light was actually on
 *    with a baseline where the light would have stayed on the whole time since boot.
 *    The difference, multiplied by an assumed lamp power, is the energy saved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "shared.h"

/* Assumption: total lamp power in one lecture room. Change to match the report. */
#define LAMP_WATTS_PER_ROOM  60

/* ---- helpers ---------------------------------------------------------- */

/* minutes since midnight -> "HH:MM" */
static void fmt_hhmm(uint16_t mins, char out[12])
{
    snprintf(out, 12, "%02u:%02u", (unsigned)((mins / 60) % 24), (unsigned)(mins % 60));
}

/* Energy saved in milliwatt-hours: watts * (seconds saved) / 3600 * 1000.
 * Integer maths only, so no floating-point printf is needed. */
static uint64_t saved_mwh(uint32_t uptime_s, uint32_t light_on_s)
{
    if (light_on_s >= uptime_s) {
        return 0;                                   /* never negative */
    }
    uint64_t saved_s = (uint64_t)(uptime_s - light_on_s);
    return (uint64_t)LAMP_WATTS_PER_ROOM * saved_s * 1000ULL / 3600ULL;
}

static void print_wh(uint64_t mwh)
{
    printf("%llu.%03llu Wh", (unsigned long long)(mwh / 1000ULL),
                             (unsigned long long)(mwh % 1000ULL));
}

/* ---- public API (declared in shared.h) -------------------------------- */

void report_print_summary(void)
{
    /* 1. Take a snapshot of the fields we need, under the lock. */
    int      building_count;
    uint16_t open_min, close_min, vacancy_s;
    bool     occupied[NUM_ROOMS], light_on[NUM_ROOMS];
    uint32_t on_s[NUM_ROOMS];

    STATE_LOCK();
    building_count = g_state.building_count;
    open_min       = g_state.schedule.open_min;
    close_min      = g_state.schedule.close_min;
    vacancy_s      = g_state.vacancy_timeout_s;
    for (int r = 0; r < NUM_ROOMS; r++) {
        occupied[r] = g_state.room_occupied[r];
        light_on[r] = g_state.light_on[r];
        on_s[r]     = g_state.light_on_seconds[r];
    }
    STATE_UNLOCK();

    /* 2. Things that take the lock themselves must be called AFTER we release it. */
    bool     window_open = access_window_open();
    uint16_t now_min     = clock_minutes_of_day();
    uint32_t uptime_s    = (uint32_t)(esp_timer_get_time() / 1000000LL);

    char t_now[12], t_open[12], t_close[12];
    fmt_hhmm(now_min, t_now);
    fmt_hhmm(open_min, t_open);
    fmt_hhmm(close_min, t_close);

    /* 3. Print (no lock held). */
    printf("\n=== System Summary ===\n");
    printf("Time            : %s\n", t_now);
    printf("Access window   : %s - %s (currently %s)\n", t_open, t_close,
           window_open ? "OPEN" : "CLOSED");
    printf("People inside   : %d\n", building_count);
    for (int r = 0; r < NUM_ROOMS; r++) {
        printf("Room %d          : %-8s | light %-3s | on for %lu s\n", r + 1,
               occupied[r] ? "OCCUPIED" : "vacant",
               light_on[r] ? "ON" : "OFF",
               (unsigned long)on_s[r]);
    }
    printf("Vacancy timeout : %u s\n", (unsigned)vacancy_s);

    printf("\n--- Energy saved by automatic lighting ---\n");
    printf("(assumes %d W per room; baseline = light on since boot, uptime %lu s)\n",
           LAMP_WATTS_PER_ROOM, (unsigned long)uptime_s);
    uint64_t total = 0;
    for (int r = 0; r < NUM_ROOMS; r++) {
        uint64_t m = saved_mwh(uptime_s, on_s[r]);
        total += m;
        printf("Room %d saved    : ", r + 1);
        print_wh(m);
        printf("\n");
    }
    printf("Total saved     : ");
    print_wh(total);
    printf("\n\n");
}

void report_print_inside(void)
{
    typedef struct {
        char uid[UID_LEN];
        char name[NAME_LEN];
    } row_t;

    /* Heap, not stack: MAX_USERS rows would be a lot for a small task stack. */
    row_t *rows = malloc(MAX_USERS * sizeof(row_t));
    if (rows == NULL) {
        printf("report: out of memory\n");
        return;
    }

    int n = 0;
    STATE_LOCK();
    for (int i = 0; i < MAX_USERS; i++) {
        if (g_state.users[i].active && g_state.users[i].inside) {
            memcpy(rows[n].uid,  g_state.users[i].uid,  UID_LEN);
            memcpy(rows[n].name, g_state.users[i].name, NAME_LEN);
            rows[n].uid[UID_LEN - 1]   = '\0';
            rows[n].name[NAME_LEN - 1] = '\0';
            n++;
        }
    }
    STATE_UNLOCK();

    printf("\n=== People inside the building: %d ===\n", n);
    for (int i = 0; i < n; i++) {
        printf("%2d. %-*s  (UID %s)\n", i + 1, NAME_LEN - 1, rows[i].name, rows[i].uid);
    }
    if (n == 0) {
        printf("(nobody inside)\n");
    }
    printf("\n");
    free(rows);
}
