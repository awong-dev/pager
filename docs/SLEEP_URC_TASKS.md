# Sleep/URC delivery — tasks

Companion to `docs/SLEEP_URC_DESIGN.md`, which holds the reasoning; this file holds only what an
implementing agent needs. Each task names the files to read first, the files it may touch, what to
do, and how to verify. Bench tasks obey `docs/HARDWARE_TESTING.md` and the bench rules in
`.overnight-handoff.md` verbatim: one reader on the port, one hardware agent at a time, bounded
foreground captures. **Coding agents never open `/dev/cu.usbmodem*`.**

Build (firmware): `export PATH=<pyshim>:$PATH; . ~/src/esp/esp-idf/export.sh; cd firmware;
PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build` for the debug image (every task below is
verified on the debug image — `sleeptest` exists only there). Host tests:
`make -C firmware/host test` — every firmware task must leave them green.

Vocabulary of the `sleeptest` report (`modes.c:1118-1205`), used by every Verify below:
`window … asleep N s (P%)`; per-bucket `avg … ms/wake, max … ms` for the seven buckets
(`post-wake yield`, `input+ui+render`, `mqtt status/retry`, `msg_pump+health`, …); `cycle N:
net_sleep … awake before it … ms`; event lines `page <id> received, N s after the relay stamped
it`, `MQTT session LOST (rc=…)`, `MQTT session (re)connected`; `modem counters: datatx_retx=…
prompt_orphan=… buf_drop_queue=… buf_drop_pool=…`; `publish ring (… entries…)` with
`topic_tail  len  outcome  ms`.

Order: **S0 → S1 → S2 → S3 → S4 → S6 → S8 (bench, 15 min) → S7 (bench, the phase-1 gate) →
S5 → S9**. S0 before everything: until the report's arithmetic is trustworthy, no acceptance
criterion below can be read.

**23 Sep update — S0-S4, S6, S8 are done; S7 ran and failed 4 of its 6 criteria.** Read
`docs/SLEEP_URC_DESIGN.md` §8 before touching anything below. New order for what is left, smallest
first: **S11 (done, in this commit) → S12 → S10 → S13 → S7b (the new gate) → S14 → S5 → S9**.
S8 retired §3(b) and answered the RI question (§8.7); S7's old criteria 1 and 3 are replaced by
S7b's.

**23 Sep (late) update — S7b ran and is VOID: the window went offline, not slow** (four
`MQTT session LOST (rc=-13)`, no page, and ~93 s of its `asleep` total was a modem reset;
`build/bench-logs/phaseAI-report.log`). Root cause: S11 redefined `probe_stuck` to include a cheap
2 s probe timeout while leaving `check_probe_stuck_escalation()` (`modes.c:1635`) reading six of
them as "the modem is wedged" → F4. **Read `docs/SLEEP_URC_DESIGN.md` §9 first.** Order for what
is left: **S10 → S17 → S13 → S12 → S15 (gated on S10) → S7c (the new gate) → S16 → S14 → S5 → S9.**
§9.4's four edits (`net_probe_guard.{c,h}`, `net.cpp`, `net.h`, `modes.c`, plus the host test) are
already in the tree, uncommitted, and are part of the S7c image.

**24 Sep update — S7c ran: the sleep half PASSED and the delivery half failed completely**
(95% asleep, 69/69 timer wakes, zero session losses, zero resets, all modem drop counters 0 — and
**no page received**, neither of the two; `build/bench-logs/phaseAK-report.log`). Sleep policy is
done; delivery is not. **Read `docs/SLEEP_URC_DESIGN.md` §10 before touching anything below** — it
retires three numbers this file has been quoting (`probe_first_attempt_ms` is a cross-probe
artefact; FreeRTOS ticks do not advance across light sleep, so every library timeout is in *awake*
time; `ESP_LOGD` was compiled out, so no image has ever been able to print an AT trace). Owner's
ruling, 24 Sep: *"3 s is okay honestly"* — hold the wake window open until the probe is answered and
lengthen the cadence; WAKE0/IO46 is a later optimisation. Order for what is left:
**S18 (done, in the tree) → S7d (the new gate) → S14 → S16 → S5 → S9.** S15's gate is open
(`rsp_no_cmd=3`) but it is **deprioritised**: §10.2(B) makes the orphaned-`OK` count a predicted
consequence of a 2 s budget under a 200 ms window, so re-read `rsp_no_cmd` from S7d's window before
patching the library.

---

## S0 — the sleeptest report mis-attributes awake time — firmware-dev, timebox 1 h

**Read:** `docs/SLEEP_URC_DESIGN.md` §1 item 4; `firmware/main/modes.c:996-1006` (`ST_MARK_BEGIN`/
`ST_MARK`), `:1925-1982` (the sleep/yield block, where `ST_MARK_BEGIN()` sits at `:1942` and
`ST_MARK(0)` at `:1980`), `:1138-1141` (the `avg` print), `:2413-2470` (`ST_MARK(3..6)`);
`build/bench-logs/phaseAD-report.log:3-10` (the impossible `avg 870 ms / max 198 ms` pair).
**Files:** `firmware/main/modes.c` only.
**Do:** bucket 0 is charged both the post-wake yield of sleeping iterations *and* the whole
`ST_MARK(6)`→`ST_MARK(0)` span of iterations that did not sleep (loop top, the skipped
`net_sleep()`, the 20/100 ms poll delay), and every bucket's `avg` divides by `s_st_sleeps`.
Make the report readable: (a) split bucket 0 into `post-wake yield` (sleeping iterations only) and
a new `awake-loop (no sleep)` bucket; (b) print `avg` over the number of iterations that actually
contributed to that bucket, not `s_st_sleeps`, and add that count to the line; (c) add one summary
line `awake total N s over M iterations (K without a sleep); longest single iteration L ms`.
Do not change any behaviour, any threshold, or the cycle list. Keep the added text inside
`s_st_text`'s 2400-byte budget (raise it to 2800 if needed and say so in the commit message).
**Verify:** host tests green. Re-read `phaseAD-report.log` by hand and state in the commit message
the three numbers the new format would have printed for that run (awake 210 s, 69 sleeps,
~6.6 stalls of ~30 s). Bench proof comes free with S7: no bucket may print `avg` > `max`.

## S1 — per-wake URC drain probe (async bare `AT`) — firmware-dev, timebox 3 h

**Read:** `docs/SLEEP_URC_DESIGN.md` §3(a) and §5; `firmware/main/modes.c:1925-1982` (where the
probe goes: first statement after `net_sleep()` returns, before the yield), `:1524-1547`
(`run_modem_health_check()` — what the probe must NOT be), `:2395-2405` (its every-60th-wake gate);
`firmware/main/net.cpp` `net_sleep()` and `net_check()` (`:1210-1220`);
`firmware/main/net.h` (`net_publish_in_flight()`, `net_modem_busy()`);
`firmware/components/dptechnics__walter-modem/src/WalterModem.cpp:5099-5103` (`checkComm`),
`src/WalterDefines.h:311-321` (`_returnAfterReply` returns immediately when `cb != NULL`),
`src/WalterModem.h:127,3501` (queue and pool are 8 slots).
**Files:** `firmware/main/net.cpp`, `net.h`, `firmware/main/modes.c`, a host test under
`firmware/host/`.
**Do:** add `bool net_urc_probe(void)` in `net.cpp`: issue **one** `WalterModem::checkComm(NULL,
cb, NULL)` — asynchronous, one `AT`, no retries, no `net_check()` registration semantics, no
watchdog interaction, no logging in the callback (it runs on the library's task: it may only set a
flag and a timestamp). Guards, all cheap RAM reads: skip if a probe is still outstanding; skip if
`net_publish_in_flight()` (a modem parked at a `>` prompt would consume `AT\r\n` as payload); skip
on the WiFi transport; skip if the modem is not begun. Never queue more than one probe at a time.
If the probe has been outstanding for more than 3 wake intervals, count `probe_stuck`, clear the
flag and allow probing again; after 6 consecutive stuck probes, report it through the existing F4
path (`rate_limited_modem_recover()` via `run_modem_health_check()`'s owner) — do not add a second
recovery mechanism. Export `net_get_probe_counters()` → `{issued, answered, stuck, noqueue}` and
print it in the sleeptest report next to the modem counters. Call it from `modes.c` on **every**
wake in both ACTIVE and SLEEP mode (not under the `mode == SLEEP` / `loc_suppress` /
`ca_apply_suppress` / `coverage_owns_radio` gates the health check uses — a bare `AT` says nothing
about registration and resets nothing). Also add, in the same report, the RCA_SLEEP_URC fix 1
discriminator: `uart_get_buffered_data_len()` sampled 50 ms after each wake → max bytes and the
number of wakes with > 0.
**Verify:** host tests green, including one for the guard state machine (in-flight, stuck-after-3,
no double queue). Bench: S7. In the commit message state the measured cost: the probe rides inside
the existing 200 ms yield, so 0 ms of added window and 0 air bytes.

## S2 — the liveness verdict must not fire on a SUBACK lost in a URC flush — firmware-dev, 2 h

**Read:** `docs/SLEEP_URC_DESIGN.md` §6 ("SUBACK collision");
`firmware/main/xport_lte.cpp:549-632` (`lte_service_session()`: the connect watchdog at `:560-572`,
the no-SUBACK-in-30 s branch at `:580-601`, the re-SUBSCRIBE at `:613-631`), `:274-302` (the
SUBSCRIBED handler that clears `s_resub_wait`), `:324+` (the MESSAGE handler);
`build/bench-logs/phaseAB-report.log:87-94` (page at +441 s, `LOST (rc=0)` at +446 s).
**Files:** `firmware/main/xport_lte.cpp`, `xport_lte.h`/`net.h` if an accessor is needed, host test.
**Do:** (i) record the timestamp of the last successfully ingested `/down` message; in the
no-SUBACK branch, if that timestamp is newer than `s_resub_sent_us`, clear `s_resub_wait` (and
`s_resub_is_resume`), log one line saying the downlink proved the session alive, and return —
**no verdict, no teardown**. (ii) Otherwise re-send the re-SUBSCRIBE once and only declare the
session dead if the second one also goes unanswered for 30 s; keep a counter so the report shows
how often the first ping was swallowed. Do not touch the connect watchdog. Keep the existing M3
ordering (`lte_session_down()` inside the branch that makes the call) for the real verdict.
**Verify:** host tests for: page-after-ping suppresses the verdict; no page and one unanswered ping
does not; two unanswered pings do. Bench: S7 — the report must show two `page … received` lines and
**zero** `MQTT session LOST` lines.

## S3 — name the stalled command; per-command timeout — firmware-dev, 3 h

**Read:** `docs/SLEEP_URC_DESIGN.md` §6 ("Ack stall"), `docs/RCA_SLEEP_URC.md` §5 fix 3-4,
`docs/RCA_SLEEP_PUBLISH.md` §1-§2; `firmware/components/dptechnics__walter-modem/src/
WalterModem.cpp:959-970` (`_uartWrite`, the ignored `uart_wait_tx_done(10 ms)`), `:2281-2305` (the
prompt handler and its bare `uart_write_bytes()` of the payload), `:1763-1790` (the DATA_TX_WAIT
retransmit, patch 1.12), `PATCHES.md` (1.1-1.12 — append 1.13 and mark the source with
`// PAGER PATCH: 1.13`); `build/bench-logs/phaseAB-report.log:101` and `phaseAD-report.log:29`
(the two 30 010 ms `/up` entries).
**Files:** `firmware/components/dptechnics__walter-modem/src/WalterModem.cpp`, `WalterModem.h`,
`PATCHES.md`, `firmware/main/net.cpp` (counter export), `firmware/main/modes.c` (report line).
**Do:** (1) **Attribution.** Export, through the existing `net_pager_counters_t`: `prompt_handled`
(incremented at the prompt handler), `payload_bytes_written`, `txdone_timeouts` (keep
`uart_wait_tx_done()`'s return and count failures), and, for the last command that exceeded 5 s,
`{first 24 chars of the AT command line, elapsed ms, CTS pin level at the stall, bytes still queued
in the UART TX ring}`. Print that as one `stalled command:` line in the sleeptest report — this is
the line RCA_SLEEP_URC fix 3 asks for. (2) **Patch 1.13:** a per-command timeout and per-command
attempt count on `WalterModemCmd` (default unchanged: `CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS`,
`WALTER_MODEM_DEFAULT_CMD_ATTEMPTS`), and use 10 s / 2 attempts for the MQTT publish, subscribe,
disconnect and config commands only. Leave attach/TLS/connect at 30 s / 3. Do not change the
retransmit behaviour patch 1.12 introduced.
**Verify:** host tests green. Bench: S7's report must carry a `stalled command:` line if any
bucket max exceeds 5 s, and any `/up` entry in the publish ring that is not `OK … ≤ 100 ms` must
now read `≤ 20 010 ms` instead of `30 010 ms`. Quote the line in the report.

## S4 — do not publish an ack inside the post-wake drain — firmware-dev, 1 h

**Read:** `docs/SLEEP_URC_DESIGN.md` §6 ("Ack stall") fix (i); `firmware/main/modes.c:2386-2389`
(the `msg_pump()` call site), `firmware/main/msg.c` `publish_ack()`; S1's 50 ms post-wake byte
counter.
**Files:** `firmware/main/modes.c` (and `net.h` only if the byte-count accessor must be exported).
**Do:** if any bytes arrived from the modem UART during this iteration's post-wake window, skip
`msg_pump()` for this iteration only and pump on the next wake (≤ 5 s later in sleep mode, ≤ 2 s in
active mode — no requirement bounds ack latency). One flag, no timers, no new state. Comment the
power cost: zero; the cost is one wake cycle of ack latency.
**Verify:** host tests green. Bench: S7 — every `/up` entry in the publish ring reads `OK` with
`ms` in the tens, and the `page … received` latencies stay ≤ 30 s.

## S6 — watchdog vs command-timeout arithmetic — firmware-dev, 1 h (do with S3)

**Read:** `docs/SLEEP_URC_DESIGN.md` §6 ("Watchdog arithmetic");
`firmware/main/watchdog.c:25` (`WD_MODEM_BLOCK_BUDGET_MS 95000`), `:112-165` (`rtc_feed`,
`watchdog_feed`, `watchdog_kick` resetting the budget per stage, `walter_modem_block_tick`),
`firmware/main/watchdog.h`; `firmware/sdkconfig:1019` (task WDT 60 s) and `:1966`
(`CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS=30000`);
`firmware/components/dptechnics__walter-modem/src/WalterModem.h:142` (3 attempts);
`.overnight-handoff.md` "23 Sep 13:50" (the uncaptured phaseAA reboot).
**Do:** patch 1.10 already feeds both watchdogs inside the blocking wait — confirm that in the
commit message with the file:line, and record the arithmetic: one stalled command = 30 s × 3 = 90 s
< 95 s budget (survives); two in one stage = 180 s > 95 s → feeding stops and the task watchdog
reboots ~60 s later (the phaseAA candidate). With S3's 10 s × 2 for MQTT data/session commands the
worst stage is 4 × 20 s = 80 s < 95 s. Change **no** watchdog constant unless the arithmetic below
fails; instead add one `ESP_LOGI` naming the stage and the command whenever a single command exceeds
5 s (so the next reboot is attributable), and state in the commit message the worst-case seconds per
stage after S3.
**Verify:** host tests green. Bench: S7 must complete its window with no reboot other than the
deliberate end-of-window `watchdog_hard_reset()`, and the boot banner after it must read
`reset reason: … last stage: deliberate restart` (as in `phaseAB-report.log:32`).

## S8 — console experiments for the UNVERIFIED modem behaviour — bench-tester, 15 min, no code

**Read:** `docs/SLEEP_URC_DESIGN.md` §7; `docs/HARDWARE_TESTING.md` (the `at` console command and
the port rules).
**Do:** debug image on the pager, session up, one foreground capture to
`build/bench-logs/phaseAE-console.log`. In order, pasting the exact answers into the report:
1. `at AT&K?` — then `at AT+IFC?` — then `at AT&V`.
2. `at AT+SQNRICFG?` — then `at AT+SQNHWCFG?`.
3. `at AT+SQNSMQTTRCVMESSAGE=0,"pager/test-pager/down"` with nothing pending: answer and latency.
4. `relay/.venv/bin/python tools/bench/send_test_page.py test-pager "e3a"`, wait for the page to be
   shown, then run 3 again twice: does the no-`mid` form return a payload, and is the read
   destructive?
5. `at AT+SQNSMQTTPUBLISH=0,"pager/test-pager/up",1,44` then, after 5 s, `at AT` — does the modem
   answer `OK`, swallow the `AT`, or time the prompt out by itself (and after how long)? Then
   `at AT+SQNSMQTTDISCONNECT=0` and let the pager reconnect.
**Verify:** report each answer verbatim. These four answers gate S9 (and, if 1 exposes a
"modem ignores CTS" mode, retire S9's byte-loss problem). Do not change firmware.

## S7 — PHASE-1 ACCEPTANCE — bench-tester, one window, ~15 min

**Read:** `docs/SLEEP_URC_DESIGN.md` §4; the bench rules; `build/bench-logs/phaseAD-report.log` as
the baseline to beat.
**Do:** flash the debug image containing S0-S4 and S6. One foreground capture
(`build/bench-logs/phaseAF-report.log`). Console: `sleeptest 6` (build defaults — do **not** pass
yield/interval overrides). From the host, 90 s and 250 s into the window:
`relay/.venv/bin/python tools/bench/send_test_page.py test-pager "p1a"` and `… "p1b"`. The window
closes at 360 s, then a 25 s grace, the report is saved to NVS, the pager hard-resets and reprints
it at boot; capture that. Keep `tools/bench/poll_emqx_loop.py test-pager` running in a second log
so the relay-side acks are timestamped.
**Verify — all six, from the reprinted report:**
1. two `page p1a…/p1b… received, N s after the relay stamped it` lines with **N ≤ 30** each;
2. **zero** `MQTT session LOST` lines (and therefore zero `(re)connected` lines);
3. `asleep … (P%)` with **P ≥ 85** — the predicted band is 87-91% (after the first page the pager
   is ACTIVE at a 2 s cycle: 200/2200 = 9.1% of yields plus ~7 s of renders). A single 30 s stall
   is 8.3% and fails this line;
4. every bucket `max` ≤ 1000 ms except `input+ui+render` (a full refresh is up to 3.5 s); no bucket
   prints `avg` > `max` (S0);
5. publish ring: both `/up` 44 B entries `OK` with `ms` in the tens; `datatx_retx` and
   `prompt_orphan` both 0 — if `datatx_retx` is 1, quote the `stalled command:` line (S3);
6. `probe_issued ≈ number of wakes`, `probe_answered ≈ probe_issued`, `probe_stuck = 0`,
   `probe_noqueue = 0`; the 50 ms post-wake byte counter is > 0 on at least the two wakes that
   carried a page.
Also confirm at the relay that both pages were acked within 30 s of the stamp. **If 1-3 pass but
4-6 do not, phase 1 is still a pass** — report which line failed and stop; do not start S9.

## S5 — bound the reconnect after a host-side dead verdict — firmware-dev, 2 h (after S7)

**Read:** `docs/SLEEP_URC_DESIGN.md` §6 ("Reconnect stall");
`firmware/main/xport_lte.cpp:580-601` (the verdict + `lte_session_down()` at `:597`), `net.cpp`
`net_session_up()` / `configure_session()`, `firmware/main/modes.c:2195-2260` (the retry branch and
`handle_mqtt_loss()`); `build/bench-logs/phaseAB-report.log:72` (`mqtt status/retry max 81795 ms`).
**Do:** after a host-side dead verdict, tear the modem client down and **return** — do not let the
same wake iteration also run `configure_session()` + `mqttConnect()`; leave the reconnect to the
existing F1/F3 backoff on a later wake, by which time a probe has drained the queue. Combined with
S3's 10 s/2-attempt timeout, the worst case per attempt falls from ~90 s to ~20 s. Say in the
commit message whether M3's disconnect-first already covered this (it does not: it removes the
`+CME ERROR: 4` loop, not the timeouts).
**Verify:** host tests for the ordering (verdict → disconnect → return; reconnect only on a later
service call). Bench: force a dead verdict on a healthy session (the debug hook the M3 task asked
for, or block the SUBACK) and confirm from the log that `AT+SQNSMQTTDISCONNECT` precedes the next
`AT+SQNSMQTTCFG` **by at least one wake interval**, that `mqtt status/retry max` stays under
25 000 ms, and that `MQTT session usable` follows.

## S9 — PHASE 2 spike: modem-woken host (CTS asserted + UART wakeup) — firmware-dev then bench

Only after S7 passes and S8's answers are in. Behind a debug console flag (`sleeptest 6 … ` must
still run the phase-1 path by default) so one flash can do an A/B.
**Read:** `docs/SLEEP_URC_DESIGN.md` §3(c) and §7 E5; `docs/RCA_SLEEP_URC.md` §4 and §5's closing
paragraph; `firmware/main/net.cpp` `net_sleep()` (the RTS choreography and `gpio_sleep_sel_dis`);
`firmware/components/dptechnics__walter-modem/src/WalterModem.cpp:5054-5097` (the vendor's own
`sleep()`, same RTS defect); IDF `esp_sleep_enable_uart_wakeup()` /
`uart_set_wakeup_threshold()` docs.
**Files:** `firmware/main/net.cpp`, `net.h`, `firmware/main/modes.c`, and — only if the spike
confirms clipping — the library parser plus `PATCHES.md`.
**Do:** a flag that, for one sleeptest window, leaves RTS **asserted** through light sleep, keeps
the RX pad alive across sleep (`gpio_sleep_sel_dis(RX)` — the `SLP_IE=0` isolation applies to RX
too; **UNVERIFIED** whether IDF 5.2.1 exempts a UART-wake pin, so try it both ways and say which
worked), enables `esp_sleep_enable_uart_wakeup(PAGER_MODEM_UART, 3)`, and lengthens the timer
interval to 30 s for that window. Count: wakes by cause UART, unparseable/clipped lines, pages
delivered and their latency. Do **not** write the parser resync patch until the spike shows the
clipping is real and shows what it looks like.
**Verify:** one `sleeptest 6` window with two mid-window pages: report `wakes: N timer / M other`
with M ≥ 2, page latency ≤ 30 s, `asleep ≥ 97%` (200 ms per 30 s cycle), and the clipped-line
count. Predicted saving if it holds: 5 s → 30 s cadence ≈ −35 to −45 mAh/day (95-107 → ~60).
Report the clipped-line count even if delivery works — that number decides whether phase 2 needs
(b)'s explicit `AT+SQNSMQTTRCVMESSAGE` read as its content path.

---

# 23 Sep — post-S7 tasks

Every task below names the **report line that proves it**. Smallest first. `docs/SLEEP_URC_DESIGN.md`
§8 holds the reasoning and the file:line evidence; do not re-derive it.

## S11 — the probe's budget (DONE in this commit, no bench yet) — 30 min

**Proof line:** `stalled command: "AT" elapsed=30000 ms cts=0 tx_ring_free=0 B` and
`probe counters: probe_issued=13 probe_answered=13 probe_stuck=8 probe_noqueue=0`
(`build/bench-logs/phaseAF-report.log:27-28`), plus `... /up 44 B OK 29824 ms` (`:33`, `:41`).
**Done:** `checkComm()` gained two optional pass-through parameters (PATCHES.md 1.14,
`WalterModem.cpp:5189-5194`, `WalterModem.h:4477-4479`); `net_urc_probe()` passes 1 attempt / 2 s
(`net.cpp`, `PAGER_URC_PROBE_ATTEMPTS`/`PAGER_URC_PROBE_TIMEOUT_MS`); `probe_cb()` now counts a
non-OK, non-NO_MEMORY result as `stuck` via the new `net_probe_guard_failed()` instead of lying about
`noqueue`. Host test added. Defaults unchanged for every other caller, including `net_check()`.
**23 Sep (late) correction — that last sentence was the S7b regression.** Counting a 2 s timeout as
`stuck` collided with `check_probe_stuck_escalation()` (`modes.c:1635-1653`), which resets the modem
after six consecutive `stuck` — twelve seconds in ACTIVE mode (`docs/SLEEP_URC_DESIGN.md` §9.2).
It now counts a separate `timedout`; the budget itself (1 attempt / 2 s) is unchanged and is still
unverified on the bench. **Still to verify:** S7c, and S16 for the budget A/B. Expected:
`stalled command:` either absent or `elapsed=2000 ms`; no `/up` entry above ~2 100 ms;
`awake-loop (no sleep)` under 10 s; `probe_timedout` may be large, `probe_stuck` must not be.

## S12 — the two 15 s display BUSY bounds, and the gate that fails open — firmware-dev, 1 h

**Proof line:** `input+ui+render avg 45 ms/wake, max 31551 ms` (`phaseAF-report.log:6`) against
`phaseAF-boot.log:61-63` (`BUSY: entry=1 timed out after 1502 iters (~15009 ms)`, retry 9 230 ms)
and `:68` (`BUSY line never asserted; using fixed waits (check the IO18 wire)`).
**Read:** `docs/SLEEP_URC_DESIGN.md` §8.3; `firmware/main/disp.c:22` (`PAGER_UI_BUSY_TIMEOUT_US`),
`:152-213` (`disp_wait_busy_fb()`), `:460-466` and `:586-600` (the timeout + one-retry paths),
`:124-126` (the fallback constants); `firmware/main/ui.c:536-560` (the pre-write gate and its
1 500 ms bound).
**Files:** `firmware/main/disp.c`, `firmware/main/ui.c`, host test if the bound becomes a pure helper.
**Do:** (a) cut `PAGER_UI_BUSY_TIMEOUT_US` from 15 s to a value justified by the measured refresh
times already in the file (full 3 426 ms, partial 455 ms — `disp.c:125-126`): 6 s leaves >70% margin
on the slowest measured refresh and caps the worst case at 1.5 + 6 + 6 = 13.5 s instead of 31.5 s.
State the mAh in the commit message: 18 s × 40 mA = 0.2 mAh per occurrence. (b) Count BUSY timeouts
and expose the count in the sleeptest report, so "the panel wedged" is never again inferred from a
bucket max. (c) Log one line when `disp_pre_write_gate_hook()` spends its **whole** 1 500 ms budget
and starts the refresh anyway — that is the 23 Sep corruption gate failing open, and today it is
silent. Change **no** panel command, no refresh cadence, and do not touch `mark_display_dead()`.
**Do not** raise the gate's 1 500 ms bound: with S11 in place a publish should never be in flight
that long, and if it is, S10 wants to know.
**Verify:** host tests green. S7b: no bucket max above 14 000 ms; the new BUSY-timeout counter is
printed. The IO18 wire itself is an owner hardware call — report it, do not act on it.

## S10 — decide the stall mechanism with three counters, not a trace — firmware-dev, 2 h

**Proof line:** `stalled command: "AT" elapsed=30000 ms` with `txdone_timeouts=0`,
`buf_drop_queue=0 buf_drop_pool=0` and `probe_answered=13` — i.e. the bytes went out, nothing was
dropped, and the retry was answered (`phaseAF-report.log:25-28`). §8.1 rules out CTS and the TX
path; §8.2 lists the three surviving hypotheses. There is no in-window AT trace and there will not
be one (light sleep kills the USB CDC, `modes.c:2021-2023`), so this is counters.
**Read:** `docs/SLEEP_URC_DESIGN.md` §8.1-8.2 in full; `WalterModem.cpp:1650-1706`
(`_cmdProcessingTask`: one FIFO for commands *and* response buffers, `_processModemRSP(_curCmd, …)`
at `:1660`), `:4002-4016` (completion = "buffer starts with `atRsp`", almost always `"OK"`),
`:1444-1562` (`_parseRxData`), `:1408-1439` (`_expectingPayload()`), `:1459-1467` and `:1890-1897`,
`:1986-1993` (where `_receivingPayload` is set and cleared); `WalterDefines.h:85-131`.
**Files:** `firmware/components/dptechnics__walter-modem/src/WalterModem.cpp`, `WalterDefines.h`,
`PATCHES.md` (append 1.15), `firmware/main/net.cpp` + `modes.c` (report line only).
**Do:** three counters in `walter_modem_pager_counters_t`, printed on one new report line:
(1) `rsp_no_cmd` — buffers reaching `_processModemRSP()` with `cmd == NULL` and `result == OK` (the
ones that fall through to `:4023` and are freed unused). Hypothesis 1 (desync) predicts >= 1 per
stall; 2 and 3 predict 0. (2) `payload_stuck_ms` — how long `_receivingPayload` had been true when a
command timed out at `:1897`/`:1992` (needs one `TickType_t` remembering when it was set at `:1509`).
Hypothesis 2 predicts > 0. (3) `probe_first_attempt_ms` — the elapsed of the probe's attempt 1 and
of its answer; hypothesis 3 predicts both slow, 1 and 2 predict the retry is fast. **Counters only —
change no behaviour.** Do not attempt a fix in this task; the fix depends on the answer.
**Verify:** host tests green. S7b's report carries the new line. Say in the commit message which
hypothesis the numbers support, and stop there.

## S13 — the oversize-message payload-length bug — firmware-dev, 30 min

**Proof line:** none in `phaseAF` (no oversize message occurred) — this is a read-found latent bug,
recorded so it is not re-found the hard way.
**Read:** `docs/SLEEP_URC_DESIGN.md` §8.2 hypothesis 2; `firmware/main/xport_lte.cpp:379-390` (the
oversize drain, which passes `sizeof(s_mqtt_rx_buf)`) against `:393` (the normal path, which passes
`data->msg_length`); `WalterModem.cpp:1408-1439` (`_expectingPayload()` takes the byte count from the
command line's own requested size) and `:1459-1467`.
**Files:** `firmware/main/xport_lte.cpp`, host test not applicable (no host build for this path —
say so).
**Do:** pass `data->msg_length` on the oversize drain too, capped at `sizeof(s_mqtt_rx_buf)`, so the
count the parser is told to expect matches what the modem will actually send. As written, an oversize
message makes `_receivingPayload` stick true until the next command timeout, swallowing every
response line in between — the same 30 s class of stall S7 measured, but deterministic.
**Verify:** host tests green; describe the reasoning in the commit message. Bench: optional — send a
>640 B page and confirm `oversize MQTT message dropped` is followed by ordinary traffic, not a
`stalled command:` line.

## S7b — PHASE-1 ACCEPTANCE, second attempt — bench-tester, one window, ~15 min

**RUN AND VOID, 23 Sep 22:2x (`build/bench-logs/phaseAI-report.log`).** The image was bc5e023,
i.e. S11 only — S12 and S13 were never in it — and the window went offline instead of slow: four
`MQTT session LOST (rc=-13)`, no reconnect, no page. Read `docs/SLEEP_URC_DESIGN.md` §9 before
re-running anything; the re-run, its new bar 8 and its flash order are **S7c** below. Bars 1-7 as
written here still stand.

Replaces S7. Same procedure (`sleeptest 6`, pages at 90 s and 250 s, relay poll in a second log,
capture the post-reset reprint), with S11-S13 flashed and **honest bars**:
1. `asleep … (P%)` with **P >= 85**; predicted band **86-90%** (`docs/SLEEP_URC_DESIGN.md` §8.5's
   duty table: 4.4% in SLEEP, 10.4% in ACTIVE, ACTIVE lasts 10 min after each page).
2. `awake-loop (no sleep)` **under 10 s** — this bucket is the stall detector now (it was 73 s).
3. `stalled command:` absent, or present with `elapsed=2000 ms` (the probe's new budget). No publish
   ring entry above ~2 100 ms.
4. `probe_stuck` and `probe_noqueue` reported separately and interpreted per S11 (`stuck` now also
   counts a 2 s timeout, which is cheap and expected; it is no longer a failure signal on its own).
5. zero `MQTT session LOST` (S2 held in S7; it must keep holding).
6. no bucket `max` above 14 000 ms (S12), and the new BUSY-timeout counter printed.
7. page latency: report it, **do not gate on 30 s** — §8.6 shows 46-47 s is 2 × the 20.48 s eDRX
   cycle and is not a host fix. Record the two numbers; S14 owns the bar.
**If 1-6 pass, phase 1 is done** regardless of 7.

## S14 — the delivery bar is an eDRX decision, not a firmware one — architect + owner, 1 h

**Proof line:** `+ 140 s page m_c5ced4cc received, 47 s after the relay stamped it` and
`+ 303 s … 46 s …` (`phaseAF-report.log:23-24`), with `AT+SQNEDRX=2,4,"0010","0001"`
(`phaseAF-boot.log:106`) = eDRX value 2 = 20.48 s, PTW 2.56 s, `+CPSMS: 0`.
**Read:** `docs/SLEEP_URC_DESIGN.md` §8.6 and §2's delivery budget; `docs/PROTOCOL.md` §8.2-8.3.
**Do:** no code first. One bench window with `AT+SQNEDRX=2,4,"0001","0001"` (10.24 s) set from the
console before `sleeptest`, two pages, and report both latencies — the two-cycle worst case should
fall from ~46 s to ~26 s. Then the owner decides between (a) eDRX 10.24 s, paying whatever the
modem's extra paging occasions cost (**UNVERIFIED — no current trace exists for this; that is the
measurement this task is really asking for**), and (b) amending `docs/PROTOCOL.md` to
"≤ 30 s typical, ≤ 50 s worst case" for sleep mode. Do not change the eDRX default in firmware
until that ruling exists.
**Verify:** the two latency lines from each configuration, side by side, and one sentence of owner
ruling recorded in `docs/PROTOCOL.md`.

## S9 — PHASE 2, amended by S8's answers

Unchanged in intent (see above), with two corrections from `docs/SLEEP_URC_DESIGN.md` §8.7:
- §3(b) is **dead** as the content-recovery path: `AT+SQNSMQTTRCVMESSAGE=0,"<topic>"` with no `mid`
  answers `+CME ERROR: 4` (`phaseAF-s8.log:49-53`). If UART-wake clipping is real, the only fix is a
  parser resync patch.
- **Ask the owner about the RI line first.** `+SQNRICFG: 1,3,1000` (`phaseAF-s8.log:27-31`) says the
  modem has a ring indicator, enabled, 1 000 ms pulse; `firmware/main/pins.h` routes no modem→ESP
  wake pad. Wiring it turns phase 2 into "wake on RI (ext0/ext1), then one `AT` to flush" with **no**
  byte loss and **no** parser risk, and retires this task's whole clipping problem. One question,
  potentially days saved.

---

# 23 Sep (late) — post-S7b tasks

`docs/SLEEP_URC_DESIGN.md` §9 holds the reasoning and the file:line evidence; do not re-derive it.
The S7b regression itself is **fixed in the tree, uncommitted** (§9.4's four edits: the
`stuck`/`timedout` split, the escalation's underflow, `s_modem_begun` on a failed reset, and four
new report fields). **S10 is now the highest-priority task in this file** — §9.2's one remaining
open item is the response/command desync, and S10 is what settles it. Smallest first below.

## S17 — the F4 reset's own 90 s failure mode (`+SYSSTART` discarded) — firmware-dev, 1 h

**Proof line:** `asleep 253 s` against `80 light sleeps` at a 2 s interval = 160 s of real sleep
(`build/bench-logs/phaseAI-report.log:3`), i.e. ~93 s spent awake inside the block `modes.c:2043-2060`
charges to sleep; 1 s + 3 × 30 s = 91 s is `WalterModem::reset()` waiting for a `+SYSSTART` it threw
away (`docs/SLEEP_URC_DESIGN.md` §9.2 item 3).
**Read:** §9.1-9.2; `WalterModem.cpp:5073-5086` (`reset()`: pin pulse, `vTaskDelay(1000)`, then a
`TX_WAIT` on `"+SYSSTART"` with the library's 3 × 30 s default), `:1444-1450` (`_parseRxData()`
returns immediately while `_hardwareReset` is true — every byte discarded), `:1856-1877` and
`:1965-1973` (attempts/timeout); `firmware/main/net.cpp` `net_recover_modem()`.
**Files:** `firmware/components/dptechnics__walter-modem/src/WalterModem.cpp`, `PATCHES.md`
(append 1.15 or 1.16 — check which number S15 took), `firmware/main/net.cpp` only if the call site
needs a shorter budget.
**Do:** two independent halves, both small. (a) Stop discarding the banner: `_hardwareReset` exists
to drop the garbage a reset pin pulse puts on the line, so clear it as soon as the pin is released
and the line has settled rather than after the 1000 ms delay *and* the command queue — or, simpler
and provably safe, keep discarding but recognise a `+SYSSTART` that arrives during the window by
setting a `_sawSysStart` flag the queued command consults on its first evaluation. (b) Give the
reset's `+SYSSTART` wait a budget that matches the modem's datasheet boot time rather than 3 × 30 s
(patch 1.13/1.14 already made attempts/timeout per-command): the GM02SP boots in ~2-3 s, so 1
attempt / 10 s fails **80 s earlier** with no loss of function, and `net_recover_modem()`'s caller
already handles a false return. Do (b) even if (a) turns out to be wrong — it is the cheap half.
**Power effect to state in the commit message:** 80 s × 40 mA ≈ 0.9 mAh per failed F4, and the F4
is rate-limited to 6/hour, so the worst case this removes is ~5 mAh/h. Estimated (§2), not measured.
**Verify:** host tests 21/21 (no host build for the library — say so). One bench boot: `net_recover`
from the console (or `sleeptest` with a forced escalation) and confirm from the live log that the
reset completes in seconds and `s_modem_begun`/the probe come back. Then S7c.

## S15 — the library's response pairing: refuse an answer that predates the question — firmware-dev, 2 h, GATED

**Gate:** do **not** start until S10 has run one window and `rsp_no_cmd` is **non-zero**. If it is
zero, hypothesis 1 is dead and this task is closed unstarted — say so and stop.
**Proof line (S7b):** `stalled command: "AT+SQNSMQTTCONNECT=0,"s1" elapsed=30000 ms cts=0` with
`buf_drop_queue=0 buf_drop_pool=0 txdone_timeouts=0` — nothing was dropped and the bytes went out,
so a 30 s wait for an `OK` means the `OK` went to someone else
(`docs/SLEEP_URC_DESIGN.md` §8.2 hypothesis 1, still open per §9.2's closing note).
**Read:** `docs/SLEEP_URC_DESIGN.md` §8.1-8.2 and §9.2; `WalterModem.cpp:1650-1706`
(`_cmdProcessingTask` — one FIFO for commands *and* response buffers, `_processModemRSP(_curCmd, …)`
at `:1660`, `_curCmd` cleared and the next command popped at `:1694-1706`), `:1856-1877`
(`attemptStart` is stamped at *transmit*, not at queue time), `:2387-2426` (`ERROR`/`+CME ERROR`/
`+CMS ERROR` set `result` and do **not** consult `atRsp`), `:4002-4016` (the completion test),
`:1176-1203` (`_queueRxBuffer`), `WalterDefines.h:85-131`.
**Files:** `firmware/components/dptechnics__walter-modem/src/WalterModem.cpp`, `WalterDefines.h`,
`PATCHES.md`. No `firmware/main/` change.
**Do:** stamp each response buffer with a monotonic tick in `_queueRxBuffer()` (one `TickType_t`
field on `WalterModemBuffer`, set immediately before the `xQueueSend`), and in
`_processModemRSP()`'s completion test at `:4011` require that stamp to be **>= `cmd->attemptStart`**
before either arm (`atRsp` match *or* `result != OK`) may finish the command. A buffer already
sitting in the queue when the current attempt was transmitted cannot be its answer; free it and
count it instead. Add one counter `rsp_predates_cmd` next to S10's, on the same report line. Do
**not** change the URC dispatch paths, do **not** change what happens when `cmd == NULL`, and do
**not** touch the retry/timeout arithmetic — a command whose answer is now correctly refused simply
times out one attempt later, as it does today.
**Power effect to state in the commit message:** none directly; it removes 30 s command stalls, each
~0.33 mAh of awake ESP plus up to 45 s of sleep-hold (§9.4's arithmetic; 40 mA awake / 1 mA asleep,
estimated).
**Verify:** host tests 21/21 (no host build for this file — say so). One boot: identity attaches, CA
pins, `MQTT session usable` inside 3 s, `rsp_predates_cmd` printed. Then S7c. **Revert immediately
if any boot-time command starts timing out** — that is the patch refusing a legitimate answer, i.e.
the stamp or `attemptStart` is wrong.

## S16 — probe budget: 2 s vs the old 30 s, A/B on one flash — firmware-dev + bench-tester, 1 h

**Proof line:** phaseAF (30 s budget) = 45% asleep, `stalled command: "AT" elapsed=30000 ms`, two
`/up` acks at 29 824 ms, **zero session losses**; phaseAI (2 s budget) = a modem reset, four
`MQTT session LOST (rc=-13)` and no page. Two runs, three differences (the budget, ACTIVE vs SLEEP
interval, and §9.4's fixes), so neither budget is attributable yet.
**Read:** `docs/SLEEP_URC_DESIGN.md` §9.4 option (b) and §8.4; `firmware/main/net.cpp`'s
`PAGER_URC_PROBE_ATTEMPTS`/`PAGER_URC_PROBE_TIMEOUT_MS` and `net_urc_probe()`; `PATCHES.md` 1.14.
**Files:** `firmware/main/net.cpp`, `modes.c` (console command only).
**Do:** make the two constants a runtime pair settable from the existing debug console (default
unchanged: 1 attempt / 2 s), so **one flash** runs `sleeptest 6` twice — once at 1/2 s, once at
3/30 s — with everything else identical, including the wake interval (pass `sleeptest 6 0 5000` both
times so ACTIVE vs SLEEP cannot be the hidden variable). Print the pair in the report header next to
`yield=`/`interval=`. No other behaviour change.
**Verify:** the two reports side by side: asleep%, `awake-loop (no sleep)`, `stalled command:`,
`probe_timedout`, `probe_stuck`, `MQTT session LOST` count, `modem_resets`, and the page latencies.
Recommend a default in one sentence with the mAh/day difference (§8.5's duty table converts asleep%
to mA). Expect S15, if it lands, to make the difference small — which is itself the answer.

## S7c — PHASE-1 ACCEPTANCE, third attempt — bench-tester, one window, ~15 min

**Flash order (all of it, then the window):** S10 → S17 → S13 → S12 → §9.4's four `net.cpp`/
`modes.c`/`net_probe_guard.*` edits (already in the tree, uncommitted). S15 only if its gate opened.
**Do:** exactly S7b's procedure — `sleeptest 6`, pages at 90 s and 250 s, relay poll in a second
log, capture the post-reset reprint.
**Bars:** S7b's 1-7, with bar 4 now reading `probe_timedout` (cheap and expected) rather than
`probe_stuck`, plus:
8. `probe_issued` > 0 and `probe_skip_down` ≈ 0 — the drain probe was alive for the window being
   scored. `probe_stuck` > 0 is now a real fault signal again; report it.
9. `modem_resets` = 0. A window containing an F4 is **void, not failed** — ~93 s of its `asleep`
   total is not sleep (§9.1), so nothing in it can be scored. Re-run, and report the F4 separately.
**Verify:** if 1-6, 8 and 9 pass, phase 1 is done regardless of 7 (latency is S14's eDRX call).

## S18 — hold the wake window open until the drain probe is ANSWERED — DONE, in the tree, uncommitted

**Why:** `docs/SLEEP_URC_DESIGN.md` §10.1. The modem releases a held page URC only when it accepts
a command, and it does not accept one inside the 200 ms post-wake yield. Every 200 ms-window run
delivered nothing (phaseAB, phaseAC, phaseAK); the one run that stayed awake seconds delivered in
10 s (phaseAD).
**Files touched:** `firmware/main/modes.c`, `net.cpp`, `net.h`, `modes.h`, `main.c`,
`firmware/sdkconfig.defaults`. No library patch.
**What it does:**
1. `modes.c:2264` `wait_for_probe_answer(interval_ms)` (helper at `:1772`), after the post-wake
   yield and after the sleeptest bookkeeping so the time is charged to `post-wake yield`, not to
   `asleep`. Polls `net_urc_probe_in_flight()` every 20 ms, bounded by `PAGER_PROBE_WAIT_MS`
   (4 000, `modes.c:152`), armed only when `interval_ms >= PAGER_PROBE_WAIT_MIN_INTERVAL_MS`
   (10 000, `:163`). **With the shipped 5 000/2 000 wake constants that gate is closed, so release
   behaviour is unchanged.** No watchdog kick inside the loop (bounded well inside the 95 s stage
   budget, and kicking would reset it for a caller making no progress).
2. `net.cpp:1271` `PAGER_URC_PROBE_TIMEOUT_MS` 2 000 → 6 000, still 1 attempt: a library budget
   shorter than the host's wait structurally orphans the answer (phaseAK `probe_timedout=10`,
   `rsp_no_cmd=3`).
3. `net.cpp:1418`/`net.h:451` `net_urc_probe_in_flight()`; `answer_ms last/max/n` recorded only on
   the answered branch, on a new `probe answer:` report line with the wait's own avg/max/n/gaveup.
4. `main.c` `sleeptest <min> [yield_ms] [interval_ms] [probe_wait_ms]` — 4th argument, debug only.
5. `sdkconfig.defaults` `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` — §10.2(C): the AT trace is `ESP_LOGD`
   and was compiled out of every image ever built. Runtime default stays INFO. Measured cost
   **+21 kB flash release, +35 kB debug**, no timing or power change.
**Power effect (estimated, §2's 40 mA/1 mA, not measured):** zero in any released image; at the
experiment's T=20 s + 3 s answer it is 14% duty ≈ 8.0 mA ≈ **191 mAh/day** against today's
96 mAh/day — see §10.3's table, which is the point of the experiment.
**Not done deliberately:** `PAGER_WAKE_INTERVAL_SLEEP_MS` is still 5 000. The cadence is S7d's
output, not its input.
**Verified:** host tests **21/21**; both images build (debug `0x139490`, release `0xa2040`).

## S7d — PHASE-1 ACCEPTANCE, fourth attempt — bench-tester, one window, ~12 min

**Image:** debug (`PAGER_DEBUG_NO_LIGHT_SLEEP=1`), S18 in the tree. One hardware agent, foreground
captures, kill stale bench processes first.
**Do:**
1. Boot, wait for `MQTT session usable`. `at AT+SQNIPSCFG?` then `at AT+SQNPSCFG?` and capture the
   `RX:` lines — these commands exist (`phaseAL-ps.log`) but their values have never been read.
   Retires §3(d) either way. ~2 min.
2. `sleeptest 6 0 20000 4000`. Relay pages at +90 s and +250 s; relay poll in a second log.
3. Capture the post-reset reprint of the report.
**Bars:**
1. Both pages received AND acked at the relay.
2. Page latency **20-30 s** after the relay stamp (bar, not a note: this is the ≤30 s typical call).
3. `asleep >= 84%` (§10.5 predicts 86% at a 3.0 s answer; below 84% means the answer is slower
   than the owner's 3 s and the cadence has to lengthen).
4. `awake-loop (no sleep)` under 10 s — §8.5's stall detector.
5. `probe answer: answer_ms` printed with `n` ≈ 18 and a plausible `max`. **This is the number the
   whole cadence table is parameterised on and it has never been measured.** Report it first.
6. `wait gaveup=0`; `probe_timedout=0`; `rsp_no_cmd=0`. Any of these non-zero means the 6 s budget
   is still short, i.e. the answer is slower than 3 s — report the value, do not re-run blind.
7. Zero `MQTT session LOST`; `modem_resets=0` (a window with an F4 is **void, not failed**, §9.5).
**Then:** feed `answer_ms` back into §10.3's table and pick the shipping
`PAGER_WAKE_INTERVAL_SLEEP_MS` in one sentence with its mAh/day. Sub-second answer → T=10 s;
~3 s → T=20 s; slower than 5 s → the wait is not affordable and §3(c)/WAKE0 moves ahead of it.
