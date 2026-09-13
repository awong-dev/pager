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
