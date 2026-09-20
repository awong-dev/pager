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
succeeded with confidence 10.41 and 6 good satellites. Assistance data (almanac and real-time
ephemeris) has to be downloaded **over LTE, before** detaching.

So every real fix means:

1. the MQTT session is torn down and **the pager cannot receive pages** until it is back;
2. a fix attempt bounded by §13.3 item 2's **60 s**;
3. re-attach, TLS handshake and reconnect (several seconds and about 5 kB), then the `/loc`
   publish, and the relay re-publishes anything unacked on the online edge (§5.3).

Indoors the honest expected result is `err:"no_fix"` after 60 s offline.

## 3. Design

- **Dispatch.** Intercept `kind:"loc_req"` in `on_incoming_message()` next to `cfg` and `book`,
  after signature verification and before `msg_ingest_down_cbor()`. It is not a thread entry, is
  never `shown`/`read`-acked (§13.4), and is answered regardless of lock state
  (`DEVICE_PLAN.md` §5.8: "the lock is about the screen").
- **Rate limit first (§13.3 item 1).** Keep the last fix and the time of the last *attempt* in RTC
  memory. A request inside `loc_min_s` (120 s) is answered at once with `cached:true` and never
  touches the radio. This is the cheap, common path and should be built and tested first.
- **One attempt in flight (§13.3 item 3).** A small state machine owned by `modes_run()`'s task:
  `idle → assist → detach → fixing → reattach → publish`. Requests that arrive mid-attempt (there
  will be few: the pager is offline for most of it) are queued by `req` id, at most a handful, and
  each gets its own `/loc` with the shared result.
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
- **Tell the person.** A small status-bar indication while locating, since the pager is
  unreachable for up to a minute. Whether the *child* should see that they are being located is an
  owner decision (open question 3).

## 4. Open questions for the owner

1. **Is up to ~70 s of unreachability per locate acceptable?** A page sent in that window is not
   lost: the relay re-publishes it when the pager reconnects. It is delayed.
2. **Indoors it will mostly fail.** Is a `cell`-sourced coarse position (`loc.src:"cell"`, already
   in the schema) wanted as the fallback? That needs a cell-location lookup service on the relay
   side and is a separate piece of work.
3. **Should the pager show that it is being located?**
4. **Should the first version ship the `cached`-only path plus `no_fix`,** so the web app stops
   waiting 15 minutes for `expired`, before any GNSS work lands? Task L1 alone delivers that.

## 5. Tasks

Same shape as `DEVICE_TASKS.md`. Dependency graph: `L1 → L2 → L3 → L4`, `L1 → T1`, `L4 → L5`.

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

### L2 GNSS bring-up behind a console command
- Read: the vendor `examples/positioning`; `good-connect.txt`-style logs from `walter_feels`.
- Files: `net.cpp`, `net.h`, `main.c` (a temporary `gnsstest` command, like `nettest`).
- Do: assistance update, detach, one bounded fix, re-attach. Log time to fix, confidence and
  satellite counts. No protocol work.
- Verify: outdoors, a fix with confidence under the threshold; the log gives real numbers for
  time offline. Record them in this file; they decide open question 1.

### L3 The fix state machine
- Files: `loc.c`, `modes.c`, `net.cpp`.
- Do: §3's state machine, the 60 s bound, the 120 s rate limit with `cached:true`, the queued
  `req` ids, and the deliberate-session-loss handling.
- Verify: host tests for the state machine and rate limit; on hardware, two locates 30 s apart
  produce one radio detour and two `/loc` publishes, the second `cached:true`.

### L4 Status fields and indicator
- Files: `modes.c` (`/status`), `ui.c` status bar, assets if a glyph is added.
- Verify: `/status` shows `loc_min_s`/`loc_period_s`; the relay and web app display them.

### L5 Measure, then decide on periodic fixes
- Do: measure power and data for an on-demand fix; compare with `PROTOCOL.md` §7.3's budget; only
  then consider a non-zero `loc_period_s`.

## 6. Definition of done

A locate from the web app is answered every time: a real position outdoors, `no_fix` within about
70 s indoors, `cached:true` inside the rate-limit window. Pages sent during a fix arrive after it.
`PROTOCOL.md` §13 needs no change; if building this shows that it does, that is a finding to
raise, not something to work around in firmware.
