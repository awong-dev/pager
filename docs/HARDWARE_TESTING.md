# Building, flashing and testing on a real pager

See `firmware/README.md` for the hardware and the full measurement list, and `GOTCHAS.md` before
your first flash.

## Build

```
export PATH=<dir with python3 -> python3.11>:$PATH      # see GOTCHAS.md
. ~/src/esp/esp-idf/export.sh
cd firmware
idf.py build                                           # normal build: light-sleeps
PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build  # debug build
make -C host test                                      # host unit tests, no hardware
```

The **debug build** never light-sleeps, so the USB log stays alive and the pager stays flashable;
it prints the raw AT trace and runs the console on a provisioned pager. It draws full awake
current. Never ship it.

## Flash

```
idf.py -p /dev/cu.usbmodem101 app-flash                       # application only
esptool.py --chip esp32s3 write_flash 0x11000 assets.bin      # fonts (tools/mkassets.py)
esptool.py --chip esp32s3 erase_region 0x9000 0x6000          # wipe settings -> Setup mode
```

A release build in light sleep has no USB port, so nothing can be flashed until the owner resets the
board (hold BOOT, tap RESET, or power-cycle). `ls /dev/cu.usbmodem*` coming back empty means ask the
owner, not poll the port (8 Oct 2026).

## Console commands

Setup mode has `setup`, `carrier` and `bearer`; on the debug build it also has `nettest`, `mqtttest`
and `at`. Outside Setup mode the debug build has all of these.

| Command | Does |
|---|---|
| `setup <code>` | Provision from a setup code |
| `carrier` / `carrier <n>` / `carrier custom <apn>` | Show or set the APN choice. 0 = automatic. Prints `sim: imsi=… gid1=… iccid=…`, `bearer:`, `pdp auth:` and `detected:` |
| `bearer [auto\|direct\|beam]` | Override the bearer (NVS `carrier/bearer`), read at boot. `beam` is honoured only when the preset in force is a Beam carrier (Soracom). `auto` clears the override |
| `nettest <host> <port> [udp\|tls\|<bytes>]` | Debug build only. Socket-layer probe. With a byte count: a padded HTTP GET, then waits for a reply. Re-attaches first, which disturbs a live session |
| `mqtttest <host> <port> [ca\|noneca\|emptyca\|plain]` | Debug build only. Points the modem's own MQTT client at any host. `emptyca` deletes the certificate in slot 12 first. `plain` uses TLS profile 0, the Beam shape (plaintext). Other modes are not meaningful while the bearer is `beam` |
| `cafetch <https url> <sha256 hex>` | The CA fetch over a second TLS socket, without applying it. Reports whether the MQTT session survived |
| `gnsstest <seconds>` | One location attempt, bypassing the backoff and the battery floor |
| `smstest <number> <text>`, `smslist` | Send one SMS bypassing the allow-list; show the list and the audit queue |
| `sleeptest <minutes> [yield_ms] [interval_ms]` | Light-sleep window with optional overrides; records pages, session loss, timing; saves to NVS then resets. `sleeptest` alone prints the saved report. `yield_ms` is `0` or 30..10000, `interval_ms` is `0` or 200..60000; `0` (or omitting the argument) keeps the build default, so `sleeptest 6 0 0` and `sleeptest 6` are the same run |
| `coverage` | Debug the no-coverage radio duty cycle (deregister with `at AT+COPS=2`, recover with `at AT+COPS=0`) |
| `at <command>` | One raw AT command; the reply shows in the trace. Accepts `at +CIMI` or `at AT+CIMI`. Setup mode has it on the debug build only |
| `acceltest [samples <n>\|ths <0-127>\|dur <0-127>\|refr <seconds>]` | LIS3DH register/sample dump and runtime tuning (A2); re-probes WHO_AM_I every call, so wiring the chip needs no reboot. `acceltest` alone prints registers, THS in mg, the refractory setting, edges reported and ext1 wake count |
| `flip on\|off\|status` | Persist and apply a 180-degree display rotation (NVS `disp_flip`) so the pager can be read upside down; `on`/`off` force one full refresh immediately |

Run modem commands only after `MQTT session usable` has appeared. Before that they collide with
the pager's own attach. Never send a slow raw command (`AT+COPS=0`, `AT+CFUN`) while the
coverage duty-cycle is in a search window or the modem is registering: the library runs one
command at a time, an unanswered one holds the queue for up to 90 s, and the main loop's next
command waits behind it. Before vendor patch 1.10 that tripped the 60 s task watchdog and
rebooted the pager; it still stalls the loop.

## Bench tools

Send test pages with `relay/.venv/bin/python tools/bench/send_test_page.py test-pager "<text>"`.

`tools/bench/serial_capture.py` is a serial reader that survives the USB port vanishing and
reappearing (it renames when the ESP32 resets).

### disptest — display refresh test harness

Debug build console command. No keyboard or network needed; do not use `wake` or `key` while it
runs — the UI render task would repaint over the test pattern. Commands:

| Command | Does |
|---|---|
| `disptest` or `disptest info` | Print refresh mode, partial count, and dirty rows. Run again to re-read. |
| `disptest again <0\|1>` | `0` deliberately reproduces the pre-fix two-plane bug; `1` is the corrected code. |
| `disptest lut <0\|1>` | A/B for the Orient AES128296A00-2.9ENRS "no partial waveform in OTP" hypothesis (5 Oct 2026). `0` = the OTP partial waveform, the partial command stream before 7 Oct 2026 (0x22=0xFF); it bleached the panel on the glass, so it is for A/B only. `1` = also sends 0x32 + the 153-byte `WF_PARTIAL_2IN9` LUT (`firmware/main/wf_partial_2in9.h`, copied from `docs/reference/wf_partial_2in9.h`) before 0x20, and 0x22=0xCF instead of 0xFF. `1` is the default (NVS `disp`/`lut`, absent = 1, so the setting persists). The per-partial log line says which mode ran. |
| `disptest tp0a [N]` | Show or set the host LUT's TP0A frame count (NVS `disp`/`tp0a`, default 7 since 7 Oct 2026, down from the reference table's 10, to win back keystroke latency). Raise it (8 or 9) if changed pixels look faint. |
| `disptest bars` | Paint 8-pixel wide full-height stripes (black / white / black / ...) with a FULL refresh. Establishes a baseline pattern. |
| `disptest step <n>` | Invert the 8-pixel screen column at x=n*8, trigger ONE partial refresh, wait for BUSY. |
| `disptest seq [n0] [n1] [ms]` | Step through columns n0 to n1 inclusive, ms apart (defaults: 2 12 1500). Watch for band flipping; verify pattern matches prediction. |
| `disptest full` | Force one full refresh of the framebuffer. |
| `disptest swreset` | Fault injector: send the SSD1680's SW reset (0x12) alone, wait BUSY, nothing else — leaves the controller on power-on register defaults, exactly like the 23 Sep field failure. |

**Pattern reading:** After `disptest bars`, every 8-pixel column is either solid black or solid
white. `disptest seq` inverts each column in turn, producing a predictable band-flip sequence
that can be read off as a pattern (e.g. `w b w b w b w b ...`) and compared against the expected
sequence. The leftmost columns (before the starting column of the `seq` range) never change and
appear wrong if the reading is off by one.

**23 Sep register-loss regression, acceptance sequence:** `disptest bars` (clean) -> `disptest
swreset` -> `disptest bars` again. Before the fix (both refresh paths re-arm the SSD1680's
registers before every RAM write, disp.c's `disp_pre_refresh_reset()`), the second `bars` came out
garbled/half black, because `swreset` leaves the controller on power-on register defaults and
nothing re-armed them before the next write. With the fix, the second `bars` is clean. Also run
`disptest seq 0 3 1500` right after a `swreset` — it must be clean too (this exercises the partial
path's own re-arm).

**Publish/refresh gate:** every full/partial refresh now also waits (bounded, at most 1500ms) for
any in-flight `/status`, `/up`, ack or location publish to finish and its 300ms quiet window to
pass, before sending any panel command (net.cpp's `publish_quiet.h`, disp.c's
`disp_pre_write_gate_hook()`). Watch the serial log for `ui: refresh delayed <n> ms for an
in-flight publish` around a mode change or an incoming message — it should appear whenever a
publish and a render land close together, and never during a `disptest` run (console commands
never publish).
### Accelerometer (LIS3DH) bench wiring

Adafruit LIS3DH breakout, on its own I2C bus (`I2C_NUM_1`), rewired 30 Sep 2026, 5 Oct 2026 and
8 Oct 2026 (`pins.h`):

- SDA -> IO17 (Walter pin 21), SCL -> IO18 (Walter pin 22). Not the CardKB's bus (IO10/IO9).
- Address 0x18 (SDO/SA0 open or tied low). If SDO/SA0 is pulled high instead, the chip answers
  at 0x19 and `i2cscan` shows 0x19, not 0x18 — `accel_init()` looks only at 0x18 and will report
  "not found" until either the wiring or `PAGER_I2C_ADDR_LIS3DH` (`pins.h`) changes.
- INT1 -> IO16 (Walter pin 20, `PAGER_PIN_LIS3DH_INT1`), push-pull, active high, 3.3 V logic, no
  external pull-up needed. It is an RTC GPIO, armed as the ext1 light-sleep wake.
- Power: breakout VIN from the power board (6092) 3V terminal, always on.

`i2cscan` is the one-line pre-flight check on the CardKB bus (expect `0x5F`); the LIS3DH is on
the other bus, so its presence check is the boot log's `LIS3DH found (WHO_AM_I=0x33)` line or
`acceltest`. Neither showing up means wiring, not firmware, before anything else is worth trying
(A2's `acceltest` will just say "not present").

### Power board (Adafruit 6092)

SYS terminal (bq25185 4.2 V regulator) → Walter VIN (4.5 V on USB, battery voltage minus protection FET, BUVLO at 3.0 V). 3V terminal (TLV62569 3.3 V buck, 35 µA quiescent) → LIS3DH and eInk Friend VIN (always on). Walter IO0 (3V3-OUT, switched) → CardKB only. The wake button was removed 8 Oct 2026, so ext1 wakes on LIS3DH INT1 alone. Board green 3.3 V LED is removed. Passive distribution carries no capacitors; the TLV62569 wants 10–47 µF total, already satisfied on the 6092.

## Seen working on hardware

### v1.1.0 bench results, 7-8 Oct 2026

- **Soracom SIM attaches with PAP** (`sora1`, IMSI 311588112011642). `AT+CGAUTH=1,1,"sora","sora"` is
  accepted, and the modem roams on 310410 (AT&T, LTE-M) in about 17 s. Direct bearer: session usable
  32 s after boot (release build 25 s); `SETUP done` 26 s after the command. (`docs/SORACOM_DESIGN.md` §6 B)
- **Direct bearer carries pages and replies.** Two web-to-pager pages shown within 1-6 s; two console
  replies reached the web chat. (§6 C)
- **Beam bearer** (plain MQTT to `beam.soracom.io:1883`). Session usable 17.4 s after boot, with no TLS
  step. The web Devices row shows "via Beam" with the CA controls hidden. A page and a reply both work
  through Beam. (§6 E)
- **Provisioning through Beam.** After an NVS erase, `setup` bootstraps through Beam, skips the CA fetch,
  and reports `SETUP done` 10 s after the command. (§6 E)
- **Pages into a sleeping pager.** Release build on the Soracom SIM: +10 and +40 min pages showed "on
  pager" within a minute of Send over the direct bearer (1 h relay-side watch). Over Beam, a page to the
  sleeping unit showed "on pager" about 40 s after Send. (§6 D, E)
- **eDRX granted while roaming on AT&T 310410:** `+CEDRXRDP: 4,"0010","0010","0001"` (20.48 s cycle). (§6 B)
- **OTA full, delta and rollback on proto3 (7 Oct 2026).** A delta and a full image each reached
  `ota_st ok`. An image that aborts at boot came back `rb`, and a refused id reported `bad`.
  (`docs/OTA_DESIGN.md` §7)
- **Battery sampling.** `/status` key 68 `bs` is stored as battery samples. The first sample on proto3
  read 3700 mV (7 Oct), and `sora1` logged about one every 25 min at 3700 mV (8 Oct). The modem's
  reading is not yet checked against a multimeter (M15 in `firmware/README.md`).

### Earlier results (v0.2, debug build)

Debug build, v0.2, against the production relay and broker. Google Fi (T-Mobile) and US Mobile
Dark Star (AT&T) SIMs.

- Setup from a typed code; restart into the normal session; no reset loop.
- MQTT over TLS 1.2 with a pinned CA; signed CBOR in both directions.
- A page from the web app drawn on the e-paper and acknowledged.
- Identity and counter migration from v0.1; the relay accepts the v0.2 status fields.
- The trust fallback: real handshake failures took the pager to `broken`, and the next validated
  connect healed it to `pinned`.
- A second TLS socket while MQTT stays connected; HTTPS fetches of a 790-byte and a 1939-byte
  certificate, about 3 s each, hashes matching.
- Carrier detection from the SIM (`network 310280, GID1 20FF -> US Mobile Dark Star`).
- GNSS and SMS initialisation accepted by the modem; a missing accelerometer handled.
- A stray `NO CARRIER` with no command pending no longer crashes.
- A page delivered during real light sleep with a 150 ms wake window (35 s and 136 s after sending).
- Coverage loss and recovery: radio off for 70 s and 12 minutes, then on; no modem reset, session
  reconnected in 3 s, page delivered afterwards.
- The CardKB keyboard (22 Sep): every printable key, Enter (0x0d), Esc and the four arrows
  (0xb4-0xb7) decode as the host test predicted; a key wakes the UI from "sleeping" within one
  wake cycle; Enter opens the chat, a typed reply publishes within 50 ms of Enter. The bench cable
  had SDA on IO9 and SCL on IO8 at the time; the 3 Oct rewiring moved it to SDA IO5 / SCL IO4, and
  the 5 Oct rewiring moved it again to SDA IO10 / SCL IO9 (`pins.h`). `i2cscan [swap]` finds the keyboard; `wake` and
  `key <text>` drive the UI from the console. Two things found on 8 Oct 2026: Enter has to reach
  the console as `key \\n` (two backslashes; esp_console strips one, so a single `\n` feeds
  nothing and the composer silently keeps its text), and the input event queue is 8 deep, so a
  `key` burst longer than 8 keystrokes loses its tail. Keep each `key` argument to 8 characters.
- Display partial-refresh two-plane fix (22 Sep): the SSD1680 controller's two image planes must
  be kept equal after every differential update. Pre-fix (`disptest again 0` + `bars` + `seq 2 12
  1500`): garbled bands in odd/even pattern, never settling. Post-fix (`disptest again 1` + `bars` +
  `seq 0 12 1500`): all 13 adjacent 8-row bands correct, pattern read as `w b w b w b w b w b w b w`
  matching prediction exactly. Typing on the physical CardKB: characters appear cleanly, the
  composer holds the right text, and the screen stays clean after typing stops.
- MQTT self-healing verified on the wire (commits 71f6c06, c3f910c, 22-23 Sep): one AT+SQNSMQTTCONNECT
  per boot, no +CME ERROR: 4, liveness ping at 300 s idle answered in 370 ms, modem-initiated
  disconnect recovered in one retry 5 s later (`phaseQ-*.log`).
- Display register-loss fix (07995ff, 23 Sep) verified at the glass: `disptest bars` → `disptest
  swreset` → `disptest bars` → `disptest seq 0 3 1500` all clean; owner read the inverted-columns
  pattern exactly as predicted (`phaseU-inject2.log`). Every refresh now shows a short BUSY (~3-10 ms)
  before the real one.
- Composer overflow (tasks 1.0-1.3, 23 Sep) accepted: leading "…" marker, no overprint, counter only
  from 120 chars, backspace reveals hidden text, Enter sent the whole reply (38 keystroke partials of
  8-16 rows, `phaseU-composer.log`).
- Publish/refresh gate observed: refresh delayed 560 ms for an in-flight publish (`phaseS-gate.log`)

## Not yet seen working

In the order worth testing:

1. **Confirm the 480 s MQTT keepalive keeps a session alive for at least an hour on AT&T.** Reflash
   production firmware, idle the pager (no pages sent), log to the broker's admin console periodically
   to check that the client remains listed. Minimum overnight run.

2. **Bisect the post-wake window between 50 and 200 ms to find the minimum and explain the 136 s
   delivery.** `sleeptest` with `yield_ms` overrides; log the delivery times in relation to the eDRX
   paging cycle (20.48 s). Try a bare `AT` probe right after wake to see if it could allow a shorter
   window.

3. **No-coverage radio duty cycle,** field-tested on hardware. Deregister with `at AT+COPS=2`
   (recover with `at AT+COPS=0`; do NOT use `AT+CFUN=4`); observe backoff times and search windows
   in the log; verify that radio is off during intended off periods and that sustained motion
   (walking around) resets the backoff.

4. **End-to-end `/loc` cell answer:** send a `loc_req`, confirm the relay receives a `cell` envelope
   key 49 and resolves it, confirm the relay stores `src: "cell"` in the fix.

5. A CA push from the web app, with a right and a wrong CA.

6. `gnsstest` outdoors: which radio route the modem accepts (in-place or `CFUN=4` window), time to
   fix cold and hot, re-attach time. Record numbers for tuning attempt and backoff budgets.

7. Modem SMS is not seen working. On the US Mobile line (7 Oct 2026) `AT+CMGS` returns `OK` with
   `+CMGS: -2147483648` (no message reference) and nothing arrives, so that line does not carry it. The
   Soracom preset refuses modem SMS. Relay SMS through Twilio (`docs/RELAY_SMS_DESIGN.md`) has not been
   run on the bench: texts from listed and unlisted numbers are still to test.

8. The accelerometer on the 8 Oct 2026 pins (SDA IO17, SCL IO18, INT1 IO16) is not bench-tested since
   the rewire; the shake was calibrated on the old pins on 6 Oct. The button was removed on 8 Oct, so
   drop it from this list.

Registration took about two minutes at the bench location on AT&T, and a second or two on T-Mobile.
On 8 Oct 2026 the Soracom SIM roamed onto 310410 (AT&T) in about 17 s (`docs/SORACOM_DESIGN.md` §6 B).
