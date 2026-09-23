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

## Console commands

Setup mode has `setup` and `carrier`. The debug build has all of these, in any mode.

| Command | Does |
|---|---|
| `setup <code>` | Provision from a setup code |
| `carrier` / `carrier <n>` / `carrier custom <apn>` | Show or set the APN choice. 0 = automatic |
| `nettest <host> <port> [udp\|tls\|<bytes>]` | Debug build only. Socket-layer probe. With a byte count: a padded HTTP GET, then waits for a reply. Re-attaches first, which disturbs a live session |
| `mqtttest <host> <port> [ca\|noneca\|emptyca]` | Debug build only. Points the modem's own MQTT client at any host. `emptyca` deletes the certificate in slot 12 first |
| `cafetch <https url> <sha256 hex>` | The CA fetch over a second TLS socket, without applying it. Reports whether the MQTT session survived |
| `gnsstest <seconds>` | One location attempt, bypassing the backoff and the battery floor |
| `smstest <number> <text>`, `smslist` | Send one SMS bypassing the allow-list; show the list and the audit queue |
| `sleeptest <minutes> [yield_ms] [interval_ms]` | Light-sleep window with optional overrides; records pages, session loss, timing; saves to NVS then resets. `sleeptest` alone prints the saved report. `yield_ms` is `0` or 30..10000, `interval_ms` is `0` or 200..60000; `0` (or omitting the argument) keeps the build default, so `sleeptest 6 0 0` and `sleeptest 6` are the same run |
| `coverage` | Debug the no-coverage radio duty cycle (deregister with `at AT+COPS=2`, recover with `at AT+COPS=0`) |
| `at <command>` | One raw AT command; the reply shows in the trace |
| `acceltest [samples <n>\|ths <0-127>\|dur <0-127>\|refr <seconds>]` | LIS3DH register/sample dump and runtime tuning (A2); re-probes WHO_AM_I every call, so wiring the chip needs no reboot. `acceltest` alone prints registers, THS in mg, the refractory setting, edges reported and ext1 wake count |

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

I2C bus shared with the CardKB, same two pins (`pins.h`'s own bench note next to
`PAGER_PIN_KB_SDA`: "was 8/9 the other way round") — **not** the other way round:

- SDA -> IO9, SCL -> IO8
- Address 0x18 (SDO/SA0 tied low). If SDO/SA0 is pulled high instead, the chip answers at 0x19
  and `i2cscan` shows 0x19, not 0x18 — `accel_init()` looks only at 0x18 and will report "not
  found" until either the wiring or `PAGER_I2C_ADDR_LIS3DH` (`pins.h`) changes.
- INT1 -> IO2 (`PAGER_PIN_LIS3DH_INT1`), push-pull, active high, no external pull-up needed.
- Power from the 3V3 peripheral rail (the one `board_power_init()` turns on by driving GPIO0
  low) — not directly from a separate 3V3 source.

`i2cscan` is the one-line pre-flight check: expect `0x5F` (CardKB) and `0x18` (or `0x19` per the
address note above). Neither showing up means wiring, not firmware, before anything else is
worth trying (A2's `acceltest` will just say "not present").

## Seen working on hardware

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
  has SDA on IO9 and SCL on IO8 (`pins.h`). `i2cscan [swap]` finds the keyboard; `wake` and
  `key <text>` drive the UI from the console.
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

7. SMS at all: the SIM may not carry it. Then texts from listed and unlisted numbers.

8. The accelerometer, and the button.

Registration takes about two minutes at the bench location on AT&T, a second or two on T-Mobile.
