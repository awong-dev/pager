# School Pager Firmware

ESP-IDF 5.x project for the Walter device (ESP32-S3 + Sequans LTE-M modem).

**Status: code-complete, builds clean, never run on real hardware.**
Every timing/current/visual number in this codebase is either vendor-documented or an engineering
estimate marked `PENDING_HW` — see the measurement checklists and "Residual risks" section below
before flashing a real device. `docs/PROTOCOL.md` §12 has two still-open protocol decisions
(broker free-tier verification, LWT/clean-session) that aren't firmware bugs but do affect what
`net.cpp` can promise.

## Hardware

| Part | Role | Interface |
|---|---|---|
| Walter module (DPTechnics) | ESP32-S3 + Sequans GM02SP LTE-M modem + GNSS | — |
| GDEY029T94-FT01 (SSD1680, 296×128) | E-paper display | SPI: SCK IO12, MOSI IO11, CS IO10, DC IO16, RST IO17, BUSY IO18; VCC gated by P-MOSFET on IO15 (active low) |
| M5Stack CardKB | Keyboard | I2C 0x5F on IO8 (SDA) / IO9 (SCL), polled |
| LIS3DH breakout | Motion wake (not implemented) | I2C 0x18, INT1 on IO2 |
| Push button | Wake / open reply | IO1, active low, RTC GPIO |
| LiFePO4 18650 + LFP charger | Power | VIN 3.0–5.5 V |

GPIO numbers for IO1/IO2/IO11/IO12/IO15 are provisional. All of them live in `main/pins.h` so they
can change without touching logic.

## Device behaviour contract

These are the project's fixed constraints. `docs/PROTOCOL.md` is authoritative for anything that
crosses the wire; this section covers the device-local behaviour the protocol doc does not specify.

- **SIM budget**: 100 MB/month data, 100 SMS/month. The device never sends SMS. It keeps one
  persistent TLS+MQTT session and never reconnects on a timer.
- **Power**: the device sleeps most of the day. In **sleep mode** the modem uses eDRX (target
  20.48 s cycle) and the ESP32 light-sleeps; delivery within ~30 s is acceptable. In **active
  mode** the modem stays connected and messages must show within 5 s.
- **Modes**: boot in sleep mode. Enter active mode on an incoming message or a button press; leave
  it after 10 minutes with no button or keyboard activity. The 10-minute timeout runs from mode
  *entry*, not from the last activity. The CardKB is polled every 100 ms, and only while the reply
  composer is open.
- **Display refresh**: partial refresh for the message pane, with a full refresh every 20th
  partial. 20 is this project's deliberate choice over the panel's more common "~10" guidance.
- **Message bodies** are at most 160 characters.
- The managed `walter-modem` component is never patched in place. Anything the vendor API cannot
  express is either worked around in our own code or documented as a limitation.

## Dependencies

- ESP-IDF 5.x
- `dptechnics/walter-modem` component, pinned to exactly `1.5.0` in `main/idf_component.yml` —
  do not let this float to a newer version without re-reading the vendor source first; two separate
  conclusions in `docs/PROTOCOL.md` turned out to depend on which version was actually checked out.
- Display driver (SSD1680): custom, in-tree (`main/ui.c`) — no external component. See
  `docs/PROTOCOL.md` §6 for why (the one registry candidate found requires a newer ESP-IDF than
  this project builds against).

## Hardware Acceptance Tests

- Sleep-mode current: PENDING_HW
- Acceptance test (message latency): PENDING_HW
- Full list of what to check on first hardware bring-up: the measurement checklists below (M1-M23)
  and the "Residual risks" section's cheapest-experiment column, roughly in priority order.

## Measurement checklist: network and power (all PENDING_HW)

No device has been attached. Every line below is a
number `net.cpp`/`modes.c` either assumes or logs enough to compute, but none has been
measured. See `docs/PROTOCOL.md` §6.5, §8.2-§8.4 for the arithmetic these numbers feed.

| # | What to measure | Why it matters |
|---|---|---|
| M1 | Sleep-mode average current, `T`=5s | Validates the ~1.8-2.1 mA / 43-50 mAh/day estimate (§8.4) |
| M2 | Per-wake awake time / current | Validates the ~50 ms, ~40 mA assumptions behind §8.2's `I_light(T)` model |
| M3 | End-to-end latency: `send.py` timestamp -> serial log line | Validates the 27.3 s (sleep) / 4.3 s (active) worst-case budget (§6.5) |
| M4 | Granted eDRX/PTW from `WALTER_MODEM_NETWORK_EVENT_EDRX_RECEIVED` | Assert granted eDRX == requested `"0010"` (20.48s); logged by `net.cpp`'s network event handler, but not yet confirmed against a real SIM/carrier |
| M5 | RTS hold-off: does the Sequans queue URCs while CTS is deasserted, or drop them? | The single riskiest assumption in the whole sleep design (§8.3); `net_sleep()`'s RTS choreography is unverified end to end |
| M6 | `MEMORY_FULL` event count over 24h | Direct evidence the wake-and-drain cycle is losing messages; counted in RTC (`mqtt_memfull_count`) but never exercised against real traffic |
| M7 | Does the modem send PINGREQ autonomously at the configured keepalive? | Resolved by construction per the library API (no ping call exists), but never observed on the wire |
| M8 | Clean-session behaviour across an ESP32-only reset (modem session survives?) | PROTOCOL.md §12 item 4 - the highest-value follow-up experiment; unresolved and load-bearing for whether §8.3(b) (deep sleep + forced redelivery) is ever worth revisiting |
| M16 | I²C CardKB polling (`ui.c`'s composer read / `input_feed_key()`, F6.2) across repeated 100 ms light-sleep cycles | `DEVICE_PLAN.md` §10's own open assumption: whether the CardKB and its I2C bus survive `net_sleep()`'s light-sleep re-entry without a dropped or garbled byte while the composer is open. The plan's own cheapest experiment is "one afternoon with M1's current trace" |
| M17 | UI-awake window current draw (active mode, composer open, CardKB polled every 100 ms, `PAGER_UI_AWAKE_S`=30s) | Validates `DEVICE_PLAN.md` §5.7's ≈ 7 mAh/day-at-20-interactions estimate and the "one more AT round trip per UI wake (signal + battery), ≈ 100 ms at 40 mA" it is built on |
| M18 | `bars_from_rssi_dbm()` bucket thresholds (`ui.c`: dBm ≥ -85/-95/-105/-115 → 4/3/2/1/0 bars) against a real cell | The dBm conversion itself is settled in source (F3.4: `WalterModem::getRSSI()`/AT+CSQ, `dBm = -113 + raw*2`, `raw==99` guarded off as "no reading" — `net.cpp`'s `net_get_rssi()`); the bucket boundaries chosen for the status-bar icon are engineering estimates that have never been seen against a live signal |
| M19 | `segs_from_batt_mv()` LiFePO4 threshold calibration (`ui.c`: 3300/3250/3200/3100 mV → 4/3/2/1/0 segments) | `DEVICE_PLAN.md` §10: these thresholds were picked, not derived from a real discharge curve under the device's own load; pairs with M15's voltage-reading check |

Also unresolved and not measurable without hardware: whether the modem/broker silently clamp
a 1800s MQTT keepalive, and whether the carrier's NAT tolerates a 1800s idle TCP flow
(PROTOCOL.md §6.2).

## Measurement checklist: display and input (all PENDING_HW)

Same rule as above: no device has been attached.

| # | What to measure | Why it matters |
|---|---|---|
| M9 | Noto Sans / Noto Sans CJK SC legibility on the actual panel, at 12 px and 16 px | F6.1 replaced the earlier hand-authored 5x7 font with real pre-rasterised Noto Sans + Noto Sans CJK SC glyphs (`tools/mkassets.py`) at both sizes, packed into a 597,128-byte (583 KiB) `assets.bin` — comfortably under the 1 MiB `assets` partition (`partitions.csv`) — and already rendered to PNG on the host (`make png`) for eyeballing, but never seen on the actual e-paper panel. `DEVICE_PLAN.md` §10 flags 12 px CJK specifically as the assumption to check first ("Host-side PNG first, then M9 on glass; the fallback is 14 px for `normal`"). Separately, and not a panel-legibility question: `mkassets.py`'s own docstring notes its CJK glyph selection uses code-point order as a frequency stand-in (no offline GB2312 frequency table was available), so a few common hanzi are expected to render as tofu regardless of how legible the font itself is — don't conflate the two failure modes when reading the first photograph. |
| M10 | SSD1680 `0x22` display-update-control-2 values (`0xF7` full, `0xFF` partial) | Carried forward from PROTOCOL.md §6 as "inferred, not verified from the datasheet PDF" — this firmware transcribes them unchanged and adds no independent verification. |
| M11 | BUSY pin polarity (assumed active-high) | `ui.c`'s `disp_wait_busy()` assumes BUSY=1 means busy, a common but unconfirmed SSD1680 breakout convention for this exact panel. If wrong, every refresh will either return immediately (garbage on screen) or hit the 15s timeout and mark the display dead every boot. |
| M12 | SSD1680 partial-refresh RAM continuity across a VCC_EN power cycle | `ui.c` deliberately keeps `PAGER_PIN_DISP_VCC_EN` enabled continuously after `ui_init()` rather than gating it off between every refresh (PROTOCOL.md §8.4's "~0 mA between refreshes" assumption), because power-cycling the panel would very likely wipe the controller's internal old/new RAM planes that partial refresh diffs against — this could not be confirmed against a datasheet. The real display-domain current is therefore higher than §8.4's "~0 mA" figure by an unquantified amount until this is measured. |
| M13 | CardKB byte stream during real typing, including arrow keys and a held key | Confirms `input_decode_key()`'s table (F6.2, exhaustively host-tested over 0x00-0xFF in `firmware/host/test_input.c` but never against real hardware): `0x00`=no key, `0x20`-`0x7E`=printable, `0x08`=backspace, `0x09`=tab, `0x0D`=enter, `0x1B`=esc, and — the two `DEVICE_PLAN.md` §10 rows this line exists to settle — arrow codes `0xB4`/`0xB5`/`0xB6`/`0xB7`=left/up/down/right, and whether the CardKB holds the last key until read (the "key held until read" assumption that would enable key-to-wake; `input.h`'s `input_feed_key()` entry point for this is written but not yet called from anywhere, pending this measurement). Also separately confirms PROTOCOL.md §9.4's own open assumption that the CardKB never emits multi-byte sequences. |
| M14 | Partial-refresh timing per the 20-partial/1-full cadence | Confirms the 20-partial cadence (see "Device behaviour contract" above) doesn't visibly ghost/degrade the panel before the scheduled full refresh, and feeds PROTOCOL.md §6.5's M7 (partial refresh must complete in <1.5s). |
| M15 | `getVoltage()`/`AT+SQNVMON` reading vs. a multimeter across the actual battery | PROTOCOL.md §12 item 6: `net_get_battery_mv()` now publishes a real reading in `/status`'s `batt_mv`, inferred from Walter's public schematics to track `VBAT` rather than a fixed regulated rail, but never confirmed against real hardware. 5-minute check on first bring-up. |
| M20 | Typing ~50 mixed characters on the CardKB, timed against a stopwatch | `DEVICE_PLAN.md` §10: is this acceptable to a non-developer? First bring-up check; if not, the Wi-Fi portal noted in §3.0 is the fallback. |

## Measurement checklist: firmware timing and storage (all PENDING_HW)

Same rule as above: no device has been attached. These three come from `DEVICE_PLAN.md` §10 rows
that explicitly call for a device-side timing measurement (as opposed to the CPU-speed/host-only
numbers already available, cited below for comparison).

| # | What to measure | Why it matters |
|---|---|---|
| M21 | PBKDF2-HMAC-SHA256 timing on the actual ESP32-S3, 10 000 iterations (`lock.c`'s `lock_pbkdf2()`, via `mbedtls_pkcs5_pbkdf2_hmac_ext`) | `DEVICE_PLAN.md` §10 estimates ≈ 50-100 ms on the S3. The only number measured so far is **20.5 ms for 10 000 iterations on the host CPU** (F6.5, `firmware/host/test_lock.c`'s `test_pbkdf2_round_trip_and_timing()`, printed as `PBKDF2 (10000 iterations, host CPU): %.1f ms`) — this is explicitly a host-CPU number, not the S3's (no hardware crypto acceleration assumed, different core), and must not be read as an on-device result. Adjust the iteration count to land near 100 ms once the real number is in, per §10's own instruction. |
| M22 | NVS blob write time for a 320-byte blob (`msg.c`'s `msgq` namespace, full-fidelity reply/unread bodies moved off RTC in F6.4) | `DEVICE_PLAN.md` §10: must complete well under the 100 ms composer poll and never block the modem event task (it runs on the main task either way, so a slow write is a composer-lag bug, not a crash). No host-side substitute exists — NVS write timing is real-flash-dependent. |
| M23 | `esp_partition_mmap()` boot-time cost for the `assets` partition (`gfx.c`'s `gfx_init()`) | Static facts are already known and require no hardware: the partition is 1 MiB (`partitions.csv`, `0x100000`) and the real payload is 597,128 bytes (583 KiB, F6.1's `assets.bin`), so there is no size-fit risk. `gfx.c`'s own comment claims the mmap "draws no additional current and reads nothing until a glyph is blitted" — that is a design argument, not a measurement. What is still unmeasured: the actual wall-clock cost of the `esp_partition_mmap()` call itself at boot, and whether mapping ~583 KiB of flash consumes MMU page-table capacity the app's own IROM/DROM mappings need (two 2 MiB OTA slots already share that budget). |

## Build

Set up ESP-IDF, then:

```bash
idf.py build
```

## Residual risks

A design review against `net.cpp` / `modes.c` / `msg.c` / `ui.c` as committed.
Build verified clean on `espressif/idf:release-v5.2` (`idf.py set-target esp32s3 && idf.py build`,
exit 0, only the vendor's own Kconfig style warning). `sizeof(pager_rtc_t)` = **928 B** from
`build/school_pager.map` (`.rtc.data.0` under `libmain.a`), inside the 1184 B ceiling of
PROTOCOL.md §9.1; the `_Static_assert` at `modes.c:138` is present and passing. `T` = 5 s sleep /
2 s active (`modes.c:45-46`), post-wake yield 50 ms ≥ the 30 ms floor (`modes.c:47`), keepalive
1800 s, eDRX `"0010"`/`"0001"`, PSM disabled, cert slot 12 / TLS profile 2 — all match the doc.

**Still no hardware has ever been attached.** Everything below is reasoning against source, not
measurement. Ranked by risk to battery life and message latency.

### Already fixed (both rebuilt clean)
- `net.cpp` + `net.h` + `modes.c`: `net_modem_busy()` interlock. `_eventProcessingTask` is
  priority 4 and `modes_run()` is priority 1, so every time the MQTT event handler blocks
  (`mqttReceive()`'s AT round trip, `ui.c`'s 10 ms `disp_wait_busy()` poll) `modes_run()` is
  scheduled and could call `esp_light_sleep_start()` — forcing RTS high *mid-response* instead of
  at an idle moment. That turns M5 ("does the Sequans queue or drop when CTS is deasserted") from
  a latency question into a message-loss one, on essentially every inbound message. `skip_sleep`
  now includes `net_modem_busy()`. Cost ≈ 0.02 mAh per message of 40 mA busy-polling (estimate).
- `modes.c:345-356` `on_incoming_message()`: copy `out->id` before rendering. `out` points into
  `msg.c`'s `s_thread`, which `thread_insert_locked()` memmoves; a reply submitted from the main
  task during the render would have made the `shown` ack carry the reply's `u_...` id.

### R1 — F4 hard-resets the modem on loss of *coverage*, not just on a wedged modem (battery, data, latency)
`net_check()` (`net.cpp:484-493`) returns false both when `checkComm()` fails **and** when the
registration state is not HOME/ROAMING. `run_modem_health_check()` (`modes.c:444-474`) treats that
as "modem unresponsive" and calls `net_recover_modem()` → `WalterModem::reset()` → full re-attach
→ new ~5 kB TLS handshake. A student indoors in a dead zone therefore gets a modem hard reset
every 10 minutes (the rate limit), each one destroying the persistent session the SIM budget
requires us to protect. Spec for the fix: `net_check()` should report *comm* only; add a separate
`net_is_registered()`; F4 resets only on `checkComm()` failure, and a registration loss is simply
waited out (the modem re-attaches on its own) with a counter in `/status`. Cheapest check: no
hardware needed — put the device in a Faraday bag / pull the antenna and watch for `modem_resets`
climbing in the serial log.

### R2 — a held or stuck button busy-polls at full power indefinitely (battery)
`BTN_HELD` (`modes.c:585-589`) only exits when the button is released, and `modes_run()`'s
`skip_sleep` keeps `net_sleep()` out for as long as the FSM is not IDLE (`modes.c:704`). There is
no tickless idle in `sdkconfig.defaults`, so that is ~40 mA continuously: a button wedged in a
backpack flattens a 1500 mAh cell in roughly 1.5 days. (A worse version of this — a
level-triggered ext0 wake spinning `esp_light_sleep_start()` — is already fixed; the held-button
case is still a full-power loop.) It also makes the F4 health check fire every ~1.2 s instead of every
5 min, because that check counts wake cycles, not time (`modes.c:771-774`). Fix spec: add a
`BTN_STUCK` state entered after ~5 s in `BTN_HELD`, and a `net_sleep()` variant that arms ext0 on
level **1** (wake on release) so the device can light-sleep while the button is down.
Cheapest check: tape the button down and watch the serial log's wake-cycle counter rate.

### R3 — display VCC is never gated off, and the panel is never put to sleep (battery)
`ui_shutdown()` (`ui.c:573`) is the only caller of `disp_power_off()` and **has no call site
anywhere in the firmware** — so IO15 is driven on at `ui_init()` and stays on forever, and the
SSD1680 never receives its `0x10` deep-sleep command in normal operation. `ui.c:228-235`'s own
comment claims the rail is "gated off between refreshes", and PROTOCOL.md §8.4 budgets the display
at "~0 mA" on that basis. M12 already records the deliberate reason (a VCC cycle would wipe the
controller RAM planes that partial refresh diffs against), so this is a known trade, not a
surprise — but the size of the penalty is still unknown and it sits directly on the 1.8-2.1 mA
budget. Mitigating evidence: both `0x22` update sequences (`0xF7` full, `0xFF` partial) have the
disable-analog/disable-clock bits set, so the booster and oscillator should stop after each
refresh, leaving only standby leakage. Cheapest check: M1's current trace with the panel attached
vs. detached — one measurement answers it. If it is material, the fix is to issue `0x10`/`0x01`
(deep sleep mode 1) after each refresh and hardware-reset + re-init before the next, keeping VCC
on; only re-verify that the RAM planes survive it.

### R4 — `ui.c` has no cross-task mutual exclusion (crash / corrupt display)
`ui_render_thread()` / `ui_render_message_pane()` / `ui_show_toast()` run on **two** tasks:
`_eventProcessingTask` via `on_incoming_message()` (`modes.c:363`) and `modes_run()`'s task via the
button FSM, composer and toasts. They share `s_fb_new`/`s_fb_old`/`s_partial_count` and one
`spi_device_handle_t` with no lock (`ui.c:48-55`). A render is preemptible at every
`disp_wait_busy()` `vTaskDelay`, so two renders can interleave their SSD1680 command streams. The
likely trigger is exactly the common case: a message arrives while the student is pressing the
button. Expect a garbled panel or an SPI driver abort. Fix spec: one static mutex in `ui.c` taken
by every public entry point (never held across a call back into `msg.c`/`modes.c`).

### R5 — a message render occupies the modem's event task for 0.3-1.5 s, or up to 30 s if BUSY hangs (latency, message loss)
`on_incoming_message()` runs on `_eventProcessingTask` and synchronously does a full e-paper
refresh (`modes.c:363` → `ui.c:591`). While it runs, no other MQTT event is dispatched:
`_DISCONNECTED` edges are delayed, and further `_MESSAGE` events queue in the modem — which is
what `MEMORY_FULL` (M6) reports. Worst case, with a mis-wired BUSY pin (M11), each refresh path
burns two 15 s timeouts plus a re-init before `mark_display_dead()` latches, i.e. ~30 s of dead
event task for the first message. Fix spec: hand the render to `modes_run()` via a flag
(`s_render_pending`), leave the event handler doing ingest + dedup only, and move the
`msg_mark_shown()` call to after the render on the main task — which also preserves §4's
"ack only after BUSY deasserts". Cheapest check: log `esp_timer` deltas around
`ui_render_message_pane()` and around the handler; M7/M14 collect the same numbers.

### R6 — `msg_thread_at()` / `msg_newest_unread()` return unlocked pointers into a ring that moves (wrong-id ack, garbled render)
`msg.c:635-652` are the only accessors that do **not** take the lock that `msg.h`'s header comment
says guards `s_thread`, and they hand out raw `msg_t*`. `ui.c`'s `render_thread_frame()`
(`ui.c:440-466`) dereferences them across a multi-hundred-millisecond render, and
`button_action_short()` (`modes.c:531`) copies an id out of one, while the event task can memmove
the whole ring under both. Worst outcome is a `read` ack for the wrong id; most likely outcome is
one garbled line on screen. Fix spec: take the lock inside those accessors and return a
caller-supplied *copy*, not a pointer.

### R7 — `shown` is acked even when nothing was shown (false delivery reporting)
If `ui_init()` failed or a BUSY timeout latched `s_display_dead`, `ui_render_message_pane()`
returns immediately (`ui.c:593-595`) and `modes.c` still calls `msg_mark_shown()`. The parent is
then told the student saw a message that was never rendered — PROTOCOL.md §4 defines `shown` as
"after the e-paper refresh completes (BUSY deasserted), never before". Running headless is
correct; lying about `shown` is not. Options, none free: suppress the `shown` ack
while headless (the relay then re-publishes forever), or add a `disp` field to `/status` (a schema
change, so it needs an owner). Decide before a parent relies on the delivered state.

### R8 — retained `/status` is **not settable** from `walter-modem` v1.5.0 (parent-visible staleness)
`WalterModem::mqttPublish()` emits `AT+SQNSMQTTPUBLISH=0,<topic>,<qos>,<len>`
(`src/proto/WalterMQTT.cpp:97-103`) — there is no retain argument anywhere in the public API.
PROTOCOL.md §2 and §5 mark `/status` **retained = true (fixed)**, and §5.3 keys the relay's
behaviour on "a retained `offline`". §12 item 3 currently lists only LWT and clean session as
unreachable; **retain is a third one and should be added to it.** Consequence today: after a relay
restart the parent UI has no device state until the next event-driven publish or the ≤3600 s
heartbeat (§5.4d) — and the heartbeat is suppressed entirely while the device has no network clock
(`modes.c:253-256`), which is also the case where the device is least healthy. Cheapest check:
30 minutes with the Sequans AT manual, same session as §12 item 3's raw-`AT+SQNSMQTTCFG`
experiment for the LWT; if the AT command has a retain field, both are fixable via `sendCmd()`.

### R9 — a full refresh can land inside the message-delivery path (latency)
`set_mode()` → `ui_render_thread()` → `refresh_cadence()` (`ui.c:402-409`) does a **full** refresh
every 20th partial, and the sleep→active edge caused by an incoming message goes through exactly
that path (`modes.c:357`). PROTOCOL.md §6.5 budgets 0.5-1.5 s for the refresh against 2.7 s of
total margin; a 296x128 SSD1680 full refresh is typically 2-4 s at room temperature and colder at
a bus stop in winter. Sleep-mode worst case then lands at ~30-32 s, i.e. outside target. Also note
`ui_render_message_pane()` calls `partial_refresh()` directly and bypasses the cadence check, so
the full refresh tends to fire from whichever path happens to be a message. M7/M14 measure the two
refresh times; if the full refresh exceeds ~1.5 s, defer it (e.g. run it on the active→sleep
transition, never on the inbound path).

### R10 — `msg_pump()` can drop a `read` ack it raced with, and "published" is not "PUBACK'd" (ack loss)
`msg_pump()` (`msg.c:549-576`) drops the lock across `publish_ack()` and then clears `a->in_use`.
If `msg_mark_read()` upserted the same slot during that window (student presses the button while
the `shown` ack is in flight), the upgrade to `read` is silently discarded and the thread stays at
`shown` forever. Separately, and already documented at `msg.c:538-545`: `net_publish()` returning
true means the library accepted the call, not that a PUBACK arrived, so §4.1 rule 6's
"3 attempts then drop" retry counts attempts that may never have reached the broker. Fix spec for
the first: re-read the slot's `state` after re-acquiring the lock and only clear `in_use` if it
still matches what was published.

### R11 — ext0 pull-up across light sleep is unverified (battery, phantom presses)
The button pull-up is configured digitally (`gpio_config()`, `modes.c:498-508`) but ext0 wake keeps
the RTC peripheral domain powered and reads the pad through RTC IO, whose own pull settings
default to off. If the digital pull-up is not in force during light sleep and there is no external
pull-up on IO1, the pin floats and the device wakes constantly (battery) with phantom short
presses (UX). I am not certain either way for light sleep on the ESP32-S3 and will not guess.
Cheapest check: log `esp_sleep_get_wakeup_cause()` after every `net_sleep()` and count non-timer
wakes over an idle hour; if it is real, the fix is one `rtc_gpio_pullup_en()` call, no GPIO change.

### R12 — the RX path mallocs (message loss under fragmentation)
`msg_ingest_down()` parses with cJSON (`msg.c:231`) on the event task. PROTOCOL.md §3.3's stated
reason for the 640-byte cap is that "the device never mallocs on the RX path"; it does. Low
probability on a 512 kB part, but the failure mode is silent (parse fails → counted as malformed →
message never acked, never rendered). Cheapest check: log the heap low-water mark in the `/status`
heartbeat and watch it over a 24 h soak.

### R13 — a boot that fails to attach has no retry path of its own (battery, availability)
`modes_boot()` increments `g_rtc.attach_fail_cycles` on a failed `net_init()` (`modes.c:638`) and
**nothing ever reads it again**; `modes_run()` only ever retries `net_session_up()`. A device
powered on out of coverage recovers only via R1's modem-reset path — the most expensive action in
the design — and only in sleep mode. Fix spec: retry `net_init()` on the F1 backoff schedule
before ever escalating to a reset, and delete or use the dead counter.

### R14 — `net_init()` / `net_recover_modem()` block the whole loop for up to 300 s (availability)
The attach wait (`net.cpp:350-362`) is a 300 s `vTaskDelay` loop on the caller's task. Called from
`run_modem_health_check()` it stalls `modes_run()`: no button FSM, no composer polling, no
`msg_pump()`, no light sleep for up to five minutes. The student sees a dead pager and 40 mA.
Cheapest check: falls out of R1's Faraday-bag test.

### R15 — F4 never runs in active mode, and counts cycles rather than time
`modes.c:771-774` gates the health check on `mode == SLEEP`, so a modem that wedges during an
active window is not noticed for up to 10 minutes, and the "every 60 wakes" period varies from
5 min (sleep) to 2 min (active) to ~1.2 s (R2's stuck button). Prefer a monotonic timestamp.
Low impact on its own; listed because it interacts with R1 and R2.

### `NEEDS HUMAN DECISION` items, current as of the last commit
- **§12 item 3 (LWT + clean session + retained `/status` not settable): still open.** Nothing
  worked around any of the three; `net.cpp` registers no will and passes no session flag, because
  the API has neither. All three would be settled (or not) by the same look at the Sequans AT
  manual plus the `sendCmd()` fallback PROTOCOL.md §12 already proposes.
- **§12 item 6 (battery voltage): resolved, one experiment remains.** `net.cpp` now reads the
  modem's own `AT+SQNVMON` supply-rail voltage via `WalterModem::getVoltage()` and reports it as
  `batt_mv` (`modes.c`'s `build_status_json()`), with a last-known-good fallback so a failed AT
  round trip never blocks a `/status` publish. This needed no GPIO change — see PROTOCOL.md §12
  item 6 for the schematic-based reasoning that this rail tracks the battery, not a fixed 3.3V
  rail. **Still open**: that reasoning is inference from Walter's public schematics, not a
  confirmed fact about Walter's own unpublished internal routing — compare `getVoltage()`'s reading
  against a multimeter on first hardware bring-up (5 min, M15 in the checklist above).
- **§12 item 2 (broker free-tier limits): unchecked.** Nobody has set up a real HiveMQ/EMQX account
  yet — only local `docker compose` mosquitto has been exercised. Confirm the chosen free tier
  actually supports QoS 1 both directions, a retained topic, a ~1800s keepalive, and a persistent
  session before flashing a device against it.
