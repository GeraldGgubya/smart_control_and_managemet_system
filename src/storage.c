/*
 * storage.c - SD card logging. Owner: Ariko.
 *
 * storage_task is the ONLY code that touches the SD card:
 *   - it mounts the card (FAT at /sdcard) on the shared SPI bus,
 *   - it receives log_event_t items from g_log_queue and appends them to
 *     /sdcard/events.csv (timestamp,type,source,uid,note),
 *   - it prints the last n records when cli_task asks for them.
 * Every SD access is wrapped in g_spi_mutex because the RFID readers share
 * the same SPI bus. If the card is missing the task keeps running.
 *
 * NOTE: written without compiling against ESP-IDF here. Build it first.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "shared.h"
#include "storage.h"

static const char *TAG = "storage";

/* 8.3 file name on purpose: long file names are off by default in ESP-IDF FATFS */
#define MOUNT_POINT   "/sdcard"
#define LOG_PATH      MOUNT_POINT "/events.csv"
#define LOG_HEADER    "timestamp,type,source,uid,note\n"

#define LINE_LEN      128      /* one CSV line, longest real line is ~85 chars */
#define SHOW_MAX      30       /* most records "log" can print */
#define TAIL_BYTES    4096     /* "log" only reads this much from the end of the file */
#define SPI_WAIT_MS   2000     /* how long to wait for g_spi_mutex */
#define RETRY_MS      60000    /* try to mount again this often if the card is missing */

static sdmmc_card_t *s_card = NULL;
static bool          s_mounted = false;

/* Used by cli_task to ask for "log n". Created at the top of storage_task. */
static QueueHandle_t     s_req_queue = NULL;   /* holds the requested n */
static SemaphoreHandle_t s_req_done  = NULL;   /* given when printing has finished */

/* Ring buffer for the last lines of the log file (static: keeps the stack small) */
static char s_ring[SHOW_MAX][LINE_LEN];

/* Names for the CSV "type" column. Same order as event_type_t in shared.h. */
static const char *const EVENT_NAMES[] = {
    "ENTRY_GRANTED", "ENTRY_DENIED", "EXIT_GRANTED", "EXIT_DENIED",
    "ROOM_OCCUPIED", "ROOM_VACANT", "LIGHT_ON", "LIGHT_OFF",
    "SCHEDULE_CHANGED", "ADMIN_LOGIN_OK", "ADMIN_LOGIN_FAIL",
    "ALERT", "SYSTEM"
};
_Static_assert(sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]) == EVT_SYSTEM + 1,
               "EVENT_NAMES must match event_type_t in shared.h");

/* ------------------------------------------------------------------ */
/* Mounting                                                            */
/* ------------------------------------------------------------------ */
static bool sd_mount(void)
{
    static bool warned = false;           /* only warn on the first failure */

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    /* ESP_ERR_INVALID_STATE means the bus was already initialised (by the RFID
     * code, or by an earlier mount attempt of ours). That is fine. */
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        if (!warned) ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        warned = true;
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,   /* blank virtual card in Wokwi gets formatted */
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };

    if (xSemaphoreTake(g_spi_mutex, pdMS_TO_TICKS(SPI_WAIT_MS)) != pdTRUE) {
        if (!warned) ESP_LOGW(TAG, "SPI bus busy, cannot mount SD card");
        warned = true;
        return false;
    }

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret == ESP_OK) {
        /* new card: write the CSV header once */
        FILE *f = fopen(LOG_PATH, "r");
        if (f != NULL) {
            fclose(f);
        } else {
            f = fopen(LOG_PATH, "w");
            if (f != NULL) {
                fputs(LOG_HEADER, f);
                fclose(f);
            }
        }
    }
    xSemaphoreGive(g_spi_mutex);

    if (ret != ESP_OK) {
        if (!warned) {
            ESP_LOGW(TAG, "SD card not available (%s). Events will only be printed "
                          "to the console. Will retry every %d s.",
                     esp_err_to_name(ret), RETRY_MS / 1000);
        }
        warned = true;
        return false;
    }

    s_mounted = true;
    warned = false;                       /* warn again if it fails later */
    return true;
}

/* ------------------------------------------------------------------ */
/* Writing log records                                                 */
/* ------------------------------------------------------------------ */

/* Copy src to dst, replacing characters that would break the CSV format. */
static void copy_clean(char *dst, const char *src, size_t dst_len)
{
    size_t i = 0;
    while (i < dst_len - 1 && src[i] != '\0') {
        char c = src[i];
        dst[i] = (c == ',' || c == '\r' || c == '\n') ? ' ' : c;
        i++;
    }
    dst[i] = '\0';
}

static void format_line(const log_event_t *e, char *out, size_t out_len)
{
    char ts[24];
    time_t t = (time_t)e->ts;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    const char *type_name = "UNKNOWN";
    if ((int)e->type >= 0 && (int)e->type <= EVT_SYSTEM) {
        type_name = EVENT_NAMES[e->type];
    }

    char uid[UID_LEN];
    char note[sizeof(e->note)];
    copy_clean(uid, e->uid, sizeof(uid));
    copy_clean(note, e->note, sizeof(note));

    snprintf(out, out_len, "%s,%s,%u,%s,%s\n", ts, type_name, (unsigned)e->source, uid, note);
}

static void write_event(const log_event_t *e)
{
    char line[LINE_LEN];
    format_line(e, line, sizeof(line));

    if (!s_mounted) {
        printf("[no SD] %s", line);       /* at least show the event on the console */
        return;
    }

    if (xSemaphoreTake(g_spi_mutex, pdMS_TO_TICKS(SPI_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "SPI bus busy, event not stored: %s", e->note);
        return;
    }

    FILE *f = fopen(LOG_PATH, "a");
    if (f != NULL) {
        fputs(line, f);
        fclose(f);                        /* fclose flushes the data to the card */
        xSemaphoreGive(g_spi_mutex);
        return;
    }

    /* Could not open the file: the card was probably removed. */
    xSemaphoreGive(g_spi_mutex);
    ESP_LOGW(TAG, "cannot open %s, unmounting SD card", LOG_PATH);
    s_mounted = false;
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    printf("[no SD] %s", line);
}

/* ------------------------------------------------------------------ */
/* Reading: "log n"                                                    */
/* ------------------------------------------------------------------ */
static void print_last(int n)
{
    if (n < 1) n = 1;
    if (n > SHOW_MAX) n = SHOW_MAX;

    if (!s_mounted) {
        printf("SD card not available, no records to show\n");
        return;
    }
    if (xSemaphoreTake(g_spi_mutex, pdMS_TO_TICKS(SPI_WAIT_MS)) != pdTRUE) {
        printf("SPI bus busy, try again\n");
        return;
    }

    FILE *f = fopen(LOG_PATH, "r");
    if (f == NULL) {
        xSemaphoreGive(g_spi_mutex);
        printf("No log file yet\n");
        return;
    }

    /* Only read the end of the file so a big log does not make "log" slow. */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    char buf[LINE_LEN];
    if (size > TAIL_BYTES) {
        fseek(f, size - TAIL_BYTES, SEEK_SET);
        if (fgets(buf, sizeof(buf), f) == NULL) {   /* throw away the partial first line */
            buf[0] = '\0';
        }
    } else {
        fseek(f, 0, SEEK_SET);
    }

    int total = 0;                        /* lines read; ring keeps the last SHOW_MAX */
    while (fgets(buf, sizeof(buf), f) != NULL) {
        if (strncmp(buf, "timestamp,", 10) == 0) {
            continue;                     /* skip the header line */
        }
        strcpy(s_ring[total % SHOW_MAX], buf);
        total++;
    }
    fclose(f);
    xSemaphoreGive(g_spi_mutex);          /* release the bus BEFORE printing */

    if (total == 0) {
        printf("No records yet\n");
        return;
    }
    int count = (total < n) ? total : n;
    printf("Last %d record(s)  [timestamp,type,source,uid,note]:\n", count);
    for (int i = total - count; i < total; i++) {
        printf("  %s", s_ring[i % SHOW_MAX]);
    }
}

/* Called from cli_task. storage_task does the real work. */
bool storage_print_last(int n)
{
    if (s_req_queue == NULL || s_req_done == NULL) {
        return false;                     /* storage_task has not started yet */
    }
    xSemaphoreTake(s_req_done, 0);        /* clear a stale "done" signal */
    if (xQueueSend(s_req_queue, &n, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    xSemaphoreTake(s_req_done, pdMS_TO_TICKS(8000));   /* wait until printing is finished */
    return true;
}

/* ------------------------------------------------------------------ */
/* The task                                                            */
/* ------------------------------------------------------------------ */
void storage_task(void *arg)
{
    (void)arg;

    s_req_queue = xQueueCreate(1, sizeof(int));
    s_req_done  = xSemaphoreCreateBinary();
    configASSERT(s_req_queue && s_req_done);

    if (sd_mount()) {
        ESP_LOGI(TAG, "SD card mounted, logging to %s", LOG_PATH);
    }

    TickType_t last_try = xTaskGetTickCount();

    for (;;) {
        log_event_t e;

        /* 1. store the next event (wait up to 100 ms) */
        if (xQueueReceive(g_log_queue, &e, pdMS_TO_TICKS(100)) == pdTRUE) {
            write_event(&e);
        }

        /* 2. did cli_task ask for "log n"? */
        int n;
        if (xQueueReceive(s_req_queue, &n, 0) == pdTRUE) {
            /* store everything still waiting, so the newest events show up */
            while (xQueueReceive(g_log_queue, &e, 0) == pdTRUE) {
                write_event(&e);
            }
            print_last(n);
            xSemaphoreGive(s_req_done);
        }

        /* 3. card missing? try again once in a while */
        if (!s_mounted && (xTaskGetTickCount() - last_try) >= pdMS_TO_TICKS(RETRY_MS)) {
            last_try = xTaskGetTickCount();
            if (sd_mount()) {
                ESP_LOGI(TAG, "SD card is now available, logging resumed");
            }
        }
    }
}