# School Pager Firmware

ESP-IDF 5.x project for the Walter device (ESP32-S3 + Sequans LTE-M modem).

## Dependencies

- ESP-IDF 5.x
- `dptechnics/walter-modem` component
- Display driver (SSD1680) — TBD in Phase 5

## Hardware Acceptance Tests

- Sleep-mode current: PENDING_HW
- Acceptance test (message latency): PENDING_HW

## Phase 4 measurement checklist (all PENDING_HW)

No device is attached to any session that produced this checklist. Every line below is a
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

Also unresolved and not measurable without hardware: whether the modem/broker silently clamp
a 1800s MQTT keepalive, and whether the carrier's NAT tolerates a 1800s idle TCP flow
(PROTOCOL.md §6.2).

## Phase 5 measurement checklist (all PENDING_HW)

Same rule as above: no device is attached to any session that produced this checklist.

| # | What to measure | Why it matters |
|---|---|---|
| M9 | Font legibility on the actual panel | `ui.c`'s 5x7 font is hand-authored for this project (not transcribed from an external font file); it has never been rendered or photographed. Structurally correct (right glyph count/size), but on-glass legibility is completely unverified. |
| M10 | SSD1680 `0x22` display-update-control-2 values (`0xF7` full, `0xFF` partial) | Carried forward from PROTOCOL.md §6 as "inferred, not verified from the datasheet PDF" — this firmware transcribes them unchanged and adds no independent verification. |
| M11 | BUSY pin polarity (assumed active-high) | `ui.c`'s `disp_wait_busy()` assumes BUSY=1 means busy, a common but unconfirmed SSD1680 breakout convention for this exact panel. If wrong, every refresh will either return immediately (garbage on screen) or hit the 15s timeout and mark the display dead every boot. |
| M12 | SSD1680 partial-refresh RAM continuity across a VCC_EN power cycle | `ui.c` deliberately keeps `PAGER_PIN_DISP_VCC_EN` enabled continuously after `ui_init()` rather than gating it off between every refresh (PROTOCOL.md §8.4's "~0 mA between refreshes" assumption), because power-cycling the panel would very likely wipe the controller's internal old/new RAM planes that partial refresh diffs against — this could not be confirmed against a datasheet in this session. Flagged as a `NEEDS HUMAN DECISION` item in the Phase 5 report; the real display-domain current is therefore higher than §8.4's "~0 mA" figure by an unquantified amount until this is measured. |
| M13 | CardKB byte stream during real typing | Confirms `ui_poll_keys()`'s assumption (0x00=no key, printable ASCII, 0x08=backspace, 0x0D=enter, ignore >=0x80) against real hardware, and separately confirms PROTOCOL.md §9.4's own open assumption that the CardKB never emits multi-byte sequences. |
| M14 | Partial-refresh timing per the 20-partial/1-full cadence | Confirms HANDOFF.md §5's explicit 20-partial requirement doesn't visibly ghost/degrade the panel before the scheduled full refresh, and feeds PROTOCOL.md §6.5's M7 (partial refresh must complete in <1.5s). |

## Build

Set up ESP-IDF, then:

```bash
idf.py build
```

## Residual risks (Phase 6 final review)

`firmware-architect`, Phase 6, against `net.cpp` / `modes.c` / `msg.c` / `ui.c` as committed.
Build verified clean on `espressif/idf:release-v5.2` (`idf.py set-target esp32s3 && idf.py build`,
exit 0, only the vendor's own Kconfig style warning). `sizeof(pager_rtc_t)` = **928 B** from
`build/school_pager.map` (`.rtc.data.0` under `libmain.a`), inside the 1184 B ceiling of
PROTOCOL.md §9.1; the `_Static_assert` at `modes.c:138` is present and passing. `T` = 5 s sleep /
2 s active (`modes.c:45-46`), post-wake yield 50 ms ≥ the 30 ms floor (`modes.c:47`), keepalive
1800 s, eDRX `"0010"`/`"0001"`, PSM disabled, cert slot 12 / TLS profile 2 — all match the doc.

**Still no hardware has ever been attached.** Everything below is reasoning against source, not
measurement. Ranked by risk to battery life and message latency.

### Fixed in this review (both rebuilt clean)
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
every 10 minutes (the rate limit), each one destroying the persistent session HANDOFF.md §1
requires us to protect. Spec for the fix: `net_check()` should report *comm* only; add a separate
`net_is_registered()`; F4 resets only on `checkComm()` failure, and a registration loss is simply
waited out (the modem re-attaches on its own) with a counter in `/status`. Cheapest check: no
hardware needed — put the device in a Faraday bag / pull the antenna and watch for `modem_resets`
climbing in the serial log.

### R2 — a held or stuck button busy-polls at full power indefinitely (battery)
`BTN_HELD` (`modes.c:585-589`) only exits when the button is released, and `modes_run()`'s
`skip_sleep` keeps `net_sleep()` out for as long as the FSM is not IDLE (`modes.c:704`). There is
no tickless idle in `sdkconfig.defaults`, so that is ~40 mA continuously: a button wedged in a
backpack flattens a 1500 mAh cell in roughly 1.5 days. (Phase 5 fixed the *worse* version of this
— a level-triggered ext0 wake spinning `esp_light_sleep_start()` — but the held-button case is
still a full-power loop.) It also makes the F4 health check fire every ~1.2 s instead of every
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
correct per HANDOFF.md; lying about `shown` is not. Options, none free: suppress the `shown` ack
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

### Two `NEEDS HUMAN DECISION` items, re-checked against the final code
- **§12 item 3 (LWT + clean session not settable): still accurate, and now understated.** Nothing
  in Phase 5 worked around either; `net.cpp` registers no will and passes no session flag, because
  the API has neither. Add **retained `/status`** to the same item per R8 — all three would be
  settled (or not) by the same look at the Sequans AT manual plus the `sendCmd()` fallback.
- **§12 item 6 (no battery ADC pin): still accurate and unchanged.** `modes.c:217` still publishes
  a hardcoded `batt_mv = 3300` with the placeholder documented in place; `pins.h` gained no ADC
  pin, `ui.c` renders no battery figure (its status line shows mode / link / unsent only), and
  nothing else in Phase 5 consumes `batt_mv`. Option (b) from that item — asking `walter-modem`
  for a supply reading — is still unchecked and is the only option that needs no hardware change.
