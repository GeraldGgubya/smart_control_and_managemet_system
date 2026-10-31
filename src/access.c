/*
 * access.c - entrance terminal. Owner: Bukenya Daniel.
 *
 * Provides:  entrance_task()  and  rfid_poll()  (overrides the weak versions in stubs.c)
 *
 * What it does, in order, for every card scan on either door:
 *   1. get the UID (real MFRC522 reader first, typed "scan entry|exit <uid>" as fallback)
 *   2. decide  : unknown tag / outside hours / anti-passback / not inside / GRANT
 *                (check + state update happen inside ONE STATE_LOCK so they are atomic)
 *   3. act     : servo + green LED on grant; red LED + buzzer on deny (no blocking delays)
 *   4. log     : log_event(...) for every attempt
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "shared.h"

static const char *TAG = "access";

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */
#define LOOP_MS 100           /* entrance loop period                       */
#define DOOR_OPEN_MS 3000     /* how long a door stays open after a grant   */
#define DENY_FEEDBACK_MS 400  /* red LED + buzzer on a refused scan         */
#define RFID_COOLDOWN_MS 2000 /* ignore the same card re-read within this   */

#define SERVO_CLOSED_US 500   /* pulse width for ~0 degrees                 */
#define SERVO_OPEN_US 1500    /* pulse width for ~90 degrees                */
#define SERVO_PERIOD_US 20000 /* 50 Hz                                      */
#define SERVO_RES_BITS 14

#define RFID_SPI_HOST SPI2_HOST /* must match the host storage.c uses for the SD card */

/* ------------------------------------------------------------------ */
/* Tiny non-blocking timer (tick based, wrap-safe)                     */
/* ------------------------------------------------------------------ */
typedef struct
{
    bool active;
    TickType_t at;
} deadline_t;

static void dl_arm(deadline_t *d, uint32_t ms)
{
    d->active = true;
    d->at = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
}

static bool dl_due(const deadline_t *d)
{
    return d->active && (int32_t)(xTaskGetTickCount() - d->at) >= 0;
}

/* ------------------------------------------------------------------ */
/* Doors, LEDs, buzzer                                                 */
/* ------------------------------------------------------------------ */
static const int SERVO_PIN[2] = {PIN_DOOR_ENTRY_SERVO, PIN_DOOR_EXIT_SERVO};
static const ledc_channel_t SERVO_CH[2] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1};

static deadline_t s_door_close[2]; /* when each open door should close again */
static deadline_t s_deny_off;      /* when the red LED + buzzer should stop  */

static void servo_set(door_id_t door, uint32_t pulse_us)
{
    uint32_t duty = (pulse_us * (1u << SERVO_RES_BITS)) / SERVO_PERIOD_US;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CH[door], duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CH[door]);
}

static void outputs_init(void)
{
    const int outs[] = {PIN_LED_GRANTED, PIN_LED_DENIED, PIN_BUZZER, PIN_RFID_RST};
    for (int i = 0; i < 4; i++)
    {
        gpio_reset_pin(outs[i]);
        gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
        gpio_set_level(outs[i], 0);
    }

    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RES_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000000 / SERVO_PERIOD_US, /* 50 Hz */
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tc));

    for (int d = 0; d < 2; d++)
    {
        ledc_channel_config_t cc = {
            .gpio_num = SERVO_PIN[d],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = SERVO_CH[d],
            .timer_sel = LEDC_TIMER_0,
            .intr_type = LEDC_INTR_DISABLE,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&cc));
        servo_set((door_id_t)d, SERVO_CLOSED_US);
    }
}

static void door_open(door_id_t door)
{
    servo_set(door, SERVO_OPEN_US);
    dl_arm(&s_door_close[door], DOOR_OPEN_MS); /* re-scan while open just extends it */
}

static void deny_feedback(void)
{
    gpio_set_level(PIN_LED_DENIED, 1);
    gpio_set_level(PIN_BUZZER, 1);
    dl_arm(&s_deny_off, DENY_FEEDBACK_MS);
}

/* Called every loop: closes doors / switches indicators off when their time is up. */
static void outputs_service(void)
{
    for (int d = 0; d < 2; d++)
    {
        if (dl_due(&s_door_close[d]))
        {
            servo_set((door_id_t)d, SERVO_CLOSED_US);
            s_door_close[d].active = false;
        }
    }
    gpio_set_level(PIN_LED_GRANTED, s_door_close[0].active || s_door_close[1].active);

    if (dl_due(&s_deny_off))
    {
        gpio_set_level(PIN_LED_DENIED, 0);
        gpio_set_level(PIN_BUZZER, 0);
        s_deny_off.active = false;
    }
}

/* ------------------------------------------------------------------ */
/* MFRC522 (RC522) driver over the shared SPI bus                      */
/* ------------------------------------------------------------------ */
enum
{
    REG_COMMAND = 0x01,
    REG_COMIRQ = 0x04,
    REG_ERROR = 0x06,
    REG_FIFODATA = 0x09,
    REG_FIFOLEVEL = 0x0A,
    REG_CONTROL = 0x0C,
    REG_BITFRAMING = 0x0D,
    REG_COLL = 0x0E,
    REG_MODE = 0x11,
    REG_TXCONTROL = 0x14,
    REG_TXASK = 0x15,
    REG_TMODE = 0x2A,
    REG_TPRESCALER = 0x2B,
    REG_TRELOAD_H = 0x2C,
    REG_TRELOAD_L = 0x2D,
    REG_VERSION = 0x37,
};
enum
{
    CMD_IDLE = 0x00,
    CMD_TRANSCEIVE = 0x0C,
    CMD_SOFTRESET = 0x0F
};

static spi_device_handle_t s_rc[2];
static bool s_rc_ok[2];

/* Every transaction takes g_spi_mutex because the SD card shares this bus.
 * We never hold g_state_mutex while doing this (locking rules in shared.h). */
static uint8_t rc_read(door_id_t d, uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00};
    uint8_t rx[2] = {0, 0};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
    xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    spi_device_transmit(s_rc[d], &t);
    xSemaphoreGive(g_spi_mutex);
    return rx[1];
}

static void rc_write(door_id_t d, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t)((reg << 1) & 0x7E), val};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx};
    xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    spi_device_transmit(s_rc[d], &t);
    xSemaphoreGive(g_spi_mutex);
}

static void rc_set_bits(door_id_t d, uint8_t reg, uint8_t mask) { rc_write(d, reg, rc_read(d, reg) | mask); }
static void rc_clr_bits(door_id_t d, uint8_t reg, uint8_t mask) { rc_write(d, reg, rc_read(d, reg) & (uint8_t)~mask); }

static void rc_init_chip(door_id_t d)
{
    rc_write(d, REG_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    rc_write(d, REG_TMODE, 0x80); /* timer auto-start                         */
    rc_write(d, REG_TPRESCALER, 0xA9);
    rc_write(d, REG_TRELOAD_H, 0x03); /* ~25 ms timeout                           */
    rc_write(d, REG_TRELOAD_L, 0xE8);
    rc_write(d, REG_TXASK, 0x40); /* 100% ASK modulation                      */
    rc_write(d, REG_MODE, 0x3D);  /* CRC preset 0x6363                        */
    if ((rc_read(d, REG_TXCONTROL) & 0x03) != 0x03)
    {
        rc_set_bits(d, REG_TXCONTROL, 0x03); /* antenna on                         */
    }
}

/* Send bytes to the card and read the reply. tx_bits = valid bits in last tx byte
 * (0 means all 8; REQA uses 7). Returns true on a clean reply. */
static bool rc_transceive(door_id_t d, const uint8_t *tx, uint8_t tx_len, uint8_t tx_bits,
                          uint8_t *rx, uint8_t *rx_len)
{
    rc_write(d, REG_COMMAND, CMD_IDLE);
    rc_write(d, REG_COMIRQ, 0x7F);    /* clear all interrupt flags */
    rc_write(d, REG_FIFOLEVEL, 0x80); /* flush FIFO                */
    for (uint8_t i = 0; i < tx_len; i++)
        rc_write(d, REG_FIFODATA, tx[i]);
    rc_write(d, REG_BITFRAMING, tx_bits);
    rc_write(d, REG_COMMAND, CMD_TRANSCEIVE);
    rc_set_bits(d, REG_BITFRAMING, 0x80); /* StartSend                 */

    bool done = false;
    for (int i = 0; i < 6 && !done; i++)
    { /* up to ~60 ms              */
        uint8_t irq = rc_read(d, REG_COMIRQ);
        if (irq & 0x30)
            done = true; /* RxIRq or IdleIRq          */
        else if (irq & 0x01)
            return false; /* TimerIRq: no card replied */
        else
            vTaskDelay(1);
    }
    if (!done)
        return false;
    if (rc_read(d, REG_ERROR) & 0x13)
        return false; /* buffer overflow, parity, protocol */

    uint8_t n = rc_read(d, REG_FIFOLEVEL);
    if (n == 0 || n > *rx_len)
        return false;
    for (uint8_t i = 0; i < n; i++)
        rx[i] = rc_read(d, REG_FIFODATA);
    *rx_len = n;
    return true;
}

/* Reads a 4-byte UID into uid_out as uppercase hex, no colons ("A1B2C3D4"). */
static bool rc_read_uid(door_id_t d, char uid_out[UID_LEN])
{
    uint8_t buf[8], len;

    rc_clr_bits(d, REG_COLL, 0x80); /* ValuesAfterColl = 0        */
    uint8_t reqa = 0x26;            /* REQA: is any card present? */
    len = 2;
    if (!rc_transceive(d, &reqa, 1, 7, buf, &len) || len != 2)
        return false;

    uint8_t anticoll[2] = {0x93, 0x20}; /* anti-collision, cascade 1  */
    len = 8;
    if (!rc_transceive(d, anticoll, 2, 0, buf, &len) || len != 5)
        return false;

    if ((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) != buf[4])
        return false; /* check byte (BCC) */

    snprintf(uid_out, UID_LEN, "%02X%02X%02X%02X", buf[0], buf[1], buf[2], buf[3]);
    return true;
}

static void rfid_init(void)
{
    /* Reset pulse shared by both readers. */
    gpio_set_level(PIN_RFID_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RFID_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t e = spi_bus_initialize(RFID_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
    { /* INVALID_STATE = storage.c got there first */
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(e));
        return;
    }

    const int cs[2] = {PIN_RFID_ENTRY_CS, PIN_RFID_EXIT_CS};
    const char *label[2] = {"entry", "exit"};
    for (int d = 0; d < 2; d++)
    {
        spi_device_interface_config_t dev = {
            .clock_speed_hz = 1000000,
            .mode = 0,
            .spics_io_num = cs[d],
            .queue_size = 1,
        };
        if (spi_bus_add_device(RFID_SPI_HOST, &dev, &s_rc[d]) != ESP_OK)
        {
            ESP_LOGE(TAG, "cannot add %s reader to SPI bus", label[d]);
            continue;
        }
        rc_init_chip((door_id_t)d);
        uint8_t ver = rc_read((door_id_t)d, REG_VERSION);
        s_rc_ok[d] = (ver == 0x91 || ver == 0x92);
        ESP_LOGI(TAG, "%s reader VersionReg=0x%02X -> %s", label[d], ver,
                 s_rc_ok[d] ? "OK" : "NOT DETECTED (typed scans still work)");
    }
}

/* ------------------------------------------------------------------ */
/* rfid_poll: strong definition, overrides the weak one in stubs.c     */
/* ------------------------------------------------------------------ */
bool rfid_poll(door_id_t door, char uid_out[UID_LEN])
{
    if ((int)door < 0 || (int)door > 1)
        return false;

    /* 1. real reader, with a cooldown so one tap is not logged ten times */
    static char last_uid[2][UID_LEN];
    static TickType_t last_tick[2];
    char uid[UID_LEN];
    if (s_rc_ok[door] && rc_read_uid(door, uid))
    {
        TickType_t now = xTaskGetTickCount();
        bool repeat = (strcmp(uid, last_uid[door]) == 0) &&
                      (now - last_tick[door]) < pdMS_TO_TICKS(RFID_COOLDOWN_MS);
        strlcpy(last_uid[door], uid, UID_LEN);
        last_tick[door] = now; /* held card keeps extending the cooldown */
        if (!repeat)
        {
            strlcpy(uid_out, uid, UID_LEN);
            return true;
        }
    }

    /* 2. simulated scan typed in the serial menu (same behaviour as the stub) */
    scan_msg_t m;
    if (xQueueReceive(g_scan_queue[door], &m, 0) == pdTRUE)
    {
        strlcpy(uid_out, m.uid, UID_LEN);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Access decision                                                     */
/* ------------------------------------------------------------------ */
typedef enum
{
    DEC_GRANT,
    DEC_UNKNOWN,
    DEC_CLOSED,
    DEC_PASSBACK,
    DEC_NOT_INSIDE
} decision_t;

/* The check AND the state change happen under one lock, so two scans can never
 * both pass the anti-passback test. Nothing slow happens while the lock is held. */
static decision_t decide(const char *uid, bool entering, bool window_open)
{
    decision_t d;
    STATE_LOCK();
    int i = user_find_locked(uid);
    if (i < 0)
    {
        d = DEC_UNKNOWN;
    }
    else if (!window_open)
    {
        d = DEC_CLOSED; /* applies to entry AND exit */
    }
    else if (entering && g_state.users[i].inside)
    {
        d = DEC_PASSBACK;
    }
    else if (!entering && !g_state.users[i].inside)
    {
        d = DEC_NOT_INSIDE;
    }
    else
    {
        g_state.users[i].inside = entering;
        if (entering)
        {
            g_state.building_count++;
        }
        else if (g_state.building_count > 0)
        {
            g_state.building_count--;
        }
        d = DEC_GRANT;
    }
    STATE_UNLOCK();
    return d;
}

static void handle_scan(door_id_t door, const char *raw_uid)
{
    char uid[UID_LEN];
    size_t k = 0;
    for (; raw_uid[k] && k < UID_LEN - 1; k++)
        uid[k] = (char)toupper((unsigned char)raw_uid[k]);
    uid[k] = '\0';

    bool entering = (door == DOOR_ENTRY);
    /* Read the window flag BEFORE taking the state lock (access_window_open() locks internally). */
    bool window_open = (xEventGroupGetBits(g_events) & EVT_BIT_ACCESS_OPEN) != 0;

    decision_t d = decide(uid, entering, window_open);

    const char *note; /* short text for the SD log (log_event keeps 23 characters) */
    const char *msg;  /* longer text for the console */
    switch (d)
    {
    case DEC_GRANT:
        note = "ok";
        msg = entering ? "ACCESS GRANTED: welcome, door opening"
                       : "ACCESS GRANTED: goodbye, door opening";
        break;
    case DEC_UNKNOWN:
        note = "unknown tag";
        msg = "ACCESS DENIED: tag is not registered";
        break;
    case DEC_CLOSED:
        note = "outside hours";
        msg = entering ? "ACCESS DENIED: building is closed (outside access hours)"
                       : "ACCESS DENIED: exit blocked outside access hours";
        break;
    case DEC_PASSBACK:
        note = "anti-passback";
        msg = "ACCESS DENIED: already inside the building (anti-passback)";
        break;
    default:
        note = "not inside";
        msg = "ACCESS DENIED: not recorded as inside, cannot exit";
        break;
    }

    event_type_t ev = entering ? (d == DEC_GRANT ? EVT_ENTRY_GRANTED : EVT_ENTRY_DENIED)
                               : (d == DEC_GRANT ? EVT_EXIT_GRANTED : EVT_EXIT_DENIED);
    log_event(ev, (uint8_t)door, uid, note);

    /* Printed after decide() has released the lock, so no lock is held while printing. */
    printf("\n[%s] %s -> %s\n", entering ? "ENTRY" : "EXIT", uid, msg);

    if (d == DEC_GRANT)
        door_open(door);
    else
        deny_feedback();
}

/* ------------------------------------------------------------------ */
/* The entrance task                                                   */
/* ------------------------------------------------------------------ */
void entrance_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(500)); /* let storage_task claim the SPI bus first */
    outputs_init();
    rfid_init();
    log_event(EVT_SYSTEM, 0, NULL, "entrance ready");

    for (;;)
    {
        for (int door = 0; door < 2; door++)
        {
            char uid[UID_LEN];
            if (rfid_poll((door_id_t)door, uid))
            {
                handle_scan((door_id_t)door, uid);
            }
        }
        outputs_service();
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
