# Design — delivering a page URC through light sleep

Decisions and reasoning. The executable form is `docs/SLEEP_URC_TASKS.md`. Evidence:
`.overnight-handoff.md` (23 Sep 17:30/17:45/17:50), `docs/RCA_SLEEP_URC.md`,
`docs/RCA_SLEEP_PUBLISH.md`, `build/bench-logs/phaseAB-report.log`, `phaseAC-report4.log`,
`phaseAD-report.log`, `phase1-boot.log`. Fixed constraints: `firmware/README.md`,
`docs/PROTOCOL.md` (100 MB/month, ~30 s sleep-mode delivery, <5 s active, one persistent
TLS+MQTT session, MQTT client inside the modem).

## 1. Established, and one correction to the record

1. The GM02SP **queues** URCs while the host's CTS is deasserted and releases them when the host
   sends it a command. 200 ms window: 191-237 s late (phaseAB); 1 s window: never (phaseAC);
   bare `AT` on every wake: **10 s** (phaseAD). RTS is now genuinely held through sleep
   (`gpio_sleep_sel_dis`, ab5927c), so this is the interface, not the radio.
2. A flushed page URC arriving together with a SUBACK loses the SUBACK; the 30 s no-SUBACK bound
   (`xport_lte.cpp:580-601`) then declares the session dead (`rc=0`, self-inflicted) and the
   teardown costs 82 s (`mqtt status/retry max 81795 ms`, phaseAB).
3. One `/up` ack per window blocks 30 010 ms and is rescued by patch 1.12 (`datatx_retx=1`,
   `prompt_orphan=0`).
4. **Correction — "the probe cost 870 ms/wake" is a report artefact, not a measurement.**
   `ST_MARK_BEGIN()` runs immediately after `esp_light_sleep_start()` (`modes.c:1942`) and
   `ST_MARK(0)` after the yield (`modes.c:1980`), but on any iteration that does *not* sleep the
   span `ST_MARK(6)`→`ST_MARK(0)` (whole loop top + the 20/100 ms poll delay) is also charged to
   bucket 0, and the printed `avg` divides every bucket by `s_st_sleeps` (`modes.c:1138-1141`).
   That is why `avg 870 ms` sits next to `max 198 ms` — arithmetically impossible for one sample.
   phaseAD's real awake time is (360−150)/69 = 3.04 s per wake, and it is fully explained by the
   stalls the cycle list prints: 30 208 + 30 208 + 30 214 + 61 302 + 30 210 + 16 055 = 198 s, plus
   63 ordinary wakes × ~200 ms = 12.6 s → 210.6 s ≈ the 210 s awake. **The bare `AT` itself is
   invisible in the report.** What cost 41% asleep was ~6.6 thirty-second command timeouts.
5. Why the *crude* probe stalled: `run_modem_health_check()` (`modes.c:1524-1547`) is
   `net_check()` → `WalterModem::checkComm()` **blocking**, 3 attempts, 1 s apart, and on failure
   `rate_limited_modem_recover()` — a modem power cycle. Worst case 3 × 30 s + 2 s + a reset. And
   because the library serialises one command at a time, a blocking probe queued behind an already
   stalled command reports that stall as its own: one stuck command turns *every* later wake into
   a 30 s block. Health check is also gated on `mode == SLEEP` (`modes.c:2401`), so after a page
   (ACTIVE, 2 s cycle) it does not run at all — the probe must not inherit that gate.

## 2. Power model (assumptions explicit)

- `I_awake` = 40 mA (ESP32 active + modem registered, no RRC) — the figure `modes.c:1840` and
  `RCA_SLEEP_PUBLISH.md` §4 already use. **Assumption, not measured on this board.**
- `I_sleep` = 1.0 mA (light sleep, modem attached; vendor figure quoted in `net.cpp`).
- Today: 5 s cycle, 200 ms window = 4% duty → 0.04×40 + 0.96×1.0 = **2.56 mA**, plus
  V02_DESIGN §9.3's liveness pings 1.2 mA, plus UI/display ≈ 0.2-0.7 mA → 3.95-4.45 mA →
  **95-107 mAh/day** (V02_DESIGN §8/§9.3). 17 280 wakes/day.
- **One 30 s stall = 30 s × 40 mA = 0.33 mAh.** phaseAB's rate (3 per 15 min) is ~96 mAh/day of
  waste — it doubles the budget. phaseAD's rate (6.6 per 6 min) is ~530 mAh/day: a flat 1500 mAh
  battery in under three days. Stall elimination, not probe frugality, is the power story here.
- Delivery budget is additive and one term is untouchable: broker→modem ≤ eDRX 20.48 s
  (`AT+SQNEDRX=2,4,"0010","0001"`, PSM off — `phase1-boot.log:104,109`), modem→host ≤ one wake
  interval, then render+ack 1-4 s. Worst case ≈ 26 s against the ~30 s requirement; measured 10 s.
  Nothing below shortens the eDRX term.

## 3. The four options, costed

**(a) Minimal per-wake drain probe — one `AT`, fire-and-forget.** `checkComm(NULL, cb, args)` with
a non-NULL `cb` returns as soon as the command is queued (`WalterDefines.h:311-315`), so the modes
task never blocks. Issued as the first statement after `net_sleep()` returns, the modem's answer
(< 10 ms — `phaseAC-report4.log:1-60` shows `AT+CEREG?` TX and RX inside the same 1 ms log tick)
arrives *inside the existing 200 ms yield*: **+0 ms of window, +0 air bytes, < 0.01 mA**. Upper
bound if it ever had to extend the window to 250 ms: 5% duty → +0.4 mA → **+9.6 mAh/day**.
The probe must NOT: block, retry, carry `net_check()`'s registration semantics, touch
`rate_limited_modem_recover()`, be gated on `mode == SLEEP`, or queue a second probe while one is
outstanding (queue and pool are 8 slots, `WalterModem.h:127,3501`; probes would starve
`mqttConnect()` — exactly the `could not be queued` seen in `phaseAA-live.log`).

**(b) Explicit polling — `AT+SQNSMQTTRCVMESSAGE` per wake.** `mqttReceive(topic, 0, …)`
(`WalterMQTT.cpp:146-163`; `mqttDidRing()` is a deprecated wrapper for it). Cost: ~45 B of command
line (3.9 ms of wire) and a blocking wait for the payload → +20-30 ms awake per wake = 0.4-0.6%
duty → **+4-6 mAh/day**, zero air bytes. **UNVERIFIED and load-bearing:** whether the no-`mid`
form returns a QoS 1 message at all, what an empty queue answers (`OK` vs `+CME ERROR`) and how
fast, and whether a read is destructive. It also needs new plumbing: today topic+payload reach
`msg.c` only through the MESSAGE event (`xport_lte.cpp:324+`), so a polled read needs its own
ingest path. Not phase 1 for that reason, not for its power.

**(c) CTS asserted through sleep + `esp_sleep_enable_uart_wakeup()`.** The modem transmits only
when it has something, so there is no per-wake cost at all, and — the real prize — the timer
interval stops being the delivery bound: 200 ms every 30 s = 0.67% duty →
0.0067×40 + 0.993×1.0 = 1.26 mA, +1.2 mA pings ≈ **2.5 mA ≈ 60 mAh/day** (−35 to −45 mAh/day,
~25 days on 1500 mAh). Per page: one extra wake ≈ 0.003 mAh. Costs and unknowns: the leading bytes
of the waking line are lost while the chip resumes (~1 ms ≈ 11 byte-times at 115200), i.e. exactly
the `\r\n+SQNSM…` the parser delimits on, so the parser needs a resync rule (drop everything up to
the next CRLF, count it) **and** a content-recovery path, because a corrupted
`+SQNSMQTTONMESSAGE` line loses the topic and `mid` — that is (b). Also `CONFIG_ESP_SLEEP_GPIO_
RESET_WORKAROUND=y` sets `SLP_IE=0` on every pad, so the RX pad's input is off during sleep:
`gpio_sleep_sel_dis(RX)` is required as well (same class of bug as ab5927c) — **UNVERIFIED**
whether IDF 5.2.1 already exempts a UART-wake pin. Phase 2.

**(d) Modem-side URC buffering settings — UNVERIFIED, no Sequans manual in the repo.** Candidates
to try from the debug console, in order of plausibility: `AT&K?` / `AT&K0` (flow-control mode; &K0
= modem ignores CTS), `AT+IFC?` / `AT+IFC=0,0` (same in the 3GPP spelling), `AT&V` (dump the
active profile and read whatever flow-control field it names), `AT+SQNRICFG?` / `AT+SQNHWCFG?`
(is there a RI/interrupt output at all — even if yes, `firmware/main/pins.h` routes no modem→ESP
wake line, so that is an owner hardware call). Cost if one exists: zero. Five minutes on the
bench, no code — E2/E4 in §7.

## 4. Decision

**Phase 1 = (a), as an async probe, bundled with the stall fixes.** It is the only option that is
power-neutral, needs no new AT semantics, no parser patch and no hardware question answered, and it
is already the one configuration measured to deliver a page in 10 s with the session intact
(phaseAD). The bundle is not optional: the acceptance bar (asleep ≥ 85%) *cannot* be met with a
single 30 s stall in the window — after the first page the pager is ACTIVE (2 s cycle), so yields
alone are 200/2200 = 9.1% and two renders ≈ 7 s, giving a predicted **87-91% asleep**; one 30 s
stall is another 8.3% and fails the run. So S1 (probe), S2 (SUBACK collision), S3/S4 (ack stall)
and S6 (watchdog) ship together.

**Phase 2 = (c), with (b) as its content-recovery path**, gated on experiments E3 and E5. It is
where the 35-45 mAh/day lives, and it is also the only option that makes delivery independent of
the wake cadence. (d) is a five-minute lottery ticket that can retire (c)'s byte-loss problem
entirely; run it before spending firmware time on (c).

## 5. Phase-1 mechanism

**(1) Modem library calls.** Boot unchanged (`begin` → attach → TLS → `mqttConfig` →
`mqttConnect` → `mqttSubscribe`). Per wake, first statement after `net_sleep()` returns (flow
control restored by then, `net.cpp:1195-1207`): `WalterModem::checkComm(NULL, probe_cb, NULL)` —
one `AT`, queued and returned. Skipped when: a probe is still outstanding; `net_publish_in_flight()`
(a modem parked at a `>` prompt would eat `AT\r\n` as payload — RCA_SLEEP_PUBLISH §1); transport is
WiFi; the modem is not begun or is in a reset. `probe_cb` only sets a flag and a timestamp — no
logging, no AT calls, it runs on the library's own task. Then the existing 200 ms yield, during
which `_uartRxTask` parses the flush burst and the event task runs the unchanged
`+SQNSMQTTONMESSAGE` → `mqttReceive()` path. Then input/UI, `net_service_session()`, `msg_pump()`.
`run_modem_health_check()` keeps its every-60th-wake schedule and its retries; it is not the probe.

**(2) Wake sources and RTC state.** Unchanged: timer (`interval_ms`), ext0 = button level 0, ext1 =
LIS3DH INT1 ANY_HIGH. **No new wake source in phase 1.** Light sleep retains all RAM, so the probe
state (in-flight flag, timestamp, counters) is plain RAM — nothing new belongs in RTC memory
(`pager_rtc_t` is at 520 of 1184 bytes and does not need to grow). Evidence survives the
deliberate end-of-window reset through the existing NVS sleeptest report.

**(3) Failure modes and recovery.** (i) Probe never answered (its `OK` lost in the burst, or a
stalled command in front): the in-flight flag blocks further probes; after 3 wake intervals count
`probe_stuck` and clear the flag so probing resumes; 6 consecutive stuck probes hand off to the
existing F4 `rate_limited_modem_recover()` path — no new recovery machinery. (ii) `checkComm()`
returns false (could not be queued): count `probe_noqueue`, skip, no retry. (iii) Modem parked at a
`>` prompt: skipped by (ii)'s publish-in-flight gate plus patch 1.12's payload-on-timeout.
(iv) A probe answered but no page flushed: indistinguishable and harmless. (v) The probe delays a
real command by one queue slot: bounded, single-slot by construction.

**(4) What to measure.** In the sleeptest report: bucket 0 max ≈ yield + 10 ms (after S0 fixes the
accounting); `probe_issued / probe_answered / probe_stuck / probe_noqueue`; RCA_SLEEP_URC fix 1's
discriminator — **bytes read from the modem UART in the first 50 ms after each wake** (max, and the
number of wakes with > 0: near-zero normally, a burst on the wake after a page); page latency
lines; `MQTT session LOST` count = 0; `datatx_retx` / `prompt_orphan`; asleep %.

## 6. The four side-designs

**SUBACK collision (S2).** Two changes in `xport_lte.cpp`'s Step 5 branch (`:580-601`), host-side
only, zero power: (i) suppress the verdict when downlink traffic proves the session alive — if any
`/down` message was ingested after `s_resub_sent_us`, clear `s_resub_wait` and return without a
verdict (a page arriving *is* a working subscription); (ii) declare dead only after a **second**
unanswered re-SUBSCRIBE, so one swallowed SUBACK costs one extra AT round trip (~0.1 mAh) instead
of a teardown (0.33 mAh of stall + 82 s of reconnect + a lost page). Detection of a genuinely dead
session slows from 30 s to ~60-70 s, still far inside the modem's ~6 min silent resume. Ordering
falls out for free: the probe is issued before `net_service_session()` in the same iteration, so
the queue is already draining when the ping goes out.

**Ack stall (S3/S4).** Attribution first: `datatx_retx=1` with `prompt_orphan=0` and both drop
counters 0 means either the `> ` prompt never reached the parser or the 44 payload bytes never
reached the modem — the existing counters cannot tell these apart, and the payload write
(`WalterModem.cpp:2300`) is a bare `uart_write_bytes()` with no `tx_done` wait and no return check,
while `_uartWrite()` (`:965-966`) has a 10 ms `uart_wait_tx_done()` whose return is dropped (10 ms
= ~115 bytes at 115200, so a CTS-blocked 46-byte command line can return unflushed). So: count
prompts handled, count payload bytes written, keep `uart_wait_tx_done()`'s return, sample the CTS
level and the TX ring depth at the stall, and record which AT command was outstanding — then the
report *names* the command (RCA_SLEEP_URC fix 3). Fix candidates, cheapest first: (i) never issue
an ack publish in the same loop iteration in which RX bytes arrived — defer it one wake (≤5 s of
ack latency, which nothing bounds; zero power); (ii) a per-command timeout (patch 1.13) so a
DATA_TX_WAIT command fails in 10 s × 2 attempts instead of 30 s × 3 (0.33 mAh → 0.13 mAh per
occurrence, and 1.12 still recovers it); (iii) honour `uart_wait_tx_done()`.

**Reconnect stall (S5).** phaseAB's 81 795 ms is the teardown path, not the broker: M3's
disconnect-first (`lte_session_down()` inside the verdict branch, `xport_lte.cpp:597`) removes the
`+CME ERROR: 4` loop but not the timeouts — `mqttDisconnect()`, `AT+SQNSMQTTCFG` and
`mqttConnect()` each carry 30 s × 3 attempts against a modem that is not answering yet. So M3 does
*not* cover it. Fix: after a host-side dead verdict, do the teardown and then **return** — let the
existing F1/F3 backoff issue the reconnect on a later wake, after a probe has drained the queue;
and give the session commands the 10 s/2-attempt timeout of patch 1.13. Worst case per attempt
falls from ~90 s to ~20 s.

**Watchdog arithmetic (S6).** Patch 1.10 **does** already feed both watchdogs from inside the
blocking wait (`WalterDefines.h:316-321` → `watchdog.c:148-165`), with a 95 s budget
(`watchdog.c:25`) that is reset per *stage* by `watchdog_kick()` (`watchdog.c:133-138`).
Arithmetic: task WDT 60 s (`sdkconfig:1019`), RTC WDT 180 s, command timeout 30 s
(`sdkconfig:1966`) × 3 attempts (`WalterModem.h:142`) = 90 s. One stalled command fits (90 < 95).
**Two stalled commands in one stage do not** (180 s): feeding stops at 95 s and the task WDT
reboots ~60 s later — the most likely explanation of the phaseAA reboot, whose reset reason was
never captured. Decision: 10 s × 2 attempts = 20 s for the MQTT data/session commands (patch
1.13), leaving four such stalls per stage inside the 95 s budget; keep the budget, the task WDT and
the RTC WDT as they are; and log one line naming the stage and the command whenever a single
command exceeds 5 s, so the next reboot is attributable. A stalled command can then never reboot
the pager, while a genuinely wedged loop still does.

## 7. Console experiments (debug build, `at …`, no firmware change)

- **E1 — prompt abort / probe hazard (5 min).** `at AT+SQNSMQTTPUBLISH=0,"pager/test-pager/up",1,44`
  then `at AT`. Does the modem answer `OK`, or swallow the `AT` as payload, and does the prompt ever
  time out by itself? Settles RCA_SLEEP_PUBLISH §4.2 *and* whether the probe needs the
  publish-in-flight gate.
- **E2 — flow-control mode (5 min).** `at AT&K?`, `at AT+IFC?`, `at AT&V`. If a "modem ignores CTS"
  mode exists, (d) retires (c)'s byte-loss problem.
- **E3 — polling semantics (10 min).** With the session up and nothing pending:
  `at AT+SQNSMQTTRCVMESSAGE=0,"pager/test-pager/down"` — answer and latency on an empty queue. Then
  send a page and poll twice: does the no-`mid` form return a QoS 1 payload, and is the read
  destructive? Gates (b).
- **E4 — is there a RI/interrupt line (5 min).** `at AT+SQNRICFG?`, `at AT+SQNHWCFG?`. An immediate
  `ERROR` closes the question; success still needs the owner's ruling on routing a pad.
- **E5 — UART wake byte loss (one sleeptest window, needs a debug flag).** Phase 2's first
  measurement: RTS asserted through sleep + `esp_sleep_enable_uart_wakeup()`; count wakes with
  cause UART and clipped/unparseable lines.
