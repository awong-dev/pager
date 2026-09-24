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

---

## 8. 23 Sep — S7 read, and the corrections it forces (debug image ea05271, `phaseAF-*`)

S7's own verdict: **1-3 of its six criteria — pass, fail, fail.** Pages delivered and acked, zero
`MQTT session LOST` (S2 works), but 47 s / 46 s against the ≤ 30 s bar and 45% asleep against ≥ 85%.
Criteria 4-6 also fail. Phase 1 is *not* done, and §4's 87-91% prediction was wrong for a reason
worth writing down (§8.5).

### 8.1 What the `stalled command:` line does and does not say

`stalled command: "AT" elapsed=30000 ms cts=0 tx_ring_free=0 B`. Three of those four fields rule
out the wire, not the modem:

- **`tx_ring_free=0 B` is a constant on this target, not a measurement.** The UART is installed with
  a zero-byte TX ring (`WalterModem.cpp:4944`, `uart_driver_install(uartNo, UART_BUF_SIZE*2, 0, 0,
  NULL, 0)`), so `uart_get_tx_buffer_free_size()` always reads 0 — already documented at
  `WalterDefines.h:109-116` and `WalterModem.cpp:1846-1850`. "The `AT` could not leave the ESP's TX
  ring" is not a thing this field can say. With a 0-byte ring `uart_write_bytes()` writes straight
  into the FIFO.
- **`cts=0` means CTS *asserted*, i.e. the modem was ready to receive.** RTS/CTS on the ESP32 are
  active-low: `uart_ll.h:705-706` ("`sw_rts = 1` generates low level on RTS pin"), and
  `uart_ll_set_hw_flow_ctrl()` (`:530-545`) gates TX via `conf0.tx_flow_en` on CTS_n. The premise
  "the modem holds CTS low, not ready" inverts the polarity. This is also the same convention
  `net_sleep()` relies on when it drives RTS **high** to park the modem (`net.cpp:1167-1168`).
- **`txdone_timeouts=0`** (patch 1.13, `WalterModem.cpp:989-991`): every `_uartWrite()` saw
  `uart_wait_tx_done(10 ms)` return `ESP_OK`, so all four bytes of `AT\r\n` were shifted out within
  10 ms, every time. `buf_drop_queue=0 buf_drop_pool=0`: the parser never dropped a buffer.

So the `AT` went out and the modem was accepting bytes. **The stall is on the response-pairing side,
not the wire.** `elapsed=30000` is exactly the host's own default per-attempt timeout, not any
modem-side number, and `probe_answered=13` / `probe_noqueue=0` means attempt 2's `OK` came back.

Corollary: the vendor library has **no** modem UART power-saving configuration to blame. Neither
`+SQNIPSCFG` nor `+SQNPSCFG` appears anywhere in `firmware/components/` or `firmware/main/` or the
Kconfig, and `WalterModem::sleep(_, true)` (`WalterModem.cpp:5132-5168`) is nothing but the RTS
choreography `net_sleep()` already replicates — no dummy byte, no settle delay, no DTR, no wake
sequence to imitate. S8 confirms from the device: `AT+SQNHWCFG?` → `+CME ERROR: 4` (no such
command), `AT+CPSMS: 0` (PSM off, so the modem's UART is not in a PSM sleep at all), `+IFC: 2,2`
(the modem is honouring our RTS in both directions, as designed), `AT&K?` answers bare `OK` (the
Sequans does not implement `&K`; `+IFC` is the spelling it uses). **There is no CTS problem to fix
and no vendor wake-up sequence to add.** §3(d) is closed: no "modem ignores CTS" mode was found.

### 8.2 The mechanism: ranked, with the experiment that decides it

Nothing in the S7 artefacts is an in-window AT trace — light sleep kills the USB CDC
(`modes.c:2021-2023`), so the report is read back from NVS at boot and the `TX:`/`RX:` lines for the
30 s in question do not exist. Three candidates remain, all inside the parser/queue pairing:

1. **Response/command desync inside the URC flush burst (most likely).** A command completes when
   the buffer that reaches `_processModemRSP()` *starts with* its expected `atRsp`
   (`WalterModem.cpp:4011`), which for nearly every command is literally `"OK"`; and the buffer is
   paired with whatever `_curCmd` happens to be at *dequeue* time (`:1660`), from a single FIFO
   shared by commands and response buffers (`:1650-1662`). A flush burst delivers several buffers
   back-to-back. If one of them completes the wrong command, each later command is satisfied by the
   previous one's terminator and the **last** command in the chain waits its full timeout for an
   `OK` already consumed — then times out, retries, and resynchronises. Predicted signature: exactly
   one full-timeout stall per burst, ending at the timeout, self-healing, no drops, CTS fine. That
   is precisely the report.
2. **`_receivingPayload` sticks true across the burst.** While it is set (`:1459-1467`) every byte
   is consumed as payload and **nothing is queued at all**; it clears only when the byte count
   reaches 0 (`:1465`) or on a command timeout (`:1897`, `:1992`) — hence, again, exactly the
   timeout. `_expectingPayload()` (`:1408-1439`) takes the count from the *command line's own*
   requested size, so it balances only if the host asked for exactly what the modem will send. The
   normal path does (`xport_lte.cpp:393` passes `data->msg_length`), which is why this is second —
   but the **oversize branch passes `sizeof(s_mqtt_rx_buf)`** (`xport_lte.cpp:386`) and would stick
   for certain. Latent bug regardless of this run (no oversize message occurred).
3. **The modem genuinely did not answer for 30 s.** Cannot be excluded without the trace, but it is
   least likely: `phaseAF-s8.log` shows a bare `AT` answered inside one 10 ms log tick with the
   session up, and the stall ends at the *host's* number.

**The experiment (task S10, counters not a trace, ~20 lines, no bench time beyond one S7 rerun).**
Three counters discriminate all three without capturing a byte: `rsp_no_cmd` (buffers reaching
`_processModemRSP()` with `cmd == NULL` — hypothesis 1 predicts >= 1 per stall, 2 and 3 predict 0);
`payload_stuck_ms` (how long `_receivingPayload` had been true when a command timed out —
hypothesis 2 predicts > 0); and the probe's own issue→answer microseconds for attempt 1 vs the
retry (hypothesis 3 predicts both slow, 1 and 2 predict the retry is fast).

### 8.3 The 31 551 ms "render" is the display, not the modem

Arithmetic, exact: **1500 + 15009 + ~15042 = 31 551 ms.**

- 1500 ms = `disp_pre_write_gate_hook()`'s bound spent in full (`ui.c:539` `PAGER_UI_PUBLISH_QUIET_MAX_WAIT_MS`,
  `:554-559`) — it waited its whole budget because the 29 824 ms `/up` ack publish was in flight the
  entire time, then let the refresh start anyway. That is the 23 Sep display-corruption gate
  *failing open*, which is the interesting part.
- 15 009 ms = `disp_wait_busy_fb()`'s `PAGER_UI_BUSY_TIMEOUT_US` (`disp.c:22`, reached at `:196-203`,
  called from `:460` full / `:586` partial).
- ~15 042 ms = the one retry's `disp_wait_busy()` (`disp.c:463`/`:589` → `:214`, `fallback_ms == 0`,
  so straight to the same 15 s poll).

This is not inference: **this board did exactly that at this boot** —
`phaseAF-boot.log:61-63`, `BUSY: entry=1 timed out after 1502 iters (~15009 ms)` then a retry that
took 9 230 ms, and `:68` `BUSY line never asserted; using fixed waits (check the IO18 wire)`. So:
not `net_publish_quiet_wait_ms()` (bounded 1.5 s and it behaved), not `disp_busy_idle_hook()`
(`ui.c:528-534`, one I2C read), not S4's deferral (that is `msg_pump()`, a later bucket). It is a
flaky BUSY line whose two 15 s bounds are far too long for a battery device, amplified by a publish
that was stuck for 30 s. **The IO18 BUSY wire is an owner hardware call; the 15 s bounds are ours.**

`ui_wake_status_refresh()` (`modes.c:600-604` → two blocking ATs, `modes.c:2211`, inside the same
bucket) is a *second*, real exposure of the same kind — but it did not fire in this window:
`input_awake()` is armed only by keys/buttons and `main.c:289/296`, never by an incoming page
(`modes_alert_incoming()`, `modes.c:1295-1318`, does not arm it), and no key was pressed.

### 8.4 Probe policy: neither "flow control off" nor "wait for CTS"

§8.1 rules out a CTS problem, so both options in the question are unverified defensive changes and
are dropped. What the numbers actually indict is the probe's **budget**, and one inverted bound:

- 49 sleeps → 49 `net_probe_guard_poll()` calls → 13 issued + 8 stuck × 4 refused polls + 4
  leftover = 49 exactly (`net_probe_guard.c:14-26`). So **8 of 13 probes went unanswered for >= 4
  wake intervals (>= 20 s)**, not 2. The two the publish ring caught are the visible tip.
- The guard gives up at ~20 s while the library holds the command for 30 s. Inverted: the guard
  reissues a probe that queues *behind* a still-live one, spending a second of the 8 shared queue
  slots (`WalterModem.h:127,3501`) for nothing.

**Decision: give the probe 1 attempt / 2 s** (patch 1.14) — which is what §5(1) already specified
("one `AT`, fire-and-forget… must NOT … retry"); the code simply never matched it. Safe because the
modem flushes its queued URCs when it **accepts** a command, not when the host sees the answer, so
the flush has already happened before the 2 s deadline matters. Saving per avoided 30 s block:
0.33 mAh of awake ESP (30 s × 40 mA) + up to 15 s of `publish_quiet`'s sleep-hold
(`PUBLISH_SLEEP_HOLD_MAX_US`, `publish_quiet.h:83`) + up to 30 s of `net_modem_busy()`'s ≈ up to
0.8 mAh per page; ~16 mAh/day at 20 pages/day against a 95-107 mAh/day budget. Assumptions: 40 mA
awake / 1 mA asleep (§2), not measured on this board.

### 8.5 Why 85% was unreachable, and the honest bar

Two things in §4's prediction were wrong, and one is not a bug:

1. **ACTIVE mode lasts 10 minutes, not 30 s.** `PAGER_ACTIVE_IDLE_TIMEOUT_S` is `10 * 60`
   (`modes.c:143`), set by `set_mode()` (`:819`) and re-armed by `modes_note_activity()` (`:853`).
   So from the first page at +140 s the whole rest of the window ran at the 2 s ACTIVE interval
   (`modes.c:1881-1883`). The report proves it arithmetically: 49 sleeps totalling 164 s solves
   uniquely to 22 × 5 s + 27 × 2 s.
2. **§4 assumed the pager still light-sleeps after a page. It does not, for a while.**
   `skip_sleep` also ORs `net_modem_busy()` and `net_publish_in_flight()` (`modes.c:1972-1973`);
   with a 30 s stalled command both stay true, so the loop degenerates to the 100 ms poll
   (`modes.c:2117`). That is the whole of bucket 1: `awake-loop 737 × ~99 ms ≈ 73 s`. It is
   *caused by* the stall, not by ACTIVE mode. The `ui_awake` term was never involved (§8.3).

Duty-cycle model, taking the per-iteration numbers out of this run (yield 196 ms + non-stall work
~36 ms = **232 ms awake per wake**; `input+ui+render` (35.4−31.5)/785 ≈ 5 ms and `msg_pump+health`
(54.2−29.8)/784 ≈ 31 ms):

| phase | duty | note |
|---|---|---|
| SLEEP, 5 s interval | 232/5232 = **4.4%** | steady state |
| ACTIVE, 2 s interval | 232/2232 = **10.4%** | 10 min after every page |

For `sleeptest 6` with pages at 90 s and 250 s, i.e. ~140 s SLEEP + ~220 s ACTIVE:
140 × 4.4% + 220 × 10.4% = 6.2 + 22.9 = 29 s, plus two renders (≈ 3.5 s each) and ~2 s of
per-page `handler_busy`/publish-in-flight holds → **awake ≈ 40 s, asleep ≈ 89%**.

So 85% *is* reachable, but only with the stall gone and only because the arithmetic is dominated by
a term §4 never wrote down. **New bar (S7b): asleep ≥ 85%, predicted band 86-90%**, and the report
must show `awake-loop (no sleep)` under 10 s — that bucket is the stall detector now.

Daily view (the number that matters, assumptions in §2): SLEEP steady state 0.044×40 + 0.956×1 =
**2.72 mA**; ACTIVE 0.104×40 + 0.896×1 = **5.1 mA**. Each page costs 10 min of ACTIVE = (5.1−2.72)
mA × 600 s = **0.40 mAh**; 20 pages/day = 8 mAh/day. Shortening ACTIVE from 10 min to 2 min would
recover ~6.4 mAh/day of that — real, but a UI-responsiveness policy call for the owner, not a
firmware fix, and it is **not** the modem-busy interlock (`modes.c:1939-1945` prices that at ~1.5 s
per message). Nothing here changes `PROTOCOL.md`'s < 5 s active-mode requirement, which the 2 s
interval is what satisfies.

### 8.6 46-47 s is an eDRX bill, not a host bug

Both pages landed 47 s and 46 s after the relay stamp, and the ring arithmetic says the 30 s stall
came *after* ingest (page at +140 s; the 29 824 ms `/up` completing at +176 s was therefore issued
at ~+146 s, 6 s after ingest — same shape at +303/+337). So the stall does not explain the latency.
`AT+SQNEDRX=2,4,"0010","0001"` (`phaseAF-boot.log:106`) is eDRX value 2 = **20.48 s** with a 2.56 s
paging time window; 2 × 20.48 + one 5 s wake interval + render ≈ 46-47 s. **A missed first paging
occasion costs a whole second cycle, and one missed occasion already breaks the ≤ 30 s bar.**
Levers, costed: eDRX value 1 (10.24 s) makes the two-cycle worst case ≈ 26 s and doubles the
modem's paging occasions (a modem-side cost this design has no measurement for — that is the
experiment); or accept the bar as "≤ 30 s typical, ≤ 50 s worst case" and say so in
`docs/PROTOCOL.md`. This is the single biggest remaining latency term and it is **not** a host fix.

### 8.7 Two options retired by S8, one hardware question sharpened

- **§3(b) explicit polling is dead.** `AT+SQNSMQTTRCVMESSAGE=0,"pager/test-pager/down"` on an empty
  queue answers `+CME ERROR: 4` (`phaseAF-s8.log:49-53`), so the no-`mid` form is not supported at
  all. (b) cannot be phase 2's content-recovery path, which means §3(c)'s byte-loss problem has to
  be solved by a parser resync — or by the next bullet.
- **The modem has a ring-indicator line: `+SQNRICFG: 1,3,1000`** (`phaseAF-s8.log:27-31`) — enabled,
  function 3, 1000 ms pulse. `firmware/main/pins.h` routes no modem→ESP wake pad, so this is an
  owner hardware call. If it were wired, phase 2 becomes clean and cheap: wake the ESP32 on RI
  (ext0/ext1, no UART-wake byte loss at all), then send one `AT` to flush. That retires §3(c)'s
  clipping problem and the whole of S9's parser risk. Worth asking before spending firmware time on
  S9.

---

## 9. 23 Sep (late) — S7b read: one counter changed meaning, and it reset the modem (debug image bc5e023, `phaseAI-*`)

S7b **fails 3, 5 and 7 and cannot be scored on 1-2**: the pager did not deliver pages late, it went
**offline inside the window and never came back** — four `MQTT session LOST (rc=-13)`, zero
`MQTT session (re)connected`, no publish at all inside the window (the ring's two entries are at
−152 s and −54 s), and **no page received** for two pages sent at +92 s and +250 s (never acked at
the relay, no SECURITY lines). `asleep 253 s (70%)` is not a sleep-policy reading at all — §9.1
shows 93 s of it was not sleep. The image was also never a complete S7b image: S12 and S13 were not
in it, only S11 plus 9c9f506 and W13.

### 9.1 The arithmetic that names the event: 93 s of "sleep" that was not sleep

Two independent readings of the same report disagree, and the gap is the whole story:

- `80 light sleeps` at the ACTIVE interval the cycle list prints (2001-2049 ms, `modes.c:116-117`)
  is **160 s** of `esp_light_sleep_start()`.
- the report says **`asleep 253 s`**, and `253 + 106 (awake total) = 359 s ≈ the 360 s window`, so
  the total is self-consistent — the *attribution* is not.

`s_st_asleep_us` is accumulated from *before* `net_sleep()` to *after* `net_urc_probe()` **and
`check_probe_stuck_escalation()`** (`modes.c:2043-2060`). So **~93 s of the "asleep" total was spent
awake inside that block**, and the only thing in it that can take seconds is
`check_probe_stuck_escalation()` → `rate_limited_modem_recover()` → `net_recover_modem()` → a full
F4 modem reset. Separately, `probe_issued=0` alongside `probe_answered=2 probe_stuck=1` is
impossible without a mid-window `net_probe_guard_init()` (`answered`/`stuck` only come from
`probe_cb()`, `net.cpp:1272-1287`, which cannot run for a probe never counted `issued`, and the one
synchronous callback would have shown as `noqueue`), and `net_recover_modem()` (`net.cpp:1392`) is
the only caller of that outside boot. **Two unrelated report fields both say an F4 ran.**

### 9.2 Mechanism (six lines, file:line)

1. **eb2a578 changed what `stuck` means.** A probe the library fails inside its own 2 s budget now
   counted `stuck` (`net.cpp:1280-1286` → `net_probe_guard_failed()`,
   `net_probe_guard.c:46-51`) — patch 1.14's whole point being that such a probe is *cheap and
   expendable* (§8.4).
2. **Its consumer was left reading the old meaning.** `check_probe_stuck_escalation()`
   (`modes.c:1635-1653`) treats **six consecutive `stuck` as "the modem's command path is wedged"**
   and calls `rate_limited_modem_recover()`. In ACTIVE mode, one probe per 2 s wake, that is **12
   seconds** of the cheapest possible failure buying a full modem reset. In phaseAF the same 8
   unanswered probes never escalated, because every `answered` in between reset the streak
   (`:1639-1642`) — the 30 s budget always eventually answered. The 2 s budget does not.
3. **The F4's own reset then failed slowly.** `WalterModem::reset()` pulses the reset pin, waits
   1000 ms, and then queues a `TX_WAIT` for `"+SYSSTART"` (`WalterModem.cpp:5073-5086`) — but
   `_parseRxData()` **discards every received byte while `_hardwareReset` is true** (`:1446`), which
   is exactly that 1 s window. A boot banner landing inside it is gone, and the wait then costs the
   library default of 3 × 30 s. 1 s + 90 s = **91 s ≈ §9.1's 93 s**. That is the whole excess, to
   within the measurement.
4. **A failed reset latched the drain probe off.** `net_recover_modem()` sets `s_modem_begun = false`
   before the reset (`net.cpp:1385`) and returns false without restoring it (`:1393`), while
   `WalterModem::begin()` — which is where the flag is otherwise set (`:587`) — is never reached.
   `net_urc_probe()` no-ops from then on: `probe_issued=0` for all 80 wakes, and the next F4 is
   rate-limited for 10 minutes (`modes.c:1577`). The residual `answered=2 stuck=1` are callbacks
   from probes the library still held when `net_probe_guard_init()` zeroed the counters at `:1392`.
5. **The reset also destroyed the session it was supposed to rescue.** It cleared
   `s_session_configured`/`s_registered` (`:1401-1402`) and the physical reset wiped the modem's TLS
   profile and MQTT client, so every later attempt had to redo `configure_session()` and then a
   **blocking** `mqttConnect()` (`xport_lte.cpp:453`) — `stalled command: "AT+SQNSMQTTCONNECT=0,"s1"
   elapsed=30000 ms` and `mqtt status/retry max 30013 ms` are that call, on the modes task, which
   is also why the pager could not light-sleep through it (§8.5 item 2).
6. **`rc=-13` is the modem's own verdict and the four gaps are the retry loop.** `s_last_rc` starts
   at 0 (`xport_lte.cpp:89`) and is written only by the wire-fed CONNECTED-failed / DISCONNECTED
   handlers (`:259`, `:407`); both host-side "dead" verdicts set only `s_last_class` (`:599`,
   `:655`) and `resub_first_swallowed=0` says neither fired. `classify_mqtt_rc()`'s `default`
   (`:230-234`) makes −13 TRANSIENT → `handle_mqtt_loss()` → `schedule_backoff()`. The gaps between
   the four losses are **36 / 46 / 91 s = one 30 s CONNECT stall plus backoff 5 / 15 / 60 s**
   (`modes.c:157`) — three for three.

One more defect found in the same function: after `net_probe_guard_init()` zeroes the counters,
`check_probe_stuck_escalation()`'s own `s_last_stuck` static still holds the pre-reset value, so
`s_consecutive_stuck += (0 - 6)` **underflows an unsigned** to ~4.3e9, which is `>= 6`, which
escalates again on the very next wake. Only `rate_limited_modem_recover()`'s 10-minute limit
absorbed it.

§8.2's hypothesis 1 (response/command desync inside the flush burst) is **still open and still
unmeasured** — it is the best explanation for why a 30 s `AT+SQNSMQTTCONNECT` never saw its `OK`
when `buf_drop_queue=0 buf_drop_pool=0 txdone_timeouts=0` say nothing was dropped and the bytes went
out, and item 3's silent byte-discard is a second instance of the same class. S10 is what settles
it; it was never run, and it is now the highest-priority task in the file.

### 9.3 What this is *not*: W13's shared ident scratch, and the identity

Ruled out, positively. `ident_scratch()` (`ident.c:22-33`) opens with `assert(!s_scratch_busy)` and
this is an assertion-level-2 build (`sdkconfig:518,522`), so a genuine two-task overlap — and one is
reachable in principle: `on_auth_epoch_wrap()` (`modes.c:483`) is bound as a callback into
`msg.c`/`book.c`/`loc.c`/`sms.c` and can therefore run on the modem event task, against
`ident_load()`/`catrust.c` on the main task — **aborts and reboots**. The window's uptime is
continuous to 525 s with no gap in the cycle list, so no overlap happened. A silent clobber is not
available either: the scratch is not `s_ident`, `net_session_up()` reads the live getters
(`xport_lte.cpp:453`), a wrong password answers −11/−5 rather than −13, and `phaseAI-live.log:11-22`
shows the same NVS identity attaching, pinning the CA and reaching "MQTT session usable" in 2 s
right after the reboot. The latent crash stands as a defect and belongs with the next epoch-wrap
change, not here.

### 9.4 The change, and the options it was chosen from

**Done — four surgical edits, `firmware/main/` only, no library patch:**

1. **`net_probe_guard_failed()` counts a new `timedout`, not `stuck`** (`net_probe_guard.{c,h}`,
   plumbed through `net_probe_counters_t` and the report line). `stuck` goes back to meaning only
   what its consumer was written against: the library had not released the command after
   `NET_PROBE_GUARD_STUCK_WAKES` wake intervals. This is the fix for §9.2 items 1-2 and it is the
   one that matters: **a 2 s probe timeout can no longer reset the modem.** Host test extended with
   the exact regression shape (six consecutive 2 s timeouts must leave `stuck == 0`).
2. **`check_probe_stuck_escalation()` resynchronises instead of subtracting** when the counters go
   backwards, killing the unsigned underflow (`modes.c`).
3. **A failed `WalterModem::reset()` restores `s_modem_begun`** (`net.cpp`) so it can never again
   silently disable the URC drain probe for ten minutes (§9.2 item 4). Two lines.
4. **Instrumentation so none of §9.1 has to be arithmetic next time**: `probe_timedout`,
   `probe_skip_busy`, `probe_skip_down` on the probe line and `modem_resets` (`g_rtc`, already kept)
   on the modem line. Cumulative since boot and deliberately *not* cleared by
   `net_probe_guard_init()`.

**Power and latency.** All four are bookkeeping; none adds or removes an AT round trip in the
healthy path, so the steady-state duty cycle of §8.5 is unchanged (SLEEP 4.4% → 2.72 mA, ACTIVE
10.4% → 5.1 mA). What they remove is one measured **93 s** of full-current awake time plus the
reset's own re-attach — 93 s × 40 mA ≈ **1.0 mAh per occurrence**, and this window had one in six
minutes; left alone, the 10-minute rate limit caps it at 6/hour ≈ **6 mAh/h ≈ 144 mAh/day**, i.e.
more than the entire 95-107 mAh/day budget. It also removes the eight-minute session outage and the
two lost pages, which is the part that actually matters. Assumptions unchanged: 40 mA awake / 1 mA
asleep (§2), **estimated, not measured on this board**. **Delivery latency is untouched** — still
the 2 × 20.48 s eDRX bill of §8.6 (S14's call), and the ≥85% asleep bar should now be readable for
the first time, since 93 s of the previous number was mis-attributed.

**Considered and not taken:**

- **(a) a probe with a unique terminal response** (e.g. `AT+CMEE?` matched on `+CMEE:` instead of a
  bare `OK`) does not close the hole: the information line is unique, but the **trailing `OK` still
  arrives**, and `_cmdProcessingTask` clears `_curCmd` and pops the next queued command
  (`WalterModem.cpp:1694-1706`) between the two buffers, so the orphan `OK` can still complete
  whatever is behind it. It narrows the race from seconds to microseconds at the price of a new
  probe command whose side effects are unverified. Revisit only if S10 says the orphan `OK` is the
  dominant term.
- **(b) restore a probe budget ≥ the longest observed flush burst.** The longest observed is
  `elapsed=30000 ms`, so this *is* the pre-eb2a578 behaviour: 45% asleep and two 29 824 ms `/up`
  acks. With the `stuck`/`timedout` split above, the 2 s budget no longer has the consequence that
  made it look dangerous, so there is nothing left to buy here — but the orphan-vs-slot-hold trade
  is a measurement, not an argument, so it is kept as **S16**: an A/B behind a runtime flag on one
  flash, same wake interval both times.
- **(c) patch the library's pairing.** The defect is real and localised (`:4011-4013` finishes
  `_curCmd` on *any* error line with no `atRsp` check at all, `:2387-2426`), and the honest fix is
  small — refuse to complete a command with a buffer that was queued before that command's own
  `attemptStart`. But whether it fired in this window is still inference, and a wrong patch here
  mispairs every command on the device. Specced as **S15**, gated on S10's `rsp_no_cmd` > 0.
- **skipping the probe on `net_connect_in_flight()`/`net_modem_busy()`** was written, then reverted
  as dead code: `net_urc_probe()` only runs inside `modes.c`'s `if (!skip_sleep)` branch and
  `skip_sleep` already ORs all three terms (`modes.c:1986`).

### 9.5 Amended S7b sequence

Flash order: **S10 first** (counters only, settles §9.2's open item), then S13, then S12, then
§9.4's four edits (already in the tree, uncommitted); S15 only if its gate opened. Bars 1-7 of S7b
stand, plus: **bar 8** — `probe_issued` > 0 and `probe_skip_down` ≈ 0, i.e. the drain probe was
alive for the window being scored, with `probe_timedout` reported and interpreted as cheap (bar 4's
reading now attaches to `probe_timedout`, and `probe_stuck` goes back to being a real fault signal);
**bar 9** — `modem_resets` = 0. A window containing an F4 is **void, not failed**: ~93 s of its
`asleep` total is not sleep, so no asleep% or latency reading from it means anything. Re-run it and
report the F4 as its own finding.

---

## 10. 24 Sep — S7c read: the sleep half passed, the delivery half failed, and three of the numbers we were reading are artefacts (`phaseAK-report.log`, `phaseAL-ps.log`)

S7c: **bars 1, 5, 6, 8, 9 pass; bars 2, 3, 7 fail.** 95% asleep, 69/69 timer wakes, zero session
losses, zero resets, all four modem drop counters 0 — and **no page received at all**, neither of
the two (+92 s, +250 s), never acked at the relay. The sleep policy is finished; the delivery path
is not, and the window that was supposed to settle *why* produced three unreadable numbers.

### 10.1 Mechanism, four lines

1. The modem holds a page URC while RTS is deasserted and releases it only when it **accepts** a
   command; the host's drain probe (`net_urc_probe()`) is that command.
2. The probe is not accepted inside the 200 ms post-wake window — the only windows that ever
   delivered were the ones that stayed awake seconds (phaseAD, 10 s delivery).
3. So the ESP is back asleep, RTS high, UART clock gated, long before the URC and the multi-step
   fetch it triggers (`+SQNSMQTTONMESSAGE` → `AT+SQNSMQTTRCVMESSAGE` → ~100 B → the ack publish)
   can complete. Nothing is lost on the wire; nothing is ever handed over either.
4. Fix: **hold the wake window open, RTS asserted, until the probe is ANSWERED (bounded), and
   lengthen the wake cadence so that cost is amortised.** Owner's ruling, 24 Sep: *"3 s is okay
   honestly"* — design around the answer latency, do not chase it. WAKE0/IO46 is a later
   optimisation, not the beta path.

### 10.2 Three artefacts, because they change what the rest of this section may assume

**(A) `probe_first_attempt_ms=2997` is not "the modem answers in 3.0 s".** It is one
last-writer-wins field (`net.cpp` `probe_cb()`) written by *every* callback branch —
answered, timed out, refused — against `s_probe_issue_us`, which is re-stamped by *every* new
probe. In a window with 18 issues, 8 answers, 10 timeouts and **12 stucks**, a late callback for an
old probe is routinely measured against a newer probe's stamp. Neither branch can produce 2997
honestly: an *answered* probe cannot exceed its own 2 000 ms budget, and a *timed-out* one should
land at ~2 000 ms. Same class as `tx_ring_free=0` (§8.1) and `avg 870 ms / max 198 ms` (§1 item 4).
**The modem's answer latency has never been measured.** S18 adds `answer_ms last/max/n`, recorded
only on the answered branch, which is the first honest measurement of it.

**(B) FreeRTOS ticks do not advance across `esp_light_sleep_start()`, so every library command
timeout is denominated in *awake* time.** IDF's light-sleep exit adjusts `esp_timer` only
(`esp-idf/components/esp_hw_support/sleep_modes.c:1263`, `esp_timer_private_set`); there is no
`vTaskStepTick`/`xTaskCatchUpTicks` on the manual path, and the library's budgets are all
`xTaskGetTickCount()` diffs (`WalterModem.cpp:1929`, `:2045`). Proof from this window alone, no
source needed: the probe carries **1 attempt / 2 000 ms** and yet `probe_stuck=12` — twelve probes
were still outstanding three wake intervals (15 s of wall clock) after being issued. Impossible
under real-time ticks. At phaseAK's 4.2% duty a "2 s" budget is **~48 s of wall clock** and the
library's 30 s default is **~12 minutes**. This does not invalidate §8/§9 (those windows were 45-70%
awake, so the factor was ~2, not ~24) but it does mean no timeout constant in this system means what
it reads until the host is awake — which, with S18's wait, is exactly the window that matters.

**(C) There has never been an AT trace, in any image.** `CONFIG_LOG_MAXIMUM_LEVEL=3` (INFO)
compiled `ESP_LOGD` out, and the whole trace is `ESP_LOGD` (`WalterModem.cpp:2086` `RX:`,
`:2428` `TX:`). Every `esp_log_level_set("WalterModem", ESP_LOG_DEBUG)` in `main.c`'s `at` command
and in `modes_debug_sleeptest_start()` has been a no-op. That is why §8.2 had to say "nothing in the
S7 artefacts is an in-window AT trace", and why `phaseAL-ps.log` could only print `at: OK`:
**`AT+SQNIPSCFG?` and `AT+SQNPSCFG?` both exist** (they answered OK; `AT+SQNHWCFG=?`, `AT+CSCLK?`,
`AT+SQNWAKECFG?`, `AT+SQNPMU?`, `AT+SQNSLEEP?` all ERROR, i.e. do not), but their **values were
never captured**. Fixed in `sdkconfig.defaults` (`CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`): compiled in,
runtime default still INFO, **+21 kB flash release / +35 kB debug** (measured), no timing or power
change. Capture for the values is now just: boot, wait for `MQTT session usable`, then
`at AT+SQNIPSCFG?` — the `RX:` line prints.

### 10.3 The cadence table (the decision)

Assumptions, all from §2 and **estimated, not measured on this board**: `I_awake` = 40 mA,
`I_sleep` = 1.0 mA, plus §2's other loads (liveness pings 1.2 mA + UI/display 0.2-0.7 mA) taken as a
flat **+1.5 mA**. Awake per wake = `t_ans` (the modem's answer latency, §10.2(A): **unmeasured**;
tabulated at the owner's 3.0 s and, for contrast, at 0.5 s) + 0.25 s of work (phaseAK: 196 ms yield
+ 32 ms `msg_pump+health`). Period = `T` + awake. Latency = eDRX (mean 10.2 s, worst 20.5 s of the
20.48 s cycle; §8.6's missed occasion doubles it) + wait for the next wake (mean `T/2`, worst `T`) +
`t_ans` + fetch 0.5 s + render ~2 s.

| T | duty | mA (t_ans=3.0) | **mAh/day** | days on 1500 mAh | mA (t_ans=0.5) | mAh/day | typical latency | worst (1 eDRX) |
|---|---|---|---|---|---|---|---|---|
| 5 s *(today, no wait)* | 3.8% | 4.0 | **96** | 15.6 | — | — | *never delivers* | — |
| 10 s | 24.5% | 12.1 | **290** | 5.2 | 5.2 | 125 | 20.7 s | 36 s |
| **20 s** | **14.0%** | **8.0** | **191** | **7.9** | **3.9** | **94** | **25.7 s** | **46 s** |
| 30 s | 9.8% | 6.3 | **151** | 9.9 | 3.5 | 83 | 30.7 s | 56 s |
| 60 s | 5.1% | 4.5 | **108** | 13.9 | 3.0 | 72 | 45.7 s | 86 s |

The "≤ 30 s typical" bar needs **T ≤ 28 s**. Nothing in the 3.0 s column also meets the ~100 mAh/day
budget: that is the honest shape of the trade, and it is why the two columns matter more than the
recommendation — **WAKE0/IO46, or §3(c)'s RTS-through-sleep, is worth 97 mAh/day at T=20 s**, which
is the entire budget. They stay on the list as optimisations on top of a delivery path that works.

**Recommendation for beta: T = 20 s, wait bounded at 4 s.** Typical 26 s (inside the bar), worst 46 s
(stated as such in `PROTOCOL.md`, §8.6's call), 191 mAh/day ≈ 8 days on 1500 mAh at `t_ans` = 3.0 s,
and 94 mAh/day ≈ today's budget if `t_ans` turns out to be sub-second. **Run the window first**: the
new `probe answer: answer_ms` line decides between T=20 and T=30 arithmetically, and it is the
constant this whole table is parameterised on.

Two mechanical answers the cadence change needs: the wake interval **is** a plain constant —
`PAGER_WAKE_INTERVAL_SLEEP_MS 5000u` (`modes.c:117`), `PAGER_WAKE_INTERVAL_ACTIVE_MS 2000u` (`:118`),
`PAGER_WAKE_INTERVAL_UNREGISTERED_MS 30000u` (`:131`) — and the `sleeptest` override
(`modes.c:2048`) replaces it in **both** modes, which is what lets one window measure both pages
under the same policy. And the ext wakes are unaffected: `net_sleep()` arms ext0 (button, IO1) and
ext1 (LIS3DH INT1, IO2) on every call independently of `esp_sleep_enable_timer_wakeup()`
(`net.cpp:1136-1167`), so a longer timer only lengthens the *timer* path. The real UX cost is the
**CardKB**, which is I2C-polled and not a wake source at all: in SLEEP mode a keypress is already
ignored until the next wake, and T=20 s makes that up to 20 s. The button still wakes instantly.

### 10.4 The change (S18), and what it does not do

Smallest edit that makes the experiment runnable; **release behaviour is unchanged** because the
wait is gated on a cadence the shipped constants do not use.

- `modes.c:2264` — `wait_for_probe_answer(interval_ms)` (helper at `modes.c:1772`) after the post-wake yield: polls
  `net_urc_probe_in_flight()` every 20 ms until the probe is answered, bounded by
  `PAGER_PROBE_WAIT_MS` (4 000) and armed only when `interval_ms >= PAGER_PROBE_WAIT_MIN_INTERVAL_MS`
  (10 000). Placed *after* the `#ifdef` bookkeeping so the time lands in the report's
  `post-wake yield` bucket, not in `asleep` (§9.1's mis-attribution). It does **not** wait for the
  message fetch — `skip_sleep` already ORs `net_modem_busy()`/`net_publish_in_flight()`, so the loop
  stays on its 100 ms poll for exactly as long as the fetch takes.
- `net.cpp:1271` `PAGER_URC_PROBE_TIMEOUT_MS` 2 000 → **6 000** (still 1 attempt). With a 2 s library
  budget under a 4 s host wait the answer is *structurally* orphaned: the library gives up, clears
  `_curCmd`, and the late `OK` arrives with no command — which is phaseAK's `probe_timedout=10` and
  `rsp_no_cmd=3`, both of which have been read as modem misbehaviour and are host bookkeeping.
- `net.cpp:1418`/`net.h:451` — `net_urc_probe_in_flight()`, and `answer_ms last/max/n` recorded only on the
  answered branch (§10.2(A)).
- `main.c` — `sleeptest <min> [yield_ms] [interval_ms] [probe_wait_ms]`, 4th argument, debug only.
- `sdkconfig.defaults` — `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` (§10.2(C)).

Failure modes: a probe unanswered at the bound is counted `gaveup`, left to the existing guard
(`timedout`, then `stuck` after 3 wakes) — nothing is reset and nothing is retried; the 4 s loop is
bounded well inside `watchdog.c`'s 95 s stage budget and deliberately does **not** kick it; ACTIVE
mode and every released image keep today's 200 ms window because of the interval gate.

**Not done, deliberately:** `PAGER_WAKE_INTERVAL_SLEEP_MS` is still 5 000. The cadence is the
*result* of the experiment, not an input to it, and shipping a 20 s constant before `answer_ms` has
ever been read would be exactly the unverified change the bench rules forbid.

### 10.5 The bench sequence (one window, ~12 min)

Debug image, `PAGER_DEBUG_NO_LIGHT_SLEEP=1`. One hardware agent, foreground captures.

1. Boot, wait for `MQTT session usable`. Capture `at AT+SQNIPSCFG?` and `at AT+SQNPSCFG?` — the
   `RX:` lines now print (§10.2(C)). ~2 min, and it retires §3(d) either way.
2. `sleeptest 6 0 20000 4000`. Pages from the relay at +90 s and +250 s, relay poll in a second log.
3. Capture the post-reset reprint.

**Predictions, so the run can falsify something.** `probe answer: answer_ms last/max n≈18`: if the
owner's 3 s is right, 2 500-3 500 ms; `wait avg` ≈ `answer_ms`, `gaveup` = 0. Delivery: both pages,
**20-30 s** after the relay stamp, both acked. Asleep: `1 − (answer_ms+0.25)/(20+answer_ms+0.25)`
→ **86%** at 3.0 s answer, 96% at 0.5 s; the report's `awake-loop (no sleep)` must stay under 10 s
(§8.5's stall detector). `probe_timedout` and `rsp_no_cmd` should now be **0** — if they are not, the
6 s budget is still short and the answer is slower than 3 s. If `answer_ms` comes back sub-second,
the whole table shifts to its cheap column and T should drop to 10 s, not rise.

---

## Phase 3 candidate (24 Sep 2026): the ULP-RISC-V as a byte-capturing coprocessor

Owner's question: could the ULP run permanently during light sleep, collate the bytes the modem
sends before the main core is awake, and hand them down, so no URC byte is ever clipped?

Feasibility: the modem's TX0 lands on IO14, an RTC GPIO, readable by the ULP-RISC-V in light
sleep. At 115200 baud one bit is 8.7 us = ~150 ULP cycles at 17.5 MHz, enough for a bit-banged
receiver (RTC clock accuracy and calibration are the risk). RTC memory (8 kB) holds any URC.
Power: on the order of 100 uA continuous, small against the ~1 mA light-sleep floor. The hard
part is the handoff, not the receiver: the ULP must keep capturing until the main core has
re-enabled the UART and signalled takeover, and the host must splice the ULP bytes ahead of the
UART stream without double-counting the overlap. It is a new component invisible to host tests.

Ranking: behind two cheaper gates. (1) Explicit queue read after wake: if
`AT+SQNSMQTTRCVMESSAGE=0,"<topic>"` returns a PENDING message without the URC's message id
(S8 only showed the empty case answers +CME ERROR: 4, which is a usable negative), phase 2 is
"wake on RX activity, discard the clipped line, read the queue" and no coprocessor is needed.
(2) Measure the clip: bytes lost between the first RX edge and the UART being live; a handful is
a resync rule, not a coprocessor. If both fail, the ULP receiver gets its own design document
with the handoff protocol as the centrepiece.
