# Carrier v0.2 firmware tasks

Design: docs/CARRIER_V02_FIRMWARE_DESIGN.md (read it first, all of it).
Branch `carrier-fw`. Build recipe: memory/idf-build-python-env (export
IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.2_py3.11_env, then
`source ~/src/esp/esp-idf/export.sh`), log output to the scratchpad. Build
both images: `idf.py build` (breadboard, `build/`) and
`idf.py -B build-carrier -DSDKCONFIG=build-carrier/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.carrier" build`.
Host tests: `make -C firmware/host test`. Every task ends with both builds
green and the host tests green. Debug console additions go in
`start_normal_console()` (main.c, under PAGER_DEBUG_NO_LIGHT_SLEEP) unless
the task says "also Setup console".

Code rules: never hardcode a pin outside pins.h. Every new module has a
header comment saying what it is, which board it exists on, and its power
effect. On `BOARD_BREADBOARD` every new peripheral call is a cheap no-op.
Don't add "defensive" code you have not seen a need for (memory:
verified-changes-only).

## Task A: board variant foundation (do first; everything else depends on it)

Read: main/pins.h, main/rail.c (all), main/disp.c:1190-1260 and :570-580,
main/accel.c:120-130, :200-300, :360-420, :600-620, main/main.c:2185-2330
(app_main), :761-800 and :967-1025 (carrier cmd + registration),
main/scr_devinfo.c, main/devinfo_fmt.c/.h, host/test_devinfo.c,
host/Makefile:70-90 and :535-545, main/CMakeLists.txt, sdkconfig.defaults.

Files: new main/board.c, main/board.h; edit pins.h, rail.c, disp.c, accel.c,
accel.h, main.c, devinfo_fmt.c/.h, scr_devinfo.c, host/test_devinfo.c,
host/Makefile, main/CMakeLists.txt; new firmware/sdkconfig.carrier.

Do:
1. pins.h: add the carrier-only pins with comments from the design table:
   `PAGER_PIN_FL_PWM 12`, `PAGER_PIN_FL_BTN 4`, `PAGER_PIN_PIEZ_P 6`,
   `PAGER_PIN_PIEZ_N 7`, `PAGER_PIN_EXT_LED 8`, `PAGER_PIN_VBUS_DET 15`,
   `PAGER_PIN_CHG_STAT1 38`, `PAGER_PIN_CHG_STAT2 42`,
   `PAGER_I2C_ADDR_DRV2605 0x5A`, `PAGER_I2C_ADDR_MAX17048 0x36`. Add a
   dated header paragraph (6 Nov 2026) saying the carrier v0.2 reuses IO12,
   IO38, IO42, IO8 for other functions and that board.c decides at runtime.
2. accel.c/h: make `accel_bus_init()` idempotent (static installed flag,
   check the install return value) and create the mutex once. Export
   `bool accel_probe_id(uint8_t *who)` (installs the bus if needed, reads
   WHO_AM_I under the mutex, no configuration) and
   `void accel_bus_lock(void)/accel_bus_unlock(void)` for the fuel gauge.
   Accept WHO_AM_I 0x11 as well as 0x33 everywhere it is compared (both
   compares: probe_and_configure and accel_debug_status); for 0x11 write
   reg 0x57 = 0x80 (SDO_PU) first in configure_and_arm. Log which part was
   found.
3. board.c/h per design §2. `board_init()` needs NVS and I2C: in app_main
   move `nvs_flash_init()` (with its erase/retry) ahead of `rail_init()`,
   and call `board_init()` between them. rail_init() must then skip IO12
   entirely on the carrier (`board_disp_vcc_en_pin() < 0`): no gpio_config,
   no hold undo, no on/off; `rail_disp_bus_release/restore` and
   `s_disp_bus_pins[]` must not include IO38 on the carrier; disp.c must not
   configure or drive IO38 and must pass miso_io_num = -1 on the carrier;
   `disp_power_on` and modes.c:1171 `gpio_hold_en(DISP_VCC_EN)` are skipped
   on the carrier. Keep every breadboard path byte-for-byte the same.
4. Console `board` command (design §2), registered in both the Setup console
   and the normal console (like `carrier`). The status print shows: kind,
   source (fixed/auto/last/default), WHO_AM_I, and on the carrier the raw
   levels of IO15/IO38/IO42.
5. Device info: add a "Board" row (devinfo_fmt + scr_devinfo + host test).
6. sdkconfig.carrier: `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`,
   `CONFIG_ESP_CONSOLE_UART_NUM=0`, `CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200`,
   `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, and unset
   `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`. In main.c both REPL creations
   (:1014 and :1870) become `#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG ... #else
   esp_console_new_repl_uart(...) #endif`. Add a "Build" note to
   firmware/README.md with the two build commands and why.
7. firmware/README.md "Hardware": add a short "Carrier v0.2" subsection with
   the pin table from the design doc and a pointer to the design doc.

Verify: both builds green; host tests green; `grep -n "PAGER_PIN_DISP_VCC_EN\|PAGER_PIN_DISP_SRCS" main/*.c` shows every use behind a board check or inside a breadboard-only branch. Report the exact app_main order after the change.

## Task B: piezo, tune, haptic, front light, external LED (after A)

Read: design §3 and §5; main/board.h; main/input.c (all: the retired button
path, the event queue, `input_feed_key`, BTN_SHORT/BTN_LONG), main/modes.c:
1860-1960 (`modes_alert_incoming`), :1312-1350 (render pending), :2560-2620
(attentive_service), :2900-3060 (sleep entry and wake handling), :3220-3260
(event drain), main/net.cpp:1342-1470 (`net_sleep`), main/ui.c:820-1000 (kb
bus mutex, park/unpark), main/rail.c:150-215, main/scr_device.c:81-95,
:254-350, :431-520, host/Makefile.

Files: new main/buzzer.c/h, main/tunes.c/h (pure: note table + the 28-note
Mario table + a function that expands the first n notes into
buzzer_note_t on/rest pairs; host test host/test_tunes.c checking the
first 22 and 28 expand to the right onsets in ms), main/haptic.c/h,
main/frontlight.c/h, main/extled.c/h; edit input.c, modes.c, net.cpp,
ui.c (export the kb bus lock as `ui_kb_bus_lock/unlock` with a timeout),
scr_device.c, main.c (console: `buzz`, `haptic`, `light`, `led`), CMakeLists,
host/Makefile.

Do:
1. buzzer.c per §3.1 (LEDC, two channels, inverted second channel,
   esp_timer one-shot chain). `buzzer_play` from any task; the timer
   callback only touches LEDC. Idle pins low.
2. tunes.c per §5. Default count 22 (`TUNE_MARIO_DEFAULT_NOTES`).
3. haptic.c per §3.2. Rail-gated: `haptic_play` returns false when the rail
   is off. First use after each rail_on re-checks the chip (it lost power):
   write the config registers again (cheap), load the saved calibration
   from NVS `haptic` if present, otherwise run auto-cal once and save.
4. New-message alert: in `modes_alert_incoming` set a pending flag; the
   modes loop services it right where `service_render_pending` runs:
   `ui_ensure_powered()`, `buzzer_play(BUZZ_NEW_MSG)`, `haptic_play(HAPTIC_NEW_MSG)`.
   It fires whether or not the device is locked.
5. frontlight.c per §3.3. The button goes through input.c's existing
   button path re-pointed at IO4 active-low, enabled at runtime when
   `board_has_frontlight()` (the compile-time PAGER_WAKE_BUTTON_ENABLED
   stays 0 and its IO8 ext1 arming stays dead). BTN_SHORT toggles the light
   and counts as input for the attentive window (it already resets
   `s_last_input_us`). Idle-off 20 s via a one-shot esp_timer restarted by
   every key and button event (hook in `input_feed_key` and the BTN path).
   Light off before `esp_light_sleep_start` and in `rail_off`.
   Light-sleep wake: in `net_sleep`, on the carrier, `gpio_wakeup_enable(IO4,
   GPIO_INTR_LOW_LEVEL)` + `esp_sleep_enable_gpio_wakeup()`; after wake,
   `ESP_SLEEP_WAKEUP_GPIO` is treated like a shake wake (rail on, hot
   window) plus the light on and a BTN_SHORT posted so the attentive window
   opens. Make sure IO4 does not re-trigger wake forever while held (disable
   the GPIO wake source right after wake; re-arm on the next sleep).
6. extled.c per §3.4 (LEDC fade, esp_timer periodic 6 s while awake; stop
   the timer before light sleep and start it after wake; a double breath
   when `msg` reports unread). Keep it in one place: `extled_set_unread(bool)`
   called from wherever the UI learns the unread count.
7. Device menu: row "Play tune" (carrier only; omit the row on the
   breadboard) -> `buzzer_play` of the Mario default. Console `buzz`,
   `haptic`, `light on|off|<duty%>`, `led`.

Verify: both builds + host tests green. On the breadboard build nothing new
runs at boot (grep that every init is behind board_has_*). Describe in the
report exactly which ISR/task each new piece runs on.

## Task C: fuel gauge, charging indicator, low-battery shutdown (after A, parallel with B)

Read: design §4; main/board.h, main/accel.h (lock API from A),
main/battstat.c/h, main/modes.c:640-720 (status keys, refresh_batt_mv),
:800-810, :941-1000 (build_status_cbor battery part), :1146-1180
(modes_accel_halt: the deep-sleep template), :2364-2550 (modes_boot),
:2640-2700 (wake interval), :2940-3060 (after net_sleep), main/ui.c:260-380
(status bar battery), main/scr_device.c:440-450, main/scr_devinfo.c,
main/devinfo_fmt.c, main/gfx.h (what glyph/primitive helpers exist),
main/net.cpp (net_airplane_hold_modem, net_radio_off, net_session_down).

Files: new main/fuel.c/h (device) + the pure part `fuel_fmt.c/h` (percent
rounding, state naming, shutdown decision `fuel_should_shutdown(pct, plugged,
booting)` with hysteresis; host test host/test_fuel.c); edit modes.c,
ui.c, scr_device.c, scr_devinfo.c, devinfo_fmt.c, battstat.c if needed,
main.c (console `fuel`), CMakeLists, host/Makefile.

Do:
1. fuel.c per §4: `fuel_init()` (verify VERSION reads 0x001x, log), `fuel_read()`
   under `accel_bus_lock`, `fuel_state()` from IO15/IO38 (configure IO15,
   IO38, IO42 as plain inputs, no pulls, in fuel_init on the carrier).
2. modes.c `refresh_batt_mv()`: on the carrier read the gauge instead of
   the modem (mV into the same `s_last_batt_mv`, so /status key 23 and
   every existing consumer get the right value), plus `s_last_pct`,
   `s_last_charge_state` with accessors `modes_get_batt_pct()`,
   `modes_get_charge_state()`. Keep the existing call cadence; add a call at
   the top of the loop body after each wake if `refresh_batt_mv` is not
   already on that path (one gauge read per wake, never more than one per
   10 s).
3. Shutdown: factor the body of `modes_accel_halt` into
   `modes_power_off(const char *title, const char *line2, uint64_t ext1_mask)`
   and have accel halt call it with its current text and mask 0. The
   low-battery path calls it with "Shutdown" / "Charge me" and mask =
   1<<IO15 (ANY_HIGH) so plugging in USB reboots the pager; skip the
   best-effort /status publish on low battery (no radio time). Decision
   point: right after each wake's gauge read and at the end of modes_boot
   (boot with pct <= 7 and unplugged -> shutdown again). Thresholds in
   fuel_fmt.h: `FUEL_SHUTDOWN_PCT 5`, `FUEL_BOOT_SHUTDOWN_PCT 7`. Never on
   the breadboard, never while plugged in, never if the gauge read failed.
4. UI: status bar shows `NN%` text next to the battery icon on the carrier
   (icon segments from percent: >75/50/25/10) and a small charging mark
   (a 5x7 bolt drawn with gfx primitives, or the existing font's
   character if one fits) when charging; "charged" shows the full icon
   with the mark. Device menu "batt" line: `3.87 V 72% charging`. Device
   info row the same.
5. Console `fuel`: prints mV, pct, crate, state, raw IO levels; `fuel quick`
   issues a quick-start; `fuel shutdown` runs the shutdown path on purpose
   (debug only) so the bench can see the "Shutdown" screen.

Verify: both builds + host tests green; report where the per-wake read sits
in modes_run and prove (by reading the code) it cannot run while a render
is in progress on another task.

## Task D: bench (after A+B+C merge; bench-tester)

Carrier pager on /dev/cu.usbserial-* (CP2102N, auto-reset: plain esptool
`write_flash`, no reset button). Flash the carrier **debug** build. Capture
boot: expect "board: carrier v0.2 (auto, WHO_AM_I 0x11)", the accel found
line, fuel VERSION line. Then on the console: `board`, `fuel`, `buzz 1000
200`, `buzz mario`, `buzz mario 28`, `haptic calibrate` (within the
attentive window: press a key first), `haptic msg`, `light on`, `light
off`, press SW1 (light toggles; stays on 20 s after the last key), `led
breath`, plug USB in/out and re-run `fuel` (state changes), `fuel shutdown`
(screen shows "Shutdown"; plugging in USB reboots). Send a test page and
confirm buzz + haptic + render. Also flash the breadboard debug build on a
breadboard pager if one is on the bench and confirm "board: breadboard
(auto, WHO_AM_I 0x33)" and no behaviour change. Short captures only
(memory: bench-short-captures).
