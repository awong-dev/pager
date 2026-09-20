# Location on the pager (future work)

Status: **planned, nothing here is implemented.** Written 2026-09-20 after a location request from
the web app got no answer. The wire contract already exists and is normative: read `PROTOCOL.md`
§3.2 (`kind:"loc_req"`) and all of §13 first. This file does not restate them; it covers what the
firmware has to do to meet them on this hardware, and the order to build it in.

Do this **after** the sleep-mode delivery test (`DEVICE_PLAN.md` / `PROTOCOL.md` §8.3, M5: does the
modem hold its URCs while RTS is deasserted). That is still the largest unverified piece of the
core paging path, and location work changes the same attach and session code.

## 1. Where things stand

- **Relay and web app: built.** `relay/app/location.py` stores the request, mirrors the rate limit
  (§13.3 items 5-7) and publishes a signed `loc_req` on `/down`. Seen working 2026-09-20.
- **Broker: ready.** The rule forwards `pager/+/loc` to the relay; the device ACL allows the
  publish.
- **Firmware: nothing.** `on_incoming_message()` (`modes.c`) intercepts `cfg` and `book` and hands
  everything else to `msg_ingest_down_cbor()`, which accepts only `kind:"msg"`. A `loc_req` has no
  `body`, fails as malformed, and is dropped with a debug-level log line. There is no `/loc`
  publish and no call into the vendor library's GNSS API anywhere in `firmware/main`. §13's own
  text allows this ("simply has no location feature"), which is why nothing flagged it.

## 2. The hardware constraint that shapes the design

The Sequans GM02SP cannot run LTE and GNSS at the same time. The vendor's own `walter_feels` demo,
run on this exact unit, shows what one fix costs: set the modem to minimum function (which drops
the network), attempt a fix, and re-attach afterwards. In that log the first **four** attempts,
several minutes of searching, returned garbage positions with `confidence 20000000`; the fifth
succeeded with confidence 10.41 and 6 good satellites. **That log was taken inside a building
(owner, 2026-09-20), so the four failures say "indoors", not "fixes take minutes";** do not size
anything from it. Assistance data (almanac and real-time
ephemeris) has to be downloaded **over LTE, before** detaching.

So every real fix means:

1. the MQTT session is torn down and **the pager cannot receive pages** until it is back;
2. a fix attempt bounded by §13.3 item 2's **60 s**;
3. re-attach, TLS handshake and reconnect (several seconds and about 5 kB), then the `/loc`
   publish, and the relay re-publishes anything unacked on the online edge (§5.3).

Indoors the honest expected result is `err:"no_fix"` after 60 s offline.

**Sequans' own answer (forum thread 209, Sequans staff, 2025-09-23)** confirms the constraint and
offers a cheaper way to live with it: GNSS and LTE are non-concurrent, *but* the application does
not have to detach. "You can use the 'off' periods of LTE-M (e.g. while in PSM or in CFUN=4) to
handle the GNSS fix", provided the off period is long enough. The person asking reports **hot
fixes in a few seconds**, budgets 1 minute for a cold fix, and refreshes ephemeris every 2 hours.
<https://forum.sequans.com/t/can-i-use-gnss-positioning-while-maintaining-lte-m-or-nb-iot-connectivity-simultaneously/209>

What that changes:

- **A PSM window instead of a detach.** In PSM the modem's radio is off but it stays *registered*:
  no re-attach, the PDP context and IP address are kept, and leaving PSM is a service request, not
  an attach. If the modem's TCP socket also survives, the MQTT session survives too, and a fix
  costs no TLS handshake (about 5 kB and several seconds saved per fix). The vendor demo's
  `CFUN` minimum-function route, which §2's numbers above come from, is the expensive fallback.
- This firmware disables PSM on purpose (`net.cpp`, `PROTOCOL.md` §6.3: PSM suspends paging, and a
  pager must be pageable). The idea is a **deliberate, short PSM window only while fixing**:
  request PSM with a minimal active timer, take the fix, send any uplink to leave PSM, disable PSM
  again. The pager is unreachable only for the length of the attempt.
- `UNVERIFIED`, all of it, and each needs a hardware test (task L2): whether the carrier grants
  the requested timers; how fast PSM can be entered and left on demand; whether the socket and the
  MQTT session really survive (the broker keepalive is 1800 s, so it should not notice); what
  happens to a page sent during the window (TCP retransmission should cover a window of tens of
  seconds); and whether the thread's unasked question has a better answer still: **the eDRX idle
  gap**. With a 20.48 s cycle and a 2.56 s paging window the radio is already off for about 18 s
  in every cycle, long enough for a hot fix, and using it would cost *zero* reachability. The
  thread does not say the modem allows a fix there. It is the first thing to try because it is the
  best outcome.

## 3. Design

- **Dispatch.** Intercept `kind:"loc_req"` in `on_incoming_message()` next to `cfg` and `book`,
  after signature verification and before `msg_ingest_down_cbor()`. It is not a thread entry, is
  never `shown`/`read`-acked (§13.4), and is answered regardless of lock state
  (`DEVICE_PLAN.md` §5.8: "the lock is about the screen").
- **Rate limit first (§13.3 item 1).** Keep the last fix and the time of the last *attempt* in RTC
  memory. A request inside `loc_min_s` (120 s) is answered at once with `cached:true` and never
  touches the radio. This is the cheap, common path and should be built and tested first.
- **Failing is fine; draining the battery is not (owner, 2026-09-20).** Indoors is the *likely*
  case for a school pager, and indoors no attempt succeeds however long it runs. So:
  - **Keep each attempt short.** §13.3 item 2's 60 s is a ceiling, not a target. Start at
    10-15 s and tune it in L2. A wrong or missing fix is an acceptable answer; a minute of radio
    time spent proving "still indoors" is not.
  - **Always back off.** After a failed attempt the next one is not allowed for a growing
    interval: double it up to a ceiling, reset it on a success. Requests arriving inside the
    backoff are answered immediately from the last fix (`cached:true`) or with `no_fix`, without
    powering GNSS. This is §13.3 item 1's "window runs from the last *attempt*, not the last
    success" rule with a growing window instead of a fixed 120 s, so a parent pressing the button
    repeatedly cannot drain a pager that is sitting in a classroom.
  - Attempts stay **request-driven**. Nothing fixes speculatively in the background.
- **Hot fixes are what make short attempts work.** A few seconds only succeeds with valid
  ephemeris, so refresh assistance data over LTE before an attempt when it is due. Its download
  size must be measured against `PROTOCOL.md` §7.3's data budget.
- **One attempt in flight (§13.3 item 3).** Requests arriving during an attempt are queued by
  `req` id and each gets its own `/loc` with the shared result.
- **Always answer (§13.3 item 2).** Timeout or error publishes `loc:null, err:"no_fix"`. Silence is
  reserved for firmware without the feature.
- **Assistance.** Before detaching, check `gnssGetAssistanceStatus()` and call
  `gnssUpdateAssistance()` for whatever is due. Skipping this is what makes fixes take minutes.
- **Accept only a good fix.** Use the library's confidence value with a threshold (the vendor demo
  uses 100) and map it to `loc.acc`; never publish the `20000000` garbage results.
- **Survive the detour.** The session loss is deliberate, so it must not trip `handle_mqtt_loss()`'s
  backoff or the F4 modem-health recovery. Queued acks and an unsent reply must survive it (they
  already live in RTC/NVS).
- **Report the choices.** `/status` carries `loc_min_s` and `loc_period_s` (§13.3 item 4, §5.1).
  Periodic fixes (`req:null`, §13.5) stay **off** (`loc_period_s = 0`) until the on-demand path has
  been measured; they are the expensive part of §7.3's data and power budget.
- **Small `PROTOCOL.md` §13.3 touch.** A shorter attempt is already conformant (60 s is a bound).
  The growing backoff is stricter than item 1's fixed `loc_min_s`, so say so there and report the
  current value in `/status`, so the relay and web app do not show a phantom "expired".
- **Tell the person.** A small status-bar indication while locating, since the pager is
  unreachable for up to a minute. Whether the *child* should see that they are being located is an
  owner decision (open question 3).

## 4. Open questions for the owner

1. **How long may one attempt keep the pager unreachable?** 10-15 s is the starting point
   (owner, 2026-09-20: short, with backoff, and a failed fix is acceptable). If the eDRX-gap or
   PSM-window route works (§2), an attempt may cost no reachability at all and this matters less.
2. **Indoors it will mostly fail.** Is a `cell`-sourced coarse position (`loc.src:"cell"`, already
   in the schema) wanted as the fallback? That needs a cell-location lookup service on the relay
   side and is a separate piece of work.
3. **Should the pager show that it is being located?**
4. **Should the first version ship the `cached`-only path plus `no_fix`,** so the web app stops
   waiting 15 minutes for `expired`, before any GNSS work lands? Task L1 alone delivers that.

## 5. Tasks

Same shape as `DEVICE_TASKS.md`. Dependency graph: `L1 → T1`, `L1 → L2 → L3 → L4 → L5`.

### L1 Recognise `loc_req`, answer without GNSS
- Read: `PROTOCOL.md` §3.2, §13.2-13.4, §10 keymap; `modes.c` `on_incoming_message()`;
  `lock.c`'s `lock_ingest_cfg_cbor()` as the pattern.
- Files: new `firmware/main/loc.c`/`loc.h`, `modes.c`, `main/CMakeLists.txt`, `host/test_loc.c`,
  `host/Makefile`.
- Do: parse the request, build and sign the `/loc` envelope (CBOR, `l_` id), answer
  `loc:null, err:"no_fix"` every time for now. QoS 1 because `req` is non-null.
- Verify: `make -C firmware/host test`; on hardware, a locate from the web app resolves within
  seconds as "no fix" instead of sitting until `expired`, and the relay logs no `malformed`.

### T1 Test client parity
- Files: `tools/pager_client.py`, relay e2e scenario.
- Do: make the Python test client answer `loc_req` the same way, so the e2e suite covers the
  request lifecycle without hardware.
- Verify: the e2e `locate` scenario passes against the compose stack.

### L2 GNSS bring-up: find the cheapest window (console command)
- Read: this file §2; the vendor `examples/positioning`; the `walter_feels` log in
  `good-connect.txt`; `net.cpp`'s eDRX/PSM setup.
- Files: `net.cpp`, `net.h`, `main.c` (a temporary `gnsstest <mode> <seconds>` command, like
  `nettest`).
- Do: with assistance data fresh and **outdoors**, take a fix three ways and log time to fix,
  confidence, satellites, and what happened to the MQTT session:
  1. **eDRX gap:** stay attached and connected, ask for a fix, see whether the modem allows it.
  2. **PSM window:** request PSM with a minimal active timer, fix, leave PSM with an uplink,
     disable PSM. Check the granted timers, that the session survived, and that a page sent
     during the window arrives afterwards.
  3. **`CFUN` minimum function:** the vendor demo's route, as the known-good baseline.
  Then repeat each with a 10-15 s cap, indoors and outdoors, to size a short attempt.
- Verify: a table of real numbers recorded in this file. It picks the route and the attempt length,
  and answers open question 1.

### L3 Attempt, backoff, answer
- Depends on: L2.
- Files: `loc.c`, `modes.c`, `net.cpp`, `docs/PROTOCOL.md` §13.3 (the backoff note, reviewed
  server-side like any protocol edit).
- Do: one short attempt over the route L2 picked; last fix, last-attempt time and the current
  backoff in RTC memory; backoff doubling to a ceiling and resetting on success; `cached:true` /
  `no_fix` answers inside the backoff without powering GNSS; assistance refresh when due; queued
  `req` ids; deliberate-session-loss handling if the route drops the session.
- Verify: host tests for the backoff with a fake clock. On hardware indoors: repeated locates
  produce one short attempt, then immediate answers while the backoff grows. Outdoors: a fix,
  and the backoff resets.

### L4 Status fields and indicator
- Files: `modes.c` (`/status`), `ui.c` status bar, assets if a glyph is added.
- Verify: `/status` shows `loc_min_s`/`loc_period_s`; the relay and web app display them.

### L5 Measure, then decide on periodic fixes
- Do: measure power and data for an on-demand fix; compare with `PROTOCOL.md` §7.3's budget; only
  then consider a non-zero `loc_period_s`.

## 6. Definition of done

A locate from the web app is always answered: a real position when the pager can see the sky,
`no_fix` or the last cached fix (with its true age) when it cannot. No attempt runs longer than the
figure agreed in open question 1, failures back off, and repeated requests to an indoor pager cost
almost nothing. Pages sent during an attempt arrive after it.

## 7. For later: a background fix scheduler

*(Parked, 2026-09-20. Not part of the work above; do not build it without a new decision.)* An
idea that came out of the same discussion: decouple fixing from requests. A scheduler would make
short tries on its own when something suggests a fix is newly possible or the answer has changed
(accelerometer motion on the LIS3DH, IO2, wired but with no driver yet; a change of serving cell,
which the modem already reports in `+CEREG`), keep a last known fix warm so every request is
answered instantly, back off while tries fail, and optionally publish unsolicited fixes
(`req:null`, §13.5). It suits a child who moves in and out of buildings all day. It is parked
because it spends power speculatively, needs an accelerometer driver, and would need a real
rewrite of `PROTOCOL.md` §13.3 (what an "attempt" is, when an answer is `cached`, what `no_fix`
means) agreed across firmware, relay and web app. Revisit once L5 has real power and data numbers
for a single on-demand fix.
