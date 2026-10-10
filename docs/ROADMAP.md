## Released

**v0.1** — first end-to-end delivery.

**v0.2** — bi-directional messaging web ↔ pager on real hardware (CardKB, replies publish <0.5 s); host liveness ping, locate answer, cell-tower location, message history, watchdogs.

**alpha** — first stable beta-candidate state (24 Sep 2026).

**beta** — fast wake (tap to "password:" <0.5 s, partial first frame), working relock, datasheet display power-on; rail-gating (3V3 off outside 120 s attentive window, re-init on wake); owner-verified 25 Sep 2026. Implementation in `rail.c` / `rail.h`; tagged `beta`.
- **v1.0.0**: first stable release, 8 Oct 2026 (tag at `1547715`). Right-header rewire (LIS3DH on
  IO18/17/16), per-device GNSS disable, host partial LUT; relay per-user Twilio SMS (removed 9 Oct 2026, last at 05ec3ed) with
  held-until-approved inbound; web local-time battery charts. OTA with rollback is in this build
  (verified on proto3 7 Oct 2026).
- **v1.1.0**: Soracom bearer, 8 Oct 2026. A Soracom SIM selects the Beam bearer (plain MQTT to
  Beam, PAP APN auth). Direct and Beam both verified on the bench unit, with a one-hour relay-side
  watch on the direct bearer (`docs/SORACOM_DESIGN.md` §6).
- **Soracom enrollment from the admin UI**, 9 Oct 2026: `/api/admin/soracom/{sims,sims/{imsi}/enroll,enroll-all}`
  (`relay/app/soracom.py`, `docs/SORACOM_DESIGN.md` §8). Live run against the account pending; the
  `/subscribers` list fields are from the public spec and unverified live. Web UI page not yet built.

## Next

### OTA: factory slot and bootloader precondition

Download, verify, switch and rollback are done (`docs/OTA_DESIGN.md`; verified on proto3 7 Oct
2026). Still open:
- 2 MB `factory` app partition (appended to partitions.csv, preserving existing offsets; existing
  pagers need partition-table + factory-image write only). Not built.
- Factory reset: erase otadata (held by `CONFIG_BOOTLOADER_FACTORY_RESET` on button hold at power-on),
  optionally wipe chosen data partitions. The wake button was removed on 8 Oct 2026 (`a26e2f9`), so
  the trigger needs a new decision.
- Bench workflow change: `idf.py flash` targets factory slot once it exists; dev flash to OTA slots
  requires explicit target or erasing otadata.
- Frozen factory image must stay able to connect and pull current firmware.
- Each pager needs one USB flash of the rollback-capable bootloader before its first OTA
  (`docs/OTA_DESIGN.md` D8). `sora1` has it (full USB flash 8 Oct 2026; its bootloader matches
  `build/images/ota-bootloader.bin`). `rc1`: not recorded; confirm before its first OTA push.
- `otaErr: "bad"` stays in `/status` after a good update; the relay should clear it on dl/ok
  (status unverified, 8 Oct 2026).
- On-demand OTA deltas (`docs/OTA_DESIGN.md` D12, 9 Oct 2026): the relay builds and caches
  `fw-cache/<target16>/from-<base16>.dz` for any published (base, target) pair; the 7-day lifecycle
  rule is infra-dev's. Landed in `relay/`; needs a real push to verify (bucket write, Cloud Run
  image with the `detools` build, device fetch from `fw-cache/`).
- Orphan cleanup for the firmware bucket: a script the owner applies (status unverified, 8 Oct 2026).

### Firmware

- **Composer overflow:** single-line tail-scroll, counter only from 120 chars (v0.3 task 1.0-1.4,
  `docs/DEVICE_PLAN.md` 822-840).
- **Debug console multi-char input:** `key <text>` produced no redraw on bench (v0.3 task 1.0,
  `build/bench-logs/phaseG.log`) (status unverified, 8 Oct 2026; console `key` injection now drives
  the keyboard on the bench, see `docs/HARDWARE_TESTING.md`).
- **CardKB burst loss: done 9 Oct 2026, burst verification pending.** Keys were lost whenever the
  main loop stalled (wake status refresh, publish-quiet gate, modem servicing). The CardKB is now
  read by a `kbd` task, the input queue is 32 deep, and queue-full logs at WARN
  (`docs/TASK_kbtask.md`). Pending: the owner's `abcdefgh` burst test with a working keyboard in
  three scenarios (`firmware/README.md`, "Keyboard task"). The bench unit sora1's CardKB does not
  answer on I2C yet (`docs/GOTCHAS.md`).
- **Wake status refresh after the first draw** (open; `docs/TASK_kbtask.md` survey item 1). The
  refresh in `modes.c` (43 ms typical, 2 x 5 s worst case) runs before the first render. Move it to
  the pass after `ui_render()`, and show the cached battery and RSSI first.
- **kbd task wakes the main loop** (open, follow-up; survey item 5). Replace the 100 ms
  `vTaskDelay` in `modes.c` with `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))`, and have the task
  notify the modes task after `input_feed_key()`. Measure separately from the burst fix.
- **Measure modem servicing** (later; survey item 4). Add a `svc=` bucket to the debug `looptime`
  line before any change. Defer a non-urgent step only if a call goes over 300 ms while the UI is
  awake.
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
- **SMS (modem): dead on the US Mobile line.** The SIM carries no SMS over NAS (7 Oct 2026). Texting
  goes through the relay instead (`docs/RELAY_SMS_DESIGN.md`, shipped in v1.0.0). The modem SMS tasks
  (S0-S5 in `docs/DEVICE_NEXT_TASKS.md`) are superseded for this line.
- **Accelerometer (LIS3DH):** wired on the right header (v1.0.0 rewire) and driving shake-to-wake,
  calibrated on the bench 6 Oct 2026. Tuning for the location and no-coverage backoff resets is still
  open (`docs/HARDWARE_TESTING.md` item 8, task A4 in `docs/DEVICE_NEXT_TASKS.md`) (status unverified,
  8 Oct 2026).
- **Group chat `sndr` (G7):** parse committed (`ec8749f`); the author line on the real pager still
  needs the owner's glass check (`docs/GROUP_CHAT_DESIGN.md`).
- **Sleep phase 2 — modem-woken host (S9):** CTS asserted through sleep, `esp_sleep_enable_uart_wakeup()`,
  parser resync for clipped first line. **Ask owner about RI line (modem ring indicator) first** — if
  wired, simpler fix with no clipping risk (`docs/SLEEP_URC_TASKS.md` S9, `docs/SLEEP_URC_DESIGN.md` §8.7).

### Address book: add anyone (`docs/BOOK_ADD_ANYONE_DESIGN.md`, 10 Oct 2026)

- **Relay landed** (`6ca9a89`, 10 Oct 2026): the pager's Add creates the entry (and an `added` marker); a refused send to an added entry gets a named reply and one parent alert; Approve writes the edge; Remove clears the marker. `p[]` is retired. Protocol text is in `docs/PROTOCOL.md` §3.2 and §4.2.
- **Status (10 Oct 2026):** relay B1-B4 `6ca9a89`, web W1 `99153e7`, firmware F1 `2c377e3` (in v1.2.0, on rc1). The bench acceptance run in `docs/BOOK_ADD_ANYONE_TASKS.md` is still to do.

### Bridge phones (`docs/BRIDGE_PHONE_DESIGN.md`, 9 Oct 2026)

Built and landed: relay B1–B9 (`f9649f2`..`1bc22f4`), web W1–W5 (`7f3c4fc`..`927a11c`), Android
A1–A7 (`714b2c0`). WhatsApp (DMs and groups, both tiers) landed 9 Oct 2026 as A9, B11 and W7 (`8209141`). The relay suite passes (1303 tests) and `python3 tools/e2e_v2.py bridge
bridge_voice` passes; both were re-run on 9 Oct 2026. Not re-run in that check: the web verify
commands and `./gradlew`. The Android unit-test count (23) is as the commit message reports it.

WhatsApp DMs are keyed by conversation id (LID or phone JID) since 10 Oct 2026 (`fc2bf94`, `docs/BRIDGE_WHATSAPP_LID_DESIGN.md`). The first live DM had been dropped as `dropped_bad_from`. Relay side is landed. Android side landed `3b8bf44` (A10: LID warning removed, `wa.me` link for phone JIDs); tooling landed with B13 step 5 (`bridge_sim` `lid=True`, `bridge_whatsapp` e2e rewritten); the end-to-end WhatsApp retest is pending the owner.

Open:
- **The Android app has never run on a phone.** No device was attached. The bench steps are in
  `bridge-android/README.md`. Until they run, the notification shapes, the conversation-id rule,
  the reply cache and the tier-2 selectors in `Targets.kt` are assumptions.
- **Firebase Android registration (owner step).** Create the Android app `app.kidpager.bridge` in
  the pager's Firebase project and save its `google-services.json` as `bridge-android/app/`. Until
  then the build is poll-only (`BuildConfig.FCM = false`), with outbound latency up to 60 s.
- **WhatsApp bench verification.** Unverified on a phone: the tier-2 selectors (`entry`, `Send`, `menuitem_search`,
  the conversation row ids), the JID shortcut ids (`@s.whatsapp.net`, `@g.us`), the media placeholders, and group
  search by title. Until these run on the bench, WhatsApp tier 2 is best-effort.
- **Voice sender number.** The Voice number is typed on the setup screen; no detection is built.
  The Voice sender number comes from the notification's ids, sender line or title (O4). Which of
  these Voice fills is unknown until a text is seen on hardware.
- **MMS media.** Attachments are reported by kind (`[photo]`); no media is forwarded. The app does
  not send M-NotifyResp or M-Acknowledge, so a carrier may redeliver.
- **Multi-account Chat.** One Google account per phone.
- **Dual SIM.** The relay never sends an outbox item's `sim`, so sends use the default subscription.
- **Conversation ids.** A notification's `shortcutId` and an inspect link's id may differ. If they
  do, a conversation subscribed by link never matches its later messages.
- **Reboot with a screen lock.** The boot receiver listens for `BOOT_COMPLETED` only, with no
  direct-boot handling. The mitigation is the checklist's screen lock None (`docs/GOTCHAS.md`).
- **Heartbeat gap.** The Devices row turns red after 15 min. There is no push alert for it.

### Relay

- **Composer overflow gate:** message envelope limit fails on ten maximal-length contacts (pre-existing,
  found 23 Sep while adding `t:"grp"`; 707 bytes vs 640 cap). Either the cap, count, or field lengths
  must give.
- **No retention sweep for SMS audit log;** grows forever.
- **Alerts retention sweep** (`alertsDays` setting, default 90 days): delete handled/dismissed alerts older
  than the configured period in the weekly sweep — verify the setting and sweep mechanics.
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
  `docs/V03_TASKS.md`) (status unverified, 8 Oct 2026).
- **PWA installability polish (v0.3 item 3b):** manifest (icons, maskable variant), install prompt handling,
  service worker (`onMessage` foreground, caching/fetch handler). See `docs/V03_PLAN.md` §3.2,
  `docs/SERVER_PLAN.md` §15 (status unverified, 8 Oct 2026).
- **WiFi phase 1 completion:** the WiFi transport (W5), the device WiFi API and the web WiFi panel with
  transport chip are committed (`c33a07f`, `7aad6ae`, `0ac1bf0`). Still open: first WiFi session on
  hardware (W6-attempt-2), no double session, heap margin ≥10 kB (W13 if needed), catrust
  transport-blind (W15). See `docs/WIFI_TASKS.md` (status unverified, 8 Oct 2026).

### Hardware tests

From `docs/HARDWARE_TESTING.md` "Not yet seen working," in order:
1. Confirm 480 s MQTT keepalive on AT&T for ≥1 hour idle (minimum overnight). (status unverified, 8 Oct
   2026: sora1 on AT&T roaming had no reconnects in its one-hour relay-side watch, `docs/SORACOM_DESIGN.md`
   §6 D, but that is not the same test.)
2. Bisect post-wake window between 50–200 ms; explain 136 s delivery; try bare `AT` probe.
3. No-coverage radio duty cycle, field-tested; sustained motion resets backoff.
4. End-to-end `/loc` cell answer: relay receives and resolves cell envelope, stores `src: "cell"`.
5. CA push from web app with right and wrong CA.
6. `gnsstest` outdoors: which radio route (in-place vs `CFUN=4` window), cold/hot fix times, re-attach time.
7. ~~SMS: does SIM carry it; texts from listed and unlisted numbers.~~ *(7 Oct 2026: the US Mobile SIM
   carries no SMS over NAS; relay SMS replaces the modem path, `docs/RELAY_SMS_DESIGN.md`.)*
8. ~~Accelerometer and button on hardware.~~ *(8 Oct 2026: the wake button was removed, `a26e2f9`; the
   accelerometer is wired on the right header and drives shake-to-wake, calibrated 6 Oct 2026. Field
   tuning still open.)*
9. Power board bring-up: SYS to Walter VIN, 3V to Friend/LIS3DH, CardKB gated by IO0, button to 3V, sleep current with the green LED removed.

### Measurements (unverified, driving design trade-offs)

- **Post-wake window minimum:** 50 ms fails, 150 ms works; minimum unknown. 136 s delivery latency
  unexplained. Bisect and explain (Hardware tests item 2).
- **Host liveness ping energy and carrier idle timeout:** per-ping mA and true carrier timeout unmeasured.
  300 s interval set at half the shortest observed death; measured 13 min timeout would allow 540 s and
  nearly halve the power cost. Measure and set by owner decision (SLEEP_URC_TASKS S14).
- **Battery budget current trace:** design assumed ~1% awake, 48 pings/day (43–50 mAh/day, ~34 days on 1500 mAh LiFePO4).
  Now 4% awake, 288 pings/day (95–107 mAh/day, ~23–26 days on 2500 mAh LiPo). Every term still an estimate. Current trace and
  wake-window shrink next (docs/PROTOCOL.md §8.4).

### Decisions waiting on the owner

**Multi-family (per `docs/FAMILIES_DESIGN.md` §9)**
- ~~**Per-family Twilio number** (decision 10): without it, unrecognised inbound SMS cannot be attributed to a
  family unless it carries `@alias`; fallback is super-only alert. Cost ≈ $1/month per family.~~ *(7 Oct 2026: moot — no relay SMS)* **→ *(8 Oct 2026: per-user number done; see docs/RELAY_SMS_DESIGN.md)* → *(9 Oct 2026: Twilio removed)***
- **`any_sms` outbound** (decision 2): read literally as "numbers only, no people", or "approved people plus
  any number" (`people_anysms` table row)?
- **Super and `/locate`** (decision 3): super reads locations everywhere; should super also request a fix via
  `/locate` (currently requires an edge)?
- **Cross-family edges** (decision 4): super-only in v1; family-admin request/accept flow is the natural v1.1.
- **External nicknames** (decision 5): per-family nickname if two families know the same number by different
  names (small addition)?
- **Pager modem `any_sms`** (decision 11): device firmware cannot honour `any_sms` inbound (texts from unlisted
  numbers still blocked on-device); `cfg.sms` mode flag would be needed. Modem SMS is dead on the US Mobile
  line, and relay SMS holds unknown senders for approval (`docs/RELAY_SMS_DESIGN.md`) (status unverified,
  8 Oct 2026).

**Bridge phones (`docs/BRIDGE_PHONE_DESIGN.md`)**
- **Twilio removal.** Done 9 Oct 2026. Twilio relay SMS was last present at commit 05ec3ed
  (`05ec3ed703cf27c368cb4713d03ea3f25c8ac300`, 9 Oct 2026). It was added at a30ebca (8 Oct 2026),
  consent/keywords at 13c4a4b, and had been removed once before at 123efa4 (7 Oct 2026). Review the
  removed code with `git show 05ec3ed:<path>` or `git diff 05ec3ed main -- <path>`. SMS is now
  bridge-only; a non-bridge number fails `no_bridge`.

**Device/firmware**
- **CardKB bootloader reflash** (1.1 s key-loss issue; ISP header reflash without bootloader needed if selected).
- **Modem ring indicator (RI) wiring** (simplifies UART-woken host; if routed to GPIO, phase 2 becomes
  "wake on RI, one `AT` to flush" with no byte loss — decide before S9 implementation).
- **Soracom Beam bearer:** decided and verified on the bench 8 Oct 2026 (`docs/SORACOM_DESIGN.md` §6; the
  eval is `docs/SORACOM_EVAL.md`). Open precondition before trusting the Beam bearer with real page bodies:
  AEAD bodies (see Ideas on hold). Beam can read bodies today. The broker-free UDP design remains the
  long-term option (eval, step 3).
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

4. **Tune the LIS3DH accelerometer** (wired on the right header since the v1.0.0 rewire). Resets both location
   and no-coverage backoff (status unverified, 8 Oct 2026).

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
  pages and positions from broker and path. A precondition for trusting the Soracom Beam bearer with real data.
