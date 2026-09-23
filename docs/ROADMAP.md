## v0.3 (planned)

Details in `docs/V03_PLAN.md` (decisions) and `docs/V03_TASKS.md` (agent-runnable tasks, build order
2 → 1 → 3a → 3b → 3c).

1. Composer overflow on device — single-line tail-scroll, counter only from 120 chars.
2. Web chat auto-scroll with a "New messages" chip and mark-read gated on being at the bottom.
3. Real push (the relay's FCM client is a `NullFCMClient` today and the SW ships demo config),
   installability polish, then geofences driven by serving-cell-change reports with accuracy-aware
   hysteresis, gated on a bench check of cell changes and on the geo provider.

# Unfinished and unverified

Hardware tests still to run are listed in `HARDWARE_TESTING.md`. This is everything else.

## Design needed: input and display power gating (owner, 23 Sep 2026)

The CardKB is polled over I2C every 10 ms while the UI is awake and every wake cycle while asleep,
and the e-paper panel stays powered between refreshes. Two ways to take both off the idle budget,
to be designed and decided:

1. **An alternate CardKB 1.1 firmware that is power-saving aware**: the keyboard's own MCU sleeps,
   raises a wake line on a keypress (into the ESP32's `ext1` mask next to the LIS3DH), and only
   then is polled.
2. **Explicit power gating**: cut VCC to both the e-ink (already gated via IO15) and the keyboard
   until the wake button is pressed; re-init both on wake. Interacts with the 23 Sep finding that
   the panel controller loses its registers under LTE bursts (a re-init before every refresh now
   covers that).

Either way the wake button becomes the only always-on input. Power numbers first, then pick.

**Owner, 23 Sep morning:** both the CardKB and the e-ink are on the board's `3v3_en` rail, which
firmware already switches (the panel VCC gate on IO15, `disp_power_on()`), so option 2 needs no
hardware change: cut the rail on sleep entry, re-enable on the wake button (`ext0`) or on a page
arrival, re-init the panel (RAM lost, so the first refresh is a full one — `disp_init()`'s
priming already forces it; the glass keeps its last image unpowered) and re-probe the keyboard.
Costs one full refresh per wake that draws. `disp_power_off()` was removed as dead code on
23 Sep (`3a3c969`); revive it from that commit when this is built.

## Decisions waiting on the owner

- **Soracom** (`SORACOM_EVAL.md`). If adopted, the pager's TLS and CA handling become unnecessary
  on those SIMs, and texting an ordinary phone number from the pager becomes impossible.
- Whether to send the three vendor bug reports (`VENDOR_BUG_REPORTS.md`).

## Known gaps

Unverified measurements (see HARDWARE_TESTING.md for test plans):

- **Post-wake window:** only 50 ms (fails) and 150 ms (works) were tried; the minimum between
  them is unknown, and 200 ms is in use as a margin. The 136 s delivery latency has no explanation.
- **Host liveness ping:** specified (`V02_DESIGN.md` §9) after the bench showed the modem sends
  no PINGREQ; not yet verified on hardware. The per-ping energy and the carrier's true idle
  timeout are unmeasured. Both set the interval: 300 s is half the shortest observed death;
  a measured 13 min timeout would allow 540 s and nearly halve the cost. The packet type does
  not matter (the RRC connection dominates, not the bytes), so keep the re-SUBSCRIBE: its SUBACK
  proves the link and repairs the subscription. Check whether the modem exposes LTE-M Release
  Assistance to shorten the radio tail after each ping; that is the other real lever.
- **No-coverage duty cycle:** implemented but not yet tested on hardware.
- **Cell-tower location:** the pager half sends `cell` on a `no_fix` answer (coded, untested on
  hardware); the relay half resolves it with Google Geolocation API or OpenCelliD. The OpenCelliD
  provider is unverified.
- **Battery budget:** the design assumed ~1% awake and 48 pings a day. It is now 4% awake (200 ms
  per 5 s wake) and 288 host pings a day, which moves `PROTOCOL.md` §8.4's estimate from 43–50 to
  95–107 mAh/day (about 14–16 days idle on 1500 mAh). Every term is still an estimate; a current
  trace is the next step, then shrinking the wake window.

Firmware
- "Set up again" on the device menu is a stub; setup is console-only.
- A received SMS is persisted to `msghist` like any other message, but with `ts = 0`
  (`msg_insert_sms_in()` sets it deliberately, since the row is never published), so it renders
  with no clock time and sorts only by `msghist` seq; multipart texts arrive as separate messages;
  the boot-time scan of stored texts reads slots one by one.
- Location: the PSM-window radio route (cheaper than dropping the radio) is not built;
  accelerometer thresholds are datasheet defaults.
- The payload parser in the modem library miscounts by one byte when a payload ends in a newline;
  the symptom is patched, the cause is not.
- The temporary diagnostics (`nettest`, `mqtttest`) and the raw AT trace are debug-build only
  (`PAGER_DEBUG_NO_LIGHT_SLEEP`) now; the release binary does not carry them (the AT trace is a
  log level the debug build raises in net.cpp's `net_bringup()`).

Relay
- `build_book`'s envelope-limit assertion already fails for ten contacts at maximal field lengths
  (16-char alias + 16-codepoint name each: 707 bytes signed vs the 640 cap), with or without group
  contacts (found 23 Sep while adding `t:"grp"`; pre-existing). Either the cap, the contact
  count, or the field lengths has to give.
- No retention sweep for the SMS audit log; it grows for ever.
- `ca_resolve.resolve_broker_ca()` is never called at startup, so the CA comes only from
  `BROKER_CA_PEM`.
- No end-to-end scenario for a CA push.
- The per-device APN field has an API but no web UI; the pager's own detection makes it rarely
  needed.
- Cell-tower location needs a real `CELL_GEO_API_KEY` to resolve anything in production. `opencellid`
  provider support exists but its request/response shape is `UNVERIFIED`.

Web
- No test runner. Validation logic is kept in pure modules so it can be tested later.

## GNSS: the remaining work, in order

The pager's GNSS answers location requests, but its time-to-fix and indoor performance are unknown,
and the power cost against the data budget is uncharacterized. Cell-tower location (above) gives a
coarse fix indoors; GNSS is an accuracy improvement for outdoors, not the only source. Following this
plan will unblock periodic location and complete the feature.

1. **Outdoors, `gnsstest 40` to characterize the radio route.** Determine whether GNSS runs in-place
   while attached, or requires the `CFUN=4` window (modem radio off, TLS session lost, full re-attach
   and re-handshake). Record cold-boot and hot-fix times, and the re-attach time if GNSS needs a
   dedicated window. Update `HARDWARE_TESTING.md` with the numbers for tuning.

2. **Try a GNSS fix inside a short PSM window.** Sequans forum thread 209 mentions GNSS can run in LTE
   "off" periods such as PSM. It might avoid the re-attach and the TLS handshake. Measure the PSM
   entry/exit latency and whether GNSS succeeds in the window.

3. **Tune the 20 s / 40 s attempt budgets and the 5 min to 12 h backoff** from the measured numbers in
   step 1.

4. **Wire and tune the LIS3DH accelerometer** (the driver exists; the chip was not wired on the bench).
   It resets both the location backoff and the no-coverage backoff.

5. **Measure assistance-data download size and power** against the data budget.

6. **Only then consider unsolicited periodic fixes.** They are not needed for request-driven location
   or for the school-pickup use case, and they need a rewrite of `PROTOCOL.md` §13.3.

## Ideas on hold

- **Background location.** Try for a fix when the pager moves or changes cell, not only when
  asked, and keep a warm last-known position. Parked: it spends power speculatively and needs a
  rewrite of `PROTOCOL.md` §13.3.
- **Counter resync handshake.** A signed, challenged exchange in which the relay tells a pager the
  last counter it saw. Only needed if a pager ever loses its flash but keeps its identity, which
  cannot happen today (`DEVICE_PLAN.md` §2.5).
- **Encrypting message bodies** under the device key (`DEVICE_PLAN.md` §2.2, option E). Would hide
  pages and positions from the broker and from the path. A precondition for Soracom.
