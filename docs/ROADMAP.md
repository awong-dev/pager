## Released

**v0.1** — first end-to-end delivery.

**v0.2** — bi-directional messaging web ↔ pager on real hardware (CardKB, replies publish <0.5 s); host liveness ping, locate answer, cell-tower location, message history, watchdogs.

**alpha** — first stable beta-candidate state (24 Sep 2026).

**beta** — fast wake (tap to "password:" <0.5 s, partial first frame), working relock, datasheet display power-on; rail-gating (3V3 off outside 120 s attentive window, re-init on wake); owner-verified 25 Sep 2026. Implementation in `rail.c` / `rail.h`; tagged `beta`.

## 1.0.0 (planned)

### OTA updates, rollback, and factory slot

**Core design agreed; firmware-architect review + owner approval needed before build.**

Facts:
- Release firmware 663 KB; flash 16 MB with `ota_0` and `ota_1` at 2 MB each (0x120000 and 0x320000);
  ~10.7 MB unused after msghist (ends ~0x540000).
- No `esp_ota_*` or `esp_https_ota` code yet.

Plan:
- OTA download, verify and switch code.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` with app marking itself valid after health check.
- 2 MB `factory` app partition (appended to partitions.csv, preserving existing offsets; existing
  pagers need partition-table + factory-image write only).
- Factory reset: erase otadata (held by `CONFIG_BOOTLOADER_FACTORY_RESET` on button hold at power-on),
  optionally wipe chosen data partitions.
- Bench workflow change: `idf.py flash` targets factory slot once it exists; dev flash to OTA slots
  requires explicit target or erasing otadata.
- Frozen factory image must stay able to connect and pull current firmware.

### Firmware

- **Composer overflow:** single-line tail-scroll, counter only from 120 chars (v0.3 task 1.0-1.4,
  `docs/DEVICE_PLAN.md` 822-840).
- **Debug console multi-char input:** `key <text>` produced no redraw on bench (v0.3 task 1.0,
  `build/bench-logs/phaseG.log`).
- **CardKB loses keys in first ~1.1 s after rail-on** (measured 1136–1137 ms to first I2C ACK; guard
  now 1300 ms in ui.c via `rail_off()` and `rail_on()` calls in modes.c). Option: reflash CardKB
  ATmega8A without bootloader via ISP header. **Owner decision needed.**
- **Measure sleep current with new rail-off pull-downs** on RST/DC/CS/MOSI/SCLK (rail.c); power saving
  unmeasured.
- **Flightrec lives in PSRAM, lost on hard reset.** Consider RTC memory so dead-USB bench runs salvageable.
- **Set up again on device menu** is a stub; setup console-only (`docs/GOTCHAS.md`).
- **Received SMS persisted with `ts = 0`**, renders with no clock time, multipart as separate messages,
  boot-time scan one by one (`msg_insert_sms_in()`).
- **Location PSM-window radio route** not built; accelerometer thresholds datasheet defaults
  (`docs/DEVICE_PLAN.md` §location).
- **Modem library payload parser** miscounts by one byte on newline-end; symptom patched, cause not
  (`docs/VENDOR_BUG_REPORTS.md` item 7).
- **Temporary diagnostics and raw AT trace** now debug-build-only (`PAGER_DEBUG_NO_LIGHT_SLEEP`); release
  binary does not carry them.
- **SMS testing:** does the SIM carry SMS at all; texts from listed and unlisted numbers
  (`docs/HARDWARE_TESTING.md` item 7, task S4 in `docs/DEVICE_NEXT_TASKS.md`).
- **Accelerometer (LIS3DH):** wire and tune; resets location and no-coverage backoff
  (`docs/HARDWARE_TESTING.md` item 8, task A4 in `docs/DEVICE_NEXT_TASKS.md`; driver exists, chip not wired on bench).
- **Group chat `sndr` (G7)** and **SMS messages (S0-S5)** per `docs/DEVICE_NEXT_TASKS.md`.
- **Sleep phase 2 — modem-woken host (S9):** CTS asserted through sleep, `esp_sleep_enable_uart_wakeup()`,
  parser resync for clipped first line. **Ask owner about RI line (modem ring indicator) first** — if
  wired, simpler fix with no clipping risk (`docs/SLEEP_URC_TASKS.md` S9, `docs/SLEEP_URC_DESIGN.md` §8.7).

### Relay

- **Composer overflow gate:** message envelope limit fails on ten maximal-length contacts (pre-existing,
  found 23 Sep while adding `t:"grp"`; 707 bytes vs 640 cap). Either the cap, count, or field lengths
  must give.
- **No retention sweep for SMS audit log;** grows forever.
- **`ca_resolve.resolve_broker_ca()` never called at startup,** CA comes only from `BROKER_CA_PEM`.
- **No end-to-end scenario for a CA push.**
- **Per-device APN field:** API exists, no web UI; pager auto-detection makes it rarely needed.
- **Cell-tower location:** needs real `CELL_GEO_API_KEY` in production; `opencellid` provider support
  exists but shape is UNVERIFIED (`docs/V03_PLAN.md` §3, `docs/SERVER_PLAN.md` §10).
- **Real FCM push notifications (v0.3 item 3a):** relay FCM client (`NullFCMClient` today), service worker
  Firebase config (demo hardcoded), payload mismatch (sends `senderUid`, SW reads `senderAlias`), token
  hygiene (remove after 3 consecutive unregistered errors). See `docs/V03_PLAN.md` §3.1.
- **Geofences driven by serving-cell-change reports (v0.3 item 3c):** cell-tower location backend complete;
  geofence acceptance policy on relay pending `docs/SERVER_PLAN.md` §11 design.

### Web

- **No test runner.** Validation logic in pure modules for later testing.
- **Chat auto-scroll with new-messages chip** and mark-read gated on being at bottom (v0.3 task 2.1-2.4,
  `docs/V03_TASKS.md`).
- **PWA installability polish (v0.3 item 3b):** manifest (icons, maskable variant), install prompt handling,
  service worker (`onMessage` foreground, caching/fetch handler). See `docs/V03_PLAN.md` §3.2,
  `docs/SERVER_PLAN.md` §15.
- **WiFi phase 1 completion (W6-attempt-2, W8, W13, W15):** first WiFi session, fallback, no double session.
  WiFi panel and transport chip (W8). Heap margin ≥10 kB (W13 if needed). Catrust transport-blind (W15).
  See `docs/WIFI_TASKS.md`.

### Hardware tests

From `docs/HARDWARE_TESTING.md` "Not yet seen working," in order:
1. Confirm 480 s MQTT keepalive on AT&T for ≥1 hour idle (minimum overnight).
2. Bisect post-wake window between 50–200 ms; explain 136 s delivery; try bare `AT` probe.
3. No-coverage radio duty cycle, field-tested; sustained motion resets backoff.
4. End-to-end `/loc` cell answer: relay receives and resolves cell envelope, stores `src: "cell"`.
5. CA push from web app with right and wrong CA.
6. `gnsstest` outdoors: which radio route (in-place vs `CFUN=4` window), cold/hot fix times, re-attach time.
7. SMS: does SIM carry it; texts from listed and unlisted numbers.
8. Accelerometer and button on hardware.

### Measurements (unverified, driving design trade-offs)

- **Post-wake window minimum:** 50 ms fails, 150 ms works; minimum unknown. 136 s delivery latency
  unexplained. Bisect and explain (Hardware tests item 2).
- **Host liveness ping energy and carrier idle timeout:** per-ping mA and true carrier timeout unmeasured.
  300 s interval set at half the shortest observed death; measured 13 min timeout would allow 540 s and
  nearly halve the power cost. Measure and set by owner decision (SLEEP_URC_TASKS S14).
- **Battery budget current trace:** design assumed ~1% awake, 48 pings/day (43–50 mAh/day, ~20 days on 1500 mAh).
  Now 4% awake, 288 pings/day (95–107 mAh/day, ~14–16 days). Every term still an estimate. Current trace and
  wake-window shrink next (docs/PROTOCOL.md §8.4).

### Decisions waiting on the owner

- **CardKB bootloader reflash** (1.1 s key-loss issue; ISP header reflash without bootloader needed if selected).
- **Modem ring indicator (RI) wiring** (simplifies UART-woken host; if routed to GPIO, phase 2 becomes
  "wake on RI, one `AT` to flush" with no byte loss — decide before S9 implementation).
- **Soracom** (`docs/SORACOM_EVAL.md`). If adopted, pager TLS/CA unneeded on those SIMs; texting ordinary
  phone number impossible.
- **Send the three vendor bug reports** (`docs/VENDOR_BUG_REPORTS.md`).
- **eDRX vs delivery deadline** (S14): test with eDRX 10.24 s vs 20.48 s; impacts latency estimate and power
  cost trade-off (`docs/PROTOCOL.md` §8.2-8.3).

### GNSS: the remaining work, in order

The pager's GNSS answers location requests, but time-to-fix, indoor performance, and power cost against
the data budget are uncharacterized. Cell-tower location (above) gives coarse fix indoors; GNSS is an
accuracy improvement for outdoors, not the only source. Following this plan will unblock periodic location
and complete the feature.

1. **Outdoors, `gnsstest 40` to characterize the radio route.** Determine whether GNSS runs in-place while
   attached, or requires `CFUN=4` window (modem radio off, TLS session lost, full re-attach and re-handshake).
   Record cold-boot and hot-fix times, re-attach time if GNSS needs a dedicated window. Update
   `docs/HARDWARE_TESTING.md` with numbers for tuning.

2. **Try a GNSS fix inside a short PSM window.** Sequans forum thread 209 mentions GNSS can run in LTE
   "off" periods such as PSM. Measure PSM entry/exit latency and whether GNSS succeeds in the window.

3. **Tune the 20 s / 40 s attempt budgets and the 5 min to 12 h backoff** from measured numbers in step 1.

4. **Wire and tune the LIS3DH accelerometer** (driver exists; chip not wired on bench). Resets both location
   and no-coverage backoff.

5. **Measure assistance-data download size and power** against the data budget.

6. **Only then consider unsolicited periodic fixes.** Not needed for request-driven location or the
   school-pickup use case; they need a rewrite of `docs/PROTOCOL.md` §13.3.

## Ideas on hold

- **Background location.** Try for a fix when the pager moves or changes cell, not only when asked, and keep
  a warm last-known position. Parked: it spends power speculatively and needs a rewrite of
  `docs/PROTOCOL.md` §13.3.
- **Counter resync handshake.** A signed, challenged exchange in which the relay tells a pager the last
  counter it saw. Only needed if a pager loses flash but keeps its identity, which cannot happen today
  (`docs/DEVICE_PLAN.md` §2.5).
- **Encrypting message bodies** under the device key (`docs/DEVICE_PLAN.md` §2.2, option E). Would hide
  pages and positions from broker and path. A precondition for Soracom.
