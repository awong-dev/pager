# Carrier v0.2 firmware: board variants and the new peripherals

Status: design, 6 Nov 2026. Owner request: "firmware updated to allow setting
the carrier (autodetect if not set by checking the accelerometer id from
I2C), haptic activation, piezo buzz on new message, front-light control from
the button (on until idle), a small LED pulse over time, a button that plays
the first 22 notes of the SMB3 World 1 map theme; correct battery reading
from the fuel gauge (percentage + charging indicator), checked on every
periodic wake, safe shutdown on low battery with the screen saying
'Shutdown'."

Naming: the firmware already has `carrier.c` = the *cellular* carrier (APN).
The PCB variant is called the **board** everywhere in code: `board.c/h`,
NVS namespace `board`, console command `board`.

## 1. The two boards

| | `BOARD_BREADBOARD` (Adafruit breakouts, every pager built before Nov 2026) | `BOARD_CARRIER_V02` (hardware/carrier/pager-carrier.kicad_pcb) |
|---|---|---|
| Accelerometer | Adafruit LIS3DH, WHO_AM_I **0x33**, I2C_NUM_1 @0x18 | MSKSEMI LIS2DW12TR-MS (SC7A20-class clone, LIS3DH-style registers), WHO_AM_I **0x11**, same bus, same address, INT1 on the same IO16. Needs SDO_PU=1 in reg 0x57 (memory: carrier-economic-pcba-swaps). |
| Fuel gauge | none (battery mV from the modem, battstat.c) | MAX17048 @0x36 on the always-on bus I2C_NUM_1 (AON_SDA IO17 / AON_SCL IO18). ALRT not connected: poll. |
| Charger | Adafruit 6092 (no status pins) | BQ25185: STAT1 -> IO38 (open-drain, 10k pull-up), STAT2 -> IO42. VBUS_DET -> IO15 (divider 56k/100k from VBUS: high = USB present). |
| Display power | IO12 = Friend ENA (active-high) | none: panel 3V3 is the always-on rail (R11 0R). **IO12 is FL_PWM here** (see below). |
| Display SRCS (IO38) | held high (Friend SRAM CS) | **must be an input** (it is CHG_STAT1). Driving it high fights the charger's open-drain pull-down while charging. |
| Display MISO (IO42) | SPI MISO (unused SRAM) | CHG_STAT2: leave MISO unassigned (-1) and read IO42 as a plain input. |
| 3V3_AWK rail gate | IO0 active-low (CardKB only) | IO0 active-low (AWK_EN_N via R39): CardKB **and the DRV2605L**. |
| Keyboard | CardKB @0x5F, I2C_NUM_0 SDA IO10 / SCL IO9 | same (J2, Qwiic order) |
| Haptic | none | DRV2605L @0x5A on **I2C_NUM_0** (the CardKB bus), VDD = 3V3_AWK (gated: only reachable while the rail is on). EN tied high, IN/TRIG grounded. LRA on J3 (Vybronics VG0825001U, 8 mm coin; auto-cal). |
| Piezo | none | passive CEB-20D64 on J5, driven differentially: PIEZ_P IO6, PIEZ_N IO7 through 220R each, 1M bleed across. Idle: both low. |
| Front light | none | FL_PWM IO12 -> R9 1k -> Q2 SI1308EDL low side with a MMST3904 + 15R current clamp (~40 mA): **high = on**, PWM for brightness. Panel J5 via J8. |
| Front-light button | none | SW1 (KMR221GLFS) FL_BTN IO4 to GND, external 100k pull-up + 100nF: **active-low**, RTC-capable pad. |
| External LED | none | EXT_LED IO8 -> R21 330R -> J4 anode, cathode GND: high = on, ~4 mA red. LEDC fade capable. |
| UART0 | unused | ESP_RX0/TX0 (IO44/IO43) -> CP2102N-A02 with DTR/RTS auto-reset -> the carrier's USB-C (J6). Shows up as `/dev/cu.usbserial-*`. The Walter's own USB-C (USB-JTAG) is not reachable once assembled. |

Walter header pin -> net, carrier v0.2 (from the PCB pads, 6 Nov 2026):
4 IO0 AWK_EN_N · 5 IO12 FL_PWM · 6 IO11 EINK_BUSY · 7 IO13 EINK_RST ·
8 IO38 CHG_STAT1 · 9 IO39 EINK_DC · 10 IO40 EINK_CS · 11 IO41 EINK_MOSI ·
12 IO42 CHG_STAT2 · 13 IO2 EINK_SCK · 14 IO1 TP · 15 IO4 FL_BTN · 16 IO5 TP ·
17 IO6 PIEZ_P · 18 IO7 PIEZ_N · 19 IO15 VBUS_DET · 20 IO16 ACC_INT1 ·
21 IO17 AON_SDA · 22 IO18 AON_SCL · 23 IO8 EXT_LED · 24 IO9 AWK_SCL ·
25 IO10 AWK_SDA · 26 3V3_AWK · 27 GND · 28 SYS_SW.

## 2. Board selection (`board.c`)

- `board_kind_t { BOARD_AUTO = 0, BOARD_BREADBOARD = 1, BOARD_CARRIER_V02 = 2 }`.
- NVS namespace `board`: `kind` (u8, absent/0 = auto) and `last` (u8, the
  last successful auto-detection). Survives "Set up again" like `carrier`.
- `board_init()` runs **before anything configures IO12/IO38/IO42** (i.e.
  before disp/ui/rail init). It reads the accel WHO_AM_I over I2C_NUM_1
  (accel.c exposes `accel_probe_id()` which installs the bus idempotently):
  0x33 -> breadboard, 0x11 -> carrier. If `kind` is set it wins; otherwise
  the probe result is used and written to `last`; if the probe fails, `last`
  is used; if there is no `last`, breadboard (nothing is driven against the
  charger pins only if the SRCS high is avoided, so on failure with no
  history the breadboard default is accepted and logged loudly).
- `board_get()`, `board_name()`, `board_is_carrier()`; and per-pin helpers
  that the existing code switches on: `board_disp_vcc_en_pin()` (-1 on the
  carrier), `board_disp_srcs_pin()` (-1 on the carrier), `board_disp_miso_pin()`
  (-1 on the carrier), `board_has_haptic()/has_piezo()/has_frontlight()/
  has_ext_led()/has_fuel_gauge()`.
- Console `board` (prints kind, source auto/fixed/last, raw WHO_AM_I,
  charger pins, VBUS), `board auto|breadboard|carrier` (persist; reboot to
  apply). Device info screen gets a "Board" line.
- accel.c accepts WHO_AM_I 0x11 as well as 0x33 and, for 0x11, writes
  SDO_PU (reg 0x57 bit 7 = 0x80) before the normal init sequence.

## 3. Peripheral drivers (all carrier-only; on the breadboard every call is a no-op)

### 3.1 Piezo (`buzzer.c`)
- LEDC timer (LEDC_LOW_SPEED_MODE, 10-bit) with two channels on IO6 and IO7,
  50 % duty, channel 2 with `flags.output_invert = 1` so the piezo sees the
  full 6.6 V p-p differential swing. Silence = both channels stopped and
  pins driven low (no DC across the piezo).
- A note sequencer run by an esp_timer one-shot chain (no task): `buzzer_play(const buzzer_note_t *seq, size_t n)` with `{uint16_t hz; uint16_t ms;}` and hz 0 = rest. `buzzer_stop()`. A new play replaces the current one.
- Built-in sequences: `BUZZ_NEW_MSG` (two short notes, e.g. E6 60 ms, G6 90 ms), `BUZZ_MARIO` (section 5).
- Console: `buzz <hz> <ms>`, `buzz msg`, `buzz mario [n]`, `buzz off`.

### 3.2 Haptic (`haptic.c`)
- DRV2605L @0x5A on I2C_NUM_0, used only while rail is on (rail.c); every
  call checks `rail_is_on()` and the bus lock ui.c uses for the CardKB.
- `haptic_init()` (after rail_on, lazily on first use): exit standby, LRA
  mode (FEEDBACK_CONTROL 0x1A bit7 = 1), library 6 (LRA), RATED_VOLTAGE 0x16 /
  OD_CLAMP 0x17 computed with the datasheet §8.5.2.1 formulas from
  NVS-tunable `rated_mv` (default 1200 mVrms), `clamp_mv` (default 1800 mV
  peak) and `f_hz` (default 235 Hz, the VG0825001U resonance); DRIVE_TIME
  from f. Then auto-calibration (MODE 0x07, GO, wait for GO to clear, check
  DIAG_RESULT bit3 of STATUS) once; A_CAL_COMP/A_CAL_BEMF/BEMF_GAIN are saved
  to NVS `haptic` and restored on later boots instead of re-calibrating.
  `haptic calibrate` on the console forces a redo and prints the result and
  LRA_RESONANCE_PERIOD (0x22).
- `haptic_play(effect)` loads WAV_SEQ1 = effect, WAV_SEQ2 = 0, sets GO.
  Patterns: `HAPTIC_NEW_MSG` = effect 1 (strong click 100 %) twice (seq 1,
  pause 0x8A... simpler: 1, 0, done; then repeat after 150 ms), `HAPTIC_TICK`
  = effect 24 (sharp tick). Standby between plays (MODE bit6) to save power.
- Console: `haptic <effect>`, `haptic msg`, `haptic calibrate`, `haptic cfg rated_mv clamp_mv f_hz`, `haptic status`.

### 3.3 Front light (`frontlight.c`)
- LEDC channel on IO12, 2 kHz, 10-bit; brightness NVS `board/fl_duty`
  (default 60 %). Off = duty 0 and pin low.
- Button IO4: plain input (the external 100k pull-up is enough; the internal
  one would add ~70 uA while held), active-low, polled with a 30 ms debounce
  inside `input_poll` on the modes task -> its own event `INPUT_EVT_LIGHT`.
  No GPIO ISR (gpio_wakeup_enable rewrites the pin's interrupt type to
  LOW_LEVEL, so an ISR would storm while the button is held). The event only
  arms the awake/attentive window and toggles the light; it never calls the
  retired IO8 button FSM (whose BTN_DOWN costs a mode-change /status and
  whose BTN_SHORT opens passcode entry).
- Behaviour: press toggles the light; while on, any key press or button
  press restarts the idle timer; the light turns off after
  `FRONTLIGHT_IDLE_S` = 20 s of no input, and always before light sleep or
  rail_off (never light the panel while the pager sleeps).
- In light sleep the button is a wake source (`gpio_wakeup_enable(IO4,
  GPIO_INTR_LOW_LEVEL)` + `esp_sleep_enable_gpio_wakeup()`; ext1 stays
  ANY_HIGH for the accel INT1 and cannot take an active-low pin). Armed
  only if IO4 reads high at arm time (a held button would otherwise re-wake
  at once, forever, at ~40 mA); `gpio_wakeup_disable(IO4)` after every wake.
  IDF ranks wake causes TIMER > GPIO > EXT1, so "wake is input" in modes.c is
  `cause == EXT1 || cause == GPIO || IO4 reads low`; such a wake skips the
  probe wait and renders first, like a shake. A button wake turns the light
  on and opens the attentive window. UART0 TX (IO43) is exempted from sleep
  isolation so the CP2102N never sees a floating input.

### 3.4 External LED (`extled.c`)
- LEDC channel on IO8 with hardware fades (`ledc_fade_func_install`): one
  "breath" = fade 0 -> 25 % in 250 ms, back to 0 in 400 ms, every 6 s while
  the chip is awake (esp_timer periodic). Nothing runs in light sleep (the
  LED is dark while the pager sleeps; that is the power budget). A second,
  faster pattern (double breath) while there are unread messages.
- Console: `led on|off|pulse|breath`.

## 4. Battery (`fuel.c`, carrier only)
- MAX17048 @0x36 on I2C_NUM_1 (shared with the accel; accel.c gets a bus
  mutex `accel_bus_lock()/unlock()` that both use). Registers: VCELL 0x02
  (78.125 uV/LSB), SOC 0x04 (1/256 %/LSB), CRATE 0x16 (0.208 %/h, signed),
  VERSION 0x08 (0x001x), STATUS 0x1A, MODE 0x06 (quick-start bit 14).
- `fuel_read(fuel_t *out)`: mV, percent (0-100, rounded), crate, ok flag.
  Charging indicator: VBUS_DET IO15 high = plugged in; CHG_STAT1 IO38 low =
  charging; STAT1 high with VBUS = charged/float. Three states:
  `FUEL_ON_BATTERY`, `FUEL_CHARGING`, `FUEL_CHARGED`.
- battstat.c on the carrier takes mV and percent from the gauge instead of
  the modem (AT+SQNVMON stays for the breadboard). /status keeps its
  existing battery key (mV) and gains percent + charging where the protocol
  allows (see PROTOCOL.md; add keys only if a free slot exists, otherwise
  leave /status alone and show it locally).
- UI: the status line shows `NN%` and a charging glyph (a small plug/bolt
  from gfx) when charging; Device info gets "Battery: 3.87 V 72 % charging".
- Low-battery shutdown (rule from the architect review, 6 Nov 2026: decide
  on **voltage**, show percent only; the MAX17048's default model is least
  accurate near empty and is only an OCV guess right after power-on):
  the gauge is read on **every periodic (timer) wake**, never on an input
  wake, inside the timer-only branch right before the probe wait, so it
  never delays a first render; rate-limited to one read per 10 s.
  Shut down when VCELL <= `FUEL_SHUTDOWN_MV` (3450) on two consecutive fresh
  reads **and** not plugged in. At boot, before `net_init()` (the radio must
  never start), shut down again when VCELL <= `FUEL_BOOT_SHUTDOWN_MV`
  (3550) and not plugged in; the 100 mV gap covers the cell's recovery after
  the load drops. A failed read never counts. Sequence
  (`modes_power_off_lowbatt`, modes.c):
  1. front light, ext LED, buzzer off (haptic dies with the rail in 4),
  2. modem to MINIMUM functionality (best effort, 2 s cap), then reset held
     low via `net_airplane_hold_modem()` (which now releases the pad hold
     the modem library left on IO45 before writing it, a latent bug in the
     airplane path too). No MQTT teardown, no /status publish, no CFUN=4:
     no radio time at empty-cell voltage,
  3. draw "Shutdown" / "Charge me", full refresh, wait for BUSY, then the
     SSD1680 deep-sleep command (0x10, 0x01; the panel rail is always on),
  4. `rail_off()` + hold IO0,
  5. if VBUS_DET is high now (USB arrived during the draw): `esp_restart()`,
  6. disable all wake sources, `esp_sleep_pd_config(RTC_PERIPH, OFF)` (GPIO
     wake had forced it ON for the rest of the boot), ext1 ANY_HIGH on IO15
     only, `gpio_deep_sleep_hold_en()`, `esp_deep_sleep_start()`.
  Plugging in USB reboots the pager. `fuel_init` releases the RTC hold that
  ext1 latches on IO15 (`rtc_gpio_hold_dis` + `rtc_gpio_deinit`) before
  reading it, otherwise VBUS_DET reads "plugged in" forever after that wake.
  Over USB on the bench IO15 is already high, so `fuel shutdown` draws the
  screen and reboots: that proves the ext1 path. The real path needs a
  bench supply on BAT with USB unplugged.
- Breadboard: unchanged (no gauge, no shutdown).

## 5. The tune (`tunes.c`, pure, host-tested)
Super Mario Bros. 3, World 1 map ("Grass Land"), lead line, from two
independent MIDI transcriptions on vgmusic.com (`mario3-world1.mid`,
`da_smb3-world1.mid`) that agree note for note: 200 bpm, every note a
staccato quarter (72 ms on, next onset 300 ms later) except the swing figure
where the onsets are 225 / 300 / 75 ms apart.

Onsets in 1/4-beat units (one unit = 75 ms at 200 bpm), note, duration on:
```
 1 G4   0      2 G4   4      3 A4   8      4 A4  12      5 B4  16
 6 B4  20      7 A4  23      8 G#4 27      9 A4  28     10 G4  32
11 G4  36     12 A4  40     13 A4  44     14 B4  47     (rest to 64)
15 C5  64     16 C5  68     17 D5  72     18 D5  76     19 E5  80
20 E5  84     21 D5  87     22 C#5 91     23 D5  92     24 C5  96
25 C5 100     26 D5 104     27 D5 108     28 E5 111     (rest to 128)
```
The musical phrases are 1-14 and 15-28. The owner asked for "the first 22
notes (one phrase)"; `TUNE_MARIO_DEFAULT_NOTES` = 22 as asked, with the full
28 in the table and the count as a parameter (`buzz mario 28`), because note
22 is the C#5 passing tone and stopping there sounds cut off. Flag this to
the owner. Frequencies: G4 392, G#4 415, A4 440, B4 494, C5 523, C#5 554,
D5 587, E5 659. Each note is played as 72 ms on then rest until the next
onset. UI: Device menu entry "Play tune" (carrier only) and the console.

## 6. Console on the carrier
The ESP-IDF REPL is compile-time bound to one device
(`esp_console_new_repl_uart` only exists with
CONFIG_ESP_CONSOLE_UART_DEFAULT; the USB-JTAG one only with
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG). So the carrier gets a second build
config, `firmware/sdkconfig.carrier` (UART0 primary at 115200, USB-JTAG as
the secondary output-only console), built into `build-carrier/`:
```
idf.py -B build-carrier -DSDKCONFIG=build-carrier/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.carrier" build
```
main.c picks the REPL constructor by `#if`. The breadboard build is
untouched. The OTA bucket will need a per-board image later; not in scope.
