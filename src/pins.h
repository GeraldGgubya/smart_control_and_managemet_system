#pragma once
/*
 * pins.h - single source of truth for the GPIO map (one ESP32 DevKit in Wokwi).
 * Owner: Gerald. Do NOT hard-code pin numbers in your own files; use these names.
 * If you need a different pin, ask Gerald so the map and the report stay consistent.
 *
 * Avoided on purpose: GPIO 6-11 (flash), 0/2/12/15 (strapping pins).
 * GPIO 34/35 are input-only, which is fine for PIR sensors.
 */

/* ---- Shared SPI bus (SD card + two MFRC522 RFID readers) ---- */
#define PIN_SPI_MOSI        23
#define PIN_SPI_MISO        19
#define PIN_SPI_SCK         18
#define PIN_SD_CS            5
#define PIN_RFID_ENTRY_CS   21
#define PIN_RFID_EXIT_CS    22
#define PIN_RFID_RST         4

/* ---- Entrance (Daniel) ---- */
#define PIN_DOOR_ENTRY_SERVO 13
#define PIN_DOOR_EXIT_SERVO  14
#define PIN_LED_GRANTED      32
#define PIN_LED_DENIED       33
#define PIN_BUZZER           27

/* ---- Lecture rooms (Macalvin, Rahmat) ---- */
#define PIN_ROOM1_PIR        34
#define PIN_ROOM2_PIR        35
#define PIN_ROOM1_LIGHT      25
#define PIN_ROOM2_LIGHT      26

/* ---- Serial menu (Ariko) uses UART0 (default console), no extra pins ---- */
