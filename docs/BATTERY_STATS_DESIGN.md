# Battery stats — design (7 Oct 2026)

Brief: build/bench-logs/DESIGN_battery_brief.md. Tasks: build/bench-logs/TASK_batt_{firmware,backend,web}.md.
*Measured* = from logs or code on 7 Oct 2026. *Estimate* = not measured, assumption stated. The 6092 board has
no fuel gauge, and the LiPo's 3.9→3.7 V plateau makes voltage a poor rate sensor. So the device counts how long
it spends in each state, and the drain is modelled as Σ(state seconds × state current). The currents start as
estimates and are refitted once from a full discharge.

## 1. Decisions

| # | Decision |
|---|---|
| B1 Window | The pager keeps one **window** of counters. It opens at boot, or at the last successful `/status` publish (`net_publish_raw()` returned true). On success the published values are **subtracted** from the counters rather than the counters being zeroed: the ms remainders carry over, and nothing counted between build and publish is lost. If a publish fails, nothing is subtracted and the next `/status` carries a larger window with the **same `sq`**, which the relay overwrites (idempotent, B6). |
| B2 Storage | One `RTC_NOINIT_ATTR battstat_rtc_t` in a new `battstat.c`, with its own magic and CRC32. It holds 19 u32 accumulators (12 times in **ms**, 7 counts) + `mvn` u16 + `sq` u32 + magic/CRC, **92 B**. Light sleep keeps RAM anyway. RTC_NOINIT also survives software, panic and watchdog resets and the reboot out of airplane mode (that mode only changes at boot, net.h:70), the same way watchdog.c:36 relies on it. After a power-on the struct is garbage: the CRC fails, it is zeroed, and `sq` goes back to 0. It is **not** in `pager_rtc_t`, because writing a field there without `rtc_save()` would fail g_rtc's CRC and cold-boot the whole message state. |
| B3 Attribution | Each loop iteration's awake time goes to **one cause**: `battstat_tick()` at the top of the loop charges the previous iteration's awake time, i.e. wall time minus the time spent inside `net_sleep()`. The cause is the highest-priority one seen during that iteration: **fetch > modem > ui > hot > attn > timer**. It is sampled at the `skip_sleep` computation (modes.c:2562) and again at the bottom of the loop, and `net_publish_raw()` calls `battstat_raise(MODEM)`. Mapping: fetch = `cafetch_in_progress()` (book, CA and OTA); modem = `net_modem_busy`, `connect_in_flight`, `publish_in_flight` or `resub_hold`; ui = `input_awake`, `btn_busy` or `btn_stuck`; hot = `input_hot` or `accel_shake_pending`; attn = the 1 s attentive cadence; timer = everything else (the 200 ms post-wake yield and the probe wait). |
| B4 GNSS is a modem state, not a cause | The loop light-sleeps during a GNSS attempt: `loc_attempt_in_progress()` is not in `skip_sleep`. ESP32-awake time therefore cannot see GNSS. So **modem-state seconds** are counted over the whole span, sleep included, from the state at each tick: `off` = `net_airplane()` or coverage owns the radio; `search` = not off and `net_unregistered_for_s() > 0`; `gnss` = `loc_attempt_in_progress()`. Registered idle is `dt − off − search − gnss`. |
| B5 Wire | **One new envelope key, 68 `bs`** (a sub-map), in every online `/status` once the window has at least 1 s. No new message kind and no new publish. `batt_mv` (key 23) stays the reading at publish time. |
| B6 Relay | Each online `/status` stores one doc `devices/{id}/battery/{session}-{sq}` (auto id if there is no `bs`, so current firmware still gets a voltage series). Retention is **90 days** (`settings/retention.batteryDays`), swept by `createdAt` like `locations`. `GET /api/devices/{id}/battery?since=&until=` returns samples (owner, family admin or super). Currents live in `batteryModels/{modelId}` (default `walter-6092-lipo2500`), written by a super-only PUT. A missing doc returns the priors with `source:"prior"`. |
| B7 Web | A **Battery card** on `/devices/[id]` shows voltage over time, stacked awake s/h by cause, refreshes per day, and the modelled mAh/day with an editable current table labelled **"model, uncalibrated"** until `source:"fit"`. It also has a CSV export. The Devices list gets a compact `mV` cell next to the OTA "Firmware" column, linking to the card. Charts are hand-written SVG, with no new npm dependency. |
| B8 Not counted | **Modem TX/RX bytes**: walter-modem exposes no byte counter (WalterModem.h has none; the only per-read count is PATCHES 1.22's `+SQNSRECV` length). Reconnects (`cn`) and radio events (`re`) stand in for it, since reconnects are the dominant data and energy term (PROTOCOL §7.3). |

## 2. `/status` `bs` sub-map (proposed PROTOCOL text; §10 is the key table the brief calls §14)

**Applied to PROTOCOL.md §3.3, §5.1 and §10 on 6 Oct 2026.** Added there: `bs` is CBOR-only (a JSON `/status` with `bs` is 695–858 B, over 640), and every key except `mvn` is required, as in the relay's `parse_bs`.


§10 envelope row: `| 68 | bs | map | /status (battery stats, BATTERY_STATS_DESIGN.md; optional) |`. Sub-map keys (all uint;
JSON uses the same names). A relay MUST drop an invalid `bs` alone, never the whole `/status`.

| Key | Name | Meaning | Source |
|---|---|---|---|
| 0 | `sq` | Window number. +1 per committed window and kept across resets; 0 after a power-on | battstat |
| 1 | `dt` | Wall seconds the window covers | esp_timer deltas |
| 2 | `sl` | ESP32 light-sleep seconds (inside `net_sleep()`) | battstat |
| 3 | `aw` | `[timer, attn, hot, ui, modem, fetch]` ESP32-awake seconds by cause, in rising priority order (B3). `dt ≈ sl + Σaw` (±1 s) | battstat |
| 4 | `ns` | Light-sleep entries | battstat |
| 5 | `x1` | ext1 (accelerometer) wakes | Δ`net_get_ext1_wakes()` |
| 6 | `rl` | 3V3 peripheral rail-on seconds (display + CardKB) | new `rail_on_ms_total()` |
| 7 | `rf` | `[full, partial, upgraded]` refreshes | Δ`disp_get_refresh_stats()` |
| 8 | `mvn` | Minimum valid `AT+SQNVMON` reading in the window. Omitted if there was none (airplane) | `refresh_batt_mv()` |
| 9 | `cn` | MQTT session (re)connects (TLS handshakes) | Δ`s_mqtt_link_counter` |
| 10 | `md` | `[off, search, gnss]` modem-state seconds (B4) | tick-sampled |
| 11 | `re` | Radio events: successful publishes + liveness SUBSCRIBEs + inbound MQTT messages (each one costs an RRC tail) | new `net_get_radio_events()` |

`mvn` uses only the readings that already happen (each `/status` and each UI-wake edge); **no new AT reads are added**.
Whether `AT+SQNVMON` sags under TX is UNVERIFIED. If `batt_mv − mvn` stays under 10 mV for a week, `mvn` gets dropped.

## 3. Model and priors (web constants; relay `batteryModels` default)

mAh = [I_sleep·sl + Σ I_aw[c]·aw[c] + I_rail·rl + I_idle·(dt−off−search−gnss) + I_off·off + I_search·search + I_gnss·gnss]/3600
+ E_re·re + E_cn·cn + E_full·(full+upgraded) + E_part·partial.

| Term | Prior | Basis |
|---|---|---|
| I_sleep | 1.0 mA | vendor, PROTOCOL §8.4 (board-level light sleep) |
| I_aw timer/ui/hot/attn/modem | 40 mA | §8.4 estimate (ESP32 awake at 160 MHz); the modem's RRC cost is in E_re/E_cn |
| I_aw fetch | 120 mA | OTA_DESIGN §4 (40 ESP32 + 80 modem streaming) |
| I_rail | 5 mA | estimate: CardKB ATmega328 at 3.3 V plus the gated Friend at idle; UNVERIFIED |
| I_idle / I_off / I_search / I_gnss | 0.35 / 0.03 / 20 / 30 mA | §8.4 eDRX 0.2–0.5 and floor 0.01–0.05; search and GNSS are estimates |
| E_re / E_cn | 0.10 / 0.20 mAh | §8.4 ping figure (RRC tail); connect = 2.6 s handshake (*measured* bookpull elapsed) × 80 mA + tail |
| E_full / E_part | 0.010 / 0.001 mAh | estimate: panel only (≈8 mA × 3 s / 0.4 s); the ESP32 time is already in `aw` |

**Modelled idle sleep-mode day** (no use, T = 20 s, modes.c:132): 4320 wakes × 0.35 s awake (estimate; the sleeptest
windows show 91–100 % asleep, *measured*) gives 16.8 mAh. Light sleep is 23.6, eDRX 8.4, and (288 liveness pings at 300 s,
xport_lte.cpp:57, + 24 statuses) × 0.1 = 31.2. Total **≈ 80 mAh/day ⇒ ≈ 30 days** on 2500 mAh × 0.95 usable.
§8.4's 95–107 mAh/day assumed T = 5 s. The counters replace the 0.35 s guess with a measured figure on day 1.

## 4. Calibration (owner, bench, once per hardware model)

0. **Gate, PROTOCOL §12.6 / README M15, 5 min**: compare the multimeter across the cell with the Device screen's `batt` at
   full charge and again near 3.75 V. If they differ by > 50 mV or do not track, the voltage series is meaningless.
   Stop and file it. On USB the reading is SYS ≈ 4.5 V (HARDWARE_TESTING.md:119), so the web greys samples with
   `battMv ≥ 4300` and the fit drops them.
1. Charge until the bq25185 LED goes out, unplug, and note the time. Run the release build, unattended, with the owner's
   normal paging. Days 1–2 stay idle (no input), so the baseline terms carry the bins.
2. Run until the pager dies: the 6092 BUVLO cuts at 3.0 V and the relay sees the LWT `offline`. Do not stop at 3550:
   the capacity left below 3550 under load is unknown, and the fit needs both ends.
3. `tools/battfit.py <csv>` (backend task) does the following:
   - bins samples into 6 h blocks;
   - assigns ΔSoC per bin from a generic LiPo OCV table;
   - adds the hard row Σ modelled mAh = 2375 mAh (2500 × 0.95, UNVERIFIED cell age);
   - solves ridge least squares toward the priors (σ = 50 %).
   A term whose posterior σ is still > 25 %, or whose exposure is < 2 % of the run, keeps prior × k, where
   k = 2375 / Σ prior mAh. In practice a single discharge pins **k plus I_aw(timer) and E_re**, and the rest scale.
4. A super PUTs the JSON to `/api/admin/battery-models/walter-6092-lipo2500`, and the web label changes to "model, fitted <date>".

**At the floor today**: below 3550 mV, GNSS attempts are refused (`loc_battery_ok`, loc.c:48, LOC_BATTERY_FLOOR_MV),
so location falls back to cell fixes, and the battery icon shows 0 segments (ui.c:268). OTA refuses below 3600
(OTA_DESIGN D6). **There is no shutdown, no low-battery alert and no extra publish.** The pager runs until the BUVLO
cutoff at 3.0 V.

## 5. Costs

- **RTC**: +92 B RTC_NOINIT. `pager_rtc_t` is ≈ 520 B and watchdog NOINIT ≈ 40 B, so about 650 of the 1184 B are used.
- **Flash/NVS**: none. **Code**: < 2 KB.
- **CPU**: one `esp_timer_get_time()`, ~20 adds and a CRC32 over 80 B (< 5 µs) per iteration. At most 86,400 iterations a
  day even at the 1 s attentive cadence, so < 0.5 s of CPU a day: negligible.
- **`/status` growth**: +56–64 B CBOR. The 2-byte key plus a 12-entry map: sq 3, dt 3, sl 3, aw 13, rf 5, md 6, the rest 1–3,
  keys 12. The *estimated* signed status today is ≈ 160 B, ≈ 216 B with the OTA fields (worst case 243 B with
  `stallcmd`), and ≈ 300 B with `bs`. **modes.c:941 `uint8_t buf[256]` would overflow, so it becomes 384.**
- **Data**: 34 statuses/day nominal ⇒ 2.0 kB/day ⇒ **61 KB/month**. 60/day pessimistic ⇒ **108 KB/month**. That is
  3.6 % of the 3 MB headroom under the 10 MB bar (OTA_DESIGN §4: pessimistic 7.10 MB) and 0.1 % of the 100 MB cap.
  It adds no radio session, because it rides publishes that already happen.
- **Relay**: ≤ 60 docs/device/day × 90 days ≈ 5,400 docs of about 400 B, with Firestore cost ≈ 0.

## 6. Failure modes

| Failure | Behaviour |
|---|---|
| Airplane mode or long outage | Counters keep accumulating in RTC. `mvn` stays absent because the modem is in reset. The first `/status` after the reboot carries a window of hours or days. The web spreads each sample pro-rata over `[ts−dt, ts]`. |
| Panic or watchdog reset mid-window | The window survives (B2), except for the seconds between the crash and `battstat_init()`. `rst` in the same `/status` marks it. |
| Power-on, battery swap | The window is lost and `sq` restarts at 0. The session changes too, so doc ids stay unique. |
| ms counter overflow (49.7 days) | Saturating add, and `dt` caps at 2³²−1 ms. Only a multi-week airplane window can reach it. |
| `disp` stats reset by `sleeptest` (debug only) | A source counter that has gone backwards is taken as the delta itself. |
| QoS 1 redelivery | Same doc id, so it is overwritten. |

## 7. What to measure (owner bench; agents do not flash)

1. Run the M15 gate (§4 step 0).
2. Debug console `battstat` (firmware task) during a 1 h soak: check `dt` ≈ `sl + Σaw` within 1 s, and per-wake awake
   ms = `aw.timer / ns`. That gives the real value behind the 0.35 s estimate.
3. Run `battstat`, then `battstat reboot` (calls `watchdog_hard_reset()`), then
   `battstat` again. `sq` should be unchanged and `dt` should keep growing. If not, B2 is wrong and each reset loses the window.
4. Trigger one web location request: `md.gnss` should go up by about the attempt length (≤ 20 s), while `aw` should not.
5. Do the full discharge (§4), once.
