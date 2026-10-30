/*
 * cli.c - serial menu and admin authentication. Owner: Ariko.
 *
 * cli_task reads a line from the serial console and runs a command:
 *   help | status | inside | log [n]
 *   scan entry <uid> | scan exit <uid>      (simulated RFID tap)
 *   time | time set <epoch>
 *   schedule show | schedule set HH:MM HH:MM
 *   login <pin> | logout
 * Changing the schedule (and the clock) needs an admin login.
 * After 3 wrong PINs, login is locked for 30 seconds.
 *
 * NOTE: written without compiling against ESP-IDF here. Build it first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "driver/uart.h"
#include "shared.h"
#include "storage.h"

#define LINE_MAX_LEN     80
#define ARGV_MAX         6
#define LOG_DEFAULT_N    10

#define ADMIN_PIN        "1234"     /* demo PIN, change it before the demo/report */
#define LOGIN_MAX_FAILS  3
#define LOCKOUT_MS       30000      /* 30 s lockout after 3 wrong PINs */

/* 1 = "time set" also needs login (otherwise anyone could move the clock into
 * the access window and bypass the schedule). Set to 0 to allow it without login. */
#define TIME_SET_NEEDS_LOGIN  1

/* Login state. Only cli_task uses these, so no mutex is needed. */
static bool      s_admin = false;
static int       s_fail_count = 0;
static bool      s_locked = false;
static TickType_t s_lock_start = 0;

/* ------------------------------------------------------------------ */
/* Reading a line from the serial port                                 */
/* ------------------------------------------------------------------ */
/* The Wokwi serial monitor does not echo what you type, so we echo it ourselves. */
static void read_line(char *buf, int max)
{
    int len = 0;
    uint8_t c;

    for (;;) {
        int n = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0) {
                continue;                 /* ignore empty lines (also handles CR+LF) */
            }
            printf("\n");
            break;
        }
        if ((c == 8 || c == 127) && len > 0) {   /* backspace */
            len--;
            printf("\b \b");
            fflush(stdout);
        } else if (c >= 32 && c < 127 && len < max - 1) {
            buf[len++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
    buf[len] = '\0';
}

static int split_args(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *save = NULL;
    char *tok = strtok_r(line, " \t", &save);
    while (tok != NULL && argc < max_args) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return argc;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Seconds of lockout left (0 = not locked). Also ends the lockout when it is over. */
static int lockout_seconds_left(void)
{
    if (!s_locked) {
        return 0;
    }
    uint32_t elapsed_ms = (uint32_t)(xTaskGetTickCount() - s_lock_start) * portTICK_PERIOD_MS;
    if (elapsed_ms >= LOCKOUT_MS) {
        s_locked = false;
        s_fail_count = 0;
        return 0;
    }
    return (int)((LOCKOUT_MS - elapsed_ms + 999) / 1000);
}

/* Parse "HH:MM" into minutes since midnight. Returns false if it is not valid. */
static bool parse_hhmm(const char *text, uint16_t *minutes_out)
{
    int h, m;
    char extra;
    if (sscanf(text, "%d:%d%c", &h, &m, &extra) != 2) {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    *minutes_out = (uint16_t)(h * 60 + m);
    return true;
}

/* Turn what the user typed into the UID format used by the project:
 * uppercase letters/digits only, colons removed (A1:B2:C3:D4 -> A1B2C3D4). */
static bool normalise_uid(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    for (; *in != '\0'; in++) {
        if (*in == ':') {
            continue;
        }
        if (!isalnum((unsigned char)*in) || n >= out_len - 1) {
            return false;
        }
        out[n++] = (char)toupper((unsigned char)*in);
    }
    out[n] = '\0';
    return n > 0;
}

static void show_time(void)
{
    time_t now = (time_t)clock_now();
    struct tm tm_info;
    char buf[32];
    localtime_r(&now, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    printf("Time: %s EAT  (epoch %lu)\n", buf, (unsigned long)now);
}

/* Used by every admin-only command. Prints a message and returns false if not logged in. */
static bool require_admin(void)
{
    if (!s_admin) {
        printf("Access denied: login required (login <pin>)\n");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */
static void cmd_help(void)
{
    printf("Commands:\n");
    printf("  help                          this list\n");
    printf("  status                        building, rooms, lights, energy\n");
    printf("  inside                        who is inside the building\n");
    printf("  log [n]                       last n log records from the SD card\n");
    printf("  scan entry <uid>              simulate a tag at the entry reader\n");
    printf("  scan exit <uid>               simulate a tag at the exit reader\n");
    printf("  time                          show the current time\n");
    printf("  time set <epoch>              set the clock%s\n",
           TIME_SET_NEEDS_LOGIN ? "   [admin]" : "");
    printf("  schedule show                 show the access hours\n");
    printf("  schedule set HH:MM HH:MM      set open and close time   [admin]\n");
    printf("  login <pin>                   start admin session\n");
    printf("  logout                        end admin session\n");
}

static void cmd_log(int argc, char **argv)
{
    int n = LOG_DEFAULT_N;
    if (argc >= 2) {
        n = atoi(argv[1]);
        if (n < 1) {
            printf("Usage: log [n]\n");
            return;
        }
    }
    if (!storage_print_last(n)) {
        printf("Storage is not ready yet, try again\n");
    }
}

static void cmd_scan(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: scan entry <uid>  |  scan exit <uid>\n");
        return;
    }
    door_id_t door;
    if (strcmp(argv[1], "entry") == 0) {
        door = DOOR_ENTRY;
    } else if (strcmp(argv[1], "exit") == 0) {
        door = DOOR_EXIT;
    } else {
        printf("Usage: scan entry <uid>  |  scan exit <uid>\n");
        return;
    }

    scan_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    if (!normalise_uid(argv[2], msg.uid, sizeof(msg.uid))) {
        printf("Invalid UID (letters and digits only, max %d characters)\n", UID_LEN - 1);
        return;
    }

    if (xQueueSend(g_scan_queue[door], &msg, 0) == pdTRUE) {
        printf("Simulated tag %s at the %s reader\n", msg.uid, argv[1]);
    } else {
        printf("Reader is busy, try again\n");
    }
}

static void cmd_time(int argc, char **argv)
{
    if (argc == 1) {
        show_time();
        return;
    }
    if (strcmp(argv[1], "set") != 0 || argc < 3) {
        printf("Usage: time  |  time set <epoch>\n");
        return;
    }
#if TIME_SET_NEEDS_LOGIN
    if (!require_admin()) {
        return;
    }
#endif
    char *end;
    unsigned long epoch = strtoul(argv[2], &end, 10);
    if (*end != '\0' || epoch < 1000000000UL) {
        printf("Invalid epoch. Example: time set 1793336400\n");
        return;
    }
    clock_set((uint32_t)epoch);
    log_event(EVT_SYSTEM, 0, NULL, "time set");
    show_time();
}

static void cmd_schedule(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "show") == 0) {
        STATE_LOCK();
        access_schedule_t s = g_state.schedule;      /* copy, then unlock before printing */
        STATE_UNLOCK();
        bool open_now = access_window_open();        /* takes the lock itself */
        printf("Access hours: %02d:%02d - %02d:%02d  (%s now)\n",
               s.open_min / 60, s.open_min % 60, s.close_min / 60, s.close_min % 60,
               open_now ? "OPEN" : "CLOSED");
        return;
    }

    if (argc >= 2 && strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            printf("Usage: schedule set HH:MM HH:MM\n");
            return;
        }
        if (!s_admin) {
            printf("Access denied: login required (login <pin>)\n");
            log_event(EVT_ALERT, 0, NULL, "sched set w/o login");
            return;
        }
        uint16_t open_min, close_min;
        if (!parse_hhmm(argv[2], &open_min) || !parse_hhmm(argv[3], &close_min)) {
            printf("Invalid time. Use HH:MM on a 24 hour clock, e.g. 07:00 20:30\n");
            return;
        }
        if (schedule_set(open_min, close_min)) {     /* schedule_set logs the change */
            printf("Schedule updated: %02d:%02d - %02d:%02d\n",
                   open_min / 60, open_min % 60, close_min / 60, close_min % 60);
        } else {
            printf("Schedule rejected (open and close time must be different)\n");
        }
        return;
    }

    printf("Usage: schedule show  |  schedule set HH:MM HH:MM\n");
}

static void cmd_login(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: login <pin>\n");
        return;
    }
    if (s_admin) {
        printf("Already logged in\n");
        return;
    }

    int left = lockout_seconds_left();
    if (left > 0) {
        printf("Login locked. Try again in %d s\n", left);
        log_event(EVT_ADMIN_LOGIN_FAIL, 0, NULL, "attempt while locked");
        return;
    }

    if (strcmp(argv[1], ADMIN_PIN) == 0) {
        s_admin = true;
        s_fail_count = 0;
        printf("Login successful. Admin commands are now allowed.\n");
        log_event(EVT_ADMIN_LOGIN_OK, 0, NULL, "admin");
        return;
    }

    s_fail_count++;
    if (s_fail_count >= LOGIN_MAX_FAILS) {
        s_locked = true;
        s_lock_start = xTaskGetTickCount();
        printf("Wrong PIN. Too many attempts: login locked for %d s\n", LOCKOUT_MS / 1000);
        log_event(EVT_ADMIN_LOGIN_FAIL, 0, NULL, "bad pin, locked 30s");
    } else {
        char note[48];                    /* log_event() keeps only the first 23 characters */
        snprintf(note, sizeof(note), "bad pin %d of %d", s_fail_count, LOGIN_MAX_FAILS);
        printf("Wrong PIN (%d of %d)\n", s_fail_count, LOGIN_MAX_FAILS);
        log_event(EVT_ADMIN_LOGIN_FAIL, 0, NULL, note);
    }
}

static void cmd_logout(void)
{
    if (!s_admin) {
        printf("You are not logged in\n");
        return;
    }
    s_admin = false;
    printf("Logged out\n");
    log_event(EVT_SYSTEM, 0, NULL, "admin logout");
}

/* ------------------------------------------------------------------ */
/* The task                                                            */
/* ------------------------------------------------------------------ */
void cli_task(void *arg)
{
    (void)arg;

    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    }

    char line[LINE_MAX_LEN];
    char *argv[ARGV_MAX];

    vTaskDelay(pdMS_TO_TICKS(1500));          /* let the boot messages finish */
    printf("\n=== CoCIS Smart Control System ===\n");
    printf("Type 'help' for the list of commands\n");

    for (;;) {
        printf(s_admin ? "admin> " : "> ");
        fflush(stdout);

        read_line(line, sizeof(line));
        int argc = split_args(line, argv, ARGV_MAX);
        if (argc == 0) {
            continue;
        }

        if (strcmp(argv[0], "help") == 0) {
            cmd_help();
        } else if (strcmp(argv[0], "status") == 0) {
            report_print_summary();
        } else if (strcmp(argv[0], "inside") == 0) {
            report_print_inside();
        } else if (strcmp(argv[0], "log") == 0) {
            cmd_log(argc, argv);
        } else if (strcmp(argv[0], "scan") == 0) {
            cmd_scan(argc, argv);
        } else if (strcmp(argv[0], "time") == 0) {
            cmd_time(argc, argv);
        } else if (strcmp(argv[0], "schedule") == 0) {
            cmd_schedule(argc, argv);
        } else if (strcmp(argv[0], "login") == 0) {
            cmd_login(argc, argv);
        } else if (strcmp(argv[0], "logout") == 0) {
            cmd_logout();
        } else {
            printf("Unknown command '%s'. Type 'help'.\n", argv[0]);
        }
    }
}