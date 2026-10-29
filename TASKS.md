# Smart Control & Monitoring System: Team Tasks

Deadline: **2 Nov 2026**. One ESP32, ESP-IDF, Wokwi (free). Team leader: Ggubya Gerald.

## Rules (read first)
1. Clone the repo, then create your own branch: `git checkout -b feat/<yourpart>`.
2. Edit **only your own file(s)**. Do NOT edit `shared.h`, `pins.h`, `main.c`, `stubs.c`. Need a change? Ask Gerald.
3. Build with `pio run` (open the folder that contains `platformio.ini`). It must build before you push.
4. Use pin names from `pins.h` and types/functions from `shared.h`. Never hard-code pins.
5. Locking rules (in `shared.h`): hold `STATE_LOCK()` only briefly; never do SD/serial/delay while holding it; only `storage_task` touches the SD card; send events with `log_event()`.
6. Commit often under your own name and push your branch. `git pull --rebase origin main` before pushing.
7. **You must be able to explain every line you write.** Marks are lost otherwise. Write it yourself.
8. Do not delete the stub for your function; your real function in your own file overrides it automatically.

## Timeline (revised)
- **30 Oct:** repo cloned, builds, you have started your file.
- **31 Oct, 8 pm:** your part works on its own and is pushed to your branch. Send Gerald a short status.
- **1 Nov, morning:** integration and testing (Gerald merges). Fix bugs.
- **1 Nov, afternoon/evening:** report contributions + record your own demo video.
- **2 Nov:** final check and early submission.

## Tasks

### Bukenya Daniel: `src/access.c` (entrance)
Implement `entrance_task`.
- Loop every ~100 ms; call `rfid_poll(DOOR_ENTRY, uid)` and `rfid_poll(DOOR_EXIT, uid)`.
- On a scan: check the access window (`xEventGroupGetBits(g_events) & EVT_BIT_ACCESS_OPEN`) and look up the user (`user_find_locked`, under `STATE_LOCK()`).
- Entry rules: user must exist, window must be open, and user must not already be inside (**anti-passback**). On success: set `inside = true`, `building_count++`, open the entry door, green LED. Otherwise: denied, red LED + buzzer.
- Exit rules: same checks. Per the brief, exit is also blocked outside the access window, and the user must currently be inside.
- Drive the door servos (LEDC PWM) on `PIN_DOOR_ENTRY_SERVO` / `PIN_DOOR_EXIT_SERVO`: open for ~3 s, then close.
- Log every attempt with `log_event(EVT_ENTRY_GRANTED / EVT_ENTRY_DENIED / EVT_EXIT_GRANTED / EVT_EXIT_DENIED, door, uid, reason)`.
- **Real RFID:** Wokwi has the `board-mfrc522` reader. `diagram.json` already has two (entry on CS `PIN_RFID_ENTRY_CS`, exit on `PIN_RFID_EXIT_CS`, shared RST/MISO/MOSI/SCK). Click a reader in Wokwi to pick a card (Blue, Green, ...) and press **t** to tap it. Both readers' Blue Card UID is set to `A1:B2:C3:D4`.
- ESP-IDF has no built-in RC522 driver. Write `rfid_poll()` yourself in `access.c` (SPI register access; read the VersionReg first to prove the wiring works, then read the card UID), or try an existing ESP-IDF RC522 component. Format the UID as uppercase hex **without colons** (e.g. `A1B2C3D4`). Take `g_spi_mutex` around every SPI transaction, since the SD card shares the bus.
- Until your reader works, the default `rfid_poll()` reads simulated scans typed in the serial menu, so nobody else is blocked.
- Done when: valid entry opens the door and updates the count; repeat entry is denied; entry/exit outside hours are denied.

### Wamala Macalvin: `src/room.c` (rooms and lighting)
Implement `room_task` (one function, started twice; `room = (int)(intptr_t)arg`, 0 or 1).
- Read that room's PIR (`PIN_ROOM1_PIR` / `PIN_ROOM2_PIR`); debounce it.
- Motion: mark `room_occupied`, record `room_last_motion`, switch the light on (`PIN_ROOM1_LIGHT` / `PIN_ROOM2_LIGHT`). No motion for `vacancy_timeout_s`: mark vacant and switch the light off.
- Update `g_state` under the mutex; accumulate `light_on_seconds`.
- Log `EVT_ROOM_OCCUPIED/VACANT` and `EVT_LIGHT_ON/OFF` only on changes (not every loop).
- Done when: motion in a room turns its light on, and the light turns off after the timeout. Both rooms work independently.

### Namayanja Rahmat: `src/report.c` (reports and energy estimate)
Implement `report_print_summary()` and `report_print_inside()` (declared in `shared.h`).
- Summary: building count, each room's occupied/vacant and light on/off, current access window.
- Inside list: names/UIDs of users currently inside.
- **Innovation:** an energy-saved estimate per room. Assume a lamp wattage, compare lights-on time (`light_on_seconds`) against the time they would have been on without automation, and print Wh saved.
- Copy state under the lock, release it, then print.
- Also test Room 2 behaviour with Macalvin's code and report bugs to him.
- Done when: the functions print correct information for changing state.

### Ariko Joseph Ojangole: `src/storage.c` and `src/cli.c`
`storage_task` (storage.c):
- Mount the SD card over SPI (pins in `pins.h`), FAT filesystem, e.g. at `/sdcard`.
- Receive from `g_log_queue`; append CSV lines (timestamp, type, source, uid, note) to a log file. Take `g_spi_mutex` around SD access.
- Handle a missing/failed card gracefully (keep running, print a warning).

`cli_task` (cli.c): read lines from the serial console and run commands:
- `help`, `status` (calls `report_print_summary`), `inside` (calls `report_print_inside`), `log [n]` (last n records from SD).
- `scan entry <uid>` / `scan exit <uid>`: push a `scan_msg_t` onto `g_scan_queue[door]` (simulated RFID).
- `time set <epoch>` (use `clock_set`), `time` (show).
- `schedule show`; `schedule set HH:MM HH:MM` calls `schedule_set()`, **only after admin login**.
- `login <pin>`, `logout`. **Innovation:** lock out for 30 s after 3 failed logins. Log `EVT_ADMIN_LOGIN_OK/FAIL`.
- Done when: events appear in the SD log, and the schedule can be changed only when logged in.

### Ggubya Gerald: integration and report
`main.c`, `shared.h`, `pins.h`, repo, merging, integration test, report compilation, submission.

## Integration test (1 Nov)
1. Boot: log shows all tasks started, time 08:00.
2. `scan entry A1B2C3D4`: door opens, count 1. Repeat: denied (anti-passback).
3. Motion in Room 1: light on; wait: light off.
4. `schedule set 09:00 17:00` without login: refused. After `login`: accepted.
5. With time outside the window: entry and exit denied.
6. `status` and `log 10` show correct data.
