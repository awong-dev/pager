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
| `nettest <host> <port> [udp\|tls\|<bytes>]` | Socket-layer probe. With a byte count: a padded HTTP GET, then waits for a reply. Re-attaches first, which disturbs a live session |
| `mqtttest <host> <port> [ca\|noneca\|emptyca]` | Points the modem's own MQTT client at any host. `emptyca` deletes the certificate in slot 12 first |
| `cafetch <https url> <sha256 hex>` | The CA fetch over a second TLS socket, without applying it. Reports whether the MQTT session survived |
| `gnsstest <seconds>` | One location attempt, bypassing the backoff and the battery floor |
| `smstest <number> <text>`, `smslist` | Send one SMS bypassing the allow-list; show the list and the audit queue |
| `sleeptest <minutes> [yield_ms] [interval_ms]` | Light-sleep window with optional overrides; records pages, session loss, timing; saves to NVS then resets. `sleeptest` alone prints the saved report |
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

**Pattern reading:** After `disptest bars`, every 8-pixel column is either solid black or solid
white. `disptest seq` inverts each column in turn, producing a predictable band-flip sequence
that can be read off as a pattern (e.g. `w b w b w b w b ...`) and compared against the expected
sequence. The leftmost columns (before the starting column of the `seq` range) never change and
appear wrong if the reading is off by one.

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
  composer holds the right text, and the screen stays clean after typing stops. NOT verified: a
  multi-character burst through the console (`key efghijkl`) produced no redraw at all and no
  `partial_count` increment — see "Not yet seen working" for that open item; it is a console-feed
  path only, and does not affect typing on the keyboard.

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

5. Replies from the pager through the web app, end to end: the pager side now works (above);
   check the relay stores the reply and the web app shows it.

6. A CA push from the web app, with a right and a wrong CA.

7. `gnsstest` outdoors: which radio route the modem accepts (in-place or `CFUN=4` window), time to
   fix cold and hot, re-attach time. Record numbers for tuning attempt and backoff budgets.

8. SMS at all: the SIM may not carry it. Then texts from listed and unlisted numbers.

9. The accelerometer, and the button.

Registration takes about two minutes at the bench location on AT&T, a second or two on T-Mobile.
