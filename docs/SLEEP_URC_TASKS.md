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
