# Brief: pages lost while the pager light-sleeps

Self-contained. Load this file alone into a fresh session to analyse and design the fix. It
replaces the handoff and the long design docs for this one problem; those are listed at the end
as evidence, not required reading.

## 0. Resolved 24 Sep 2026

**Root cause.** On every wake from light sleep the modem UART delivers exactly one stray 0xFF byte,
in the same millisecond as the wake path's UART reconfiguration (`net_sleep()`'s uart_set_pin/
uart_set_hw_flow_ctrl), before any real traffic. It never appears when the ESP is awake (RTS toggled
by hand, 0 of ~40 tries), so it is a wake artefact. The vendored parser assumes every message starts
with "\r\n" and searches for the trailing CRLF from byte 2, so the 0xFF is glued to the front of the
next line: "\xff\r\nOK" (the probe's OK, never credited → the 15 s wait ran out every time) or
"\xff\r\n+SQNSMQTTONMESSAGE:..." (the page notification, not recognised → page lost for good).

**Fix.** Library patch 1.18 (`firmware/components/dptechnics__walter-modem/PATCHES.md`): in
_parseRxData, when not receiving a payload and the parser buffer is empty, a leading 0xFF byte is
dropped and counted (glitch_dropped, printed in the sleeptest report's `modem counters:` line).

**Evidence.** `build/bench-logs/phaseBB-report.log` (flight-recorder dump, baseline, debug build of
`2d4030c` + instrumentation): 13 wakes, 0xFF at every wake. Cycle c08: URC arrived 6 ms after wake
glued to the 0xFF, unrecognised, page m_23f9bd83 lost. Cycle c11: OK glued, probe uncredited, URC
3.4 s later fetched only at +15.0 s probe timeout.

**A/B result.** `build/bench-logs/phaseBC-report.log`, same shipped configuration (20 s cadence,
"\r\n" wake bytes, 15 s probe wait bound): 3 of 3 pages received (20 s, 23 s, 27 s after the relay
stamp), probe_issued=18 probe_answered=18, glitch_dropped=18, zero MQTT session LOST,
modem_resets=0, awake 7 s in the 360 s window. Baseline phaseBB: 2 of 3 pages, 5 of 13 probes
credited, awake 158 s. Wakes now last ~200 ms because the wait ends when the probe is credited; the
15 s is only a bound.

**Open.** (1) No `shown` ack was published in-window in either run; check whether the locked screen
(commit c291504) holds `shown` acks until unlock. (2) The "+CME ERROR: 4" after the "\r\n" wake
bytes is unexplained and harmless. (3) Flight recorder's PSRAM dump does not survive the
post-window hard reset; the debug build now holds 15 min after a window (printing a reminder every
60 s) so a USB replug can retrieve it with the `flightrec` console command.

## 1. The problem

A page (MQTT QoS 1 message on `pager/<id>/down`) sent while the pager is in its light-sleep cycle
is delivered roughly 2 times in 3. A page that is not delivered is never delivered: the modem
emitted its notification while the host could not receive it, and the message cannot be fetched
later because the fetch needs the message id that was in the notification.

Acceptance: one `sleeptest 6` window (360 s) with three pages sent mid-window, all three acked at
the relay, zero `MQTT session LOST`, zero modem resets. Light sleep is required for the beta.
Power is secondary but tracked: the current configuration is ~55-66% asleep, estimated ~190
mAh/day against a ~100 mAh/day target.

## 2. The system

- Host: ESP32-S3 (ESP-IDF 5.2.1), 2 MiB PSRAM enabled. Board: DPTechnics Walter.
- Modem: Sequans GM02SP, LTE-M, MQTT client and TLS inside the modem. Host talks AT over UART1 at
  115200 with hardware flow control: RX IO14, TX IO48, RTS IO21 (host output), CTS IO47 (host
  input), RESET IO45. IO46 is `LTE_WAKE0` on the schematic, a modem input, unused by firmware and
  by the vendored library. The modem's ring indicator RING0 is enabled in the modem
  (`+SQNRICFG: 1,3,1000`) but is not routed to any host pin or test point.
- Modem power settings at bring-up: eDRX `AT+SQNEDRX=2,4,"0010","0001"` (20.48 s cycle, PTW
  2.56 s), PSM off (`+CPSMS: 0`), host-interface power saving `AT+SQNIPSCFG=1,100` (mode 1,
  100 ms idle), `+SQNPSCFG: 5000`. `AT+IFC?` → `2,2`. No Sequans AT manual is in the repo; the
  semantics of `+SQNIPSCFG` modes and `+SQNPSCFG` are unverified.
- Vendored library: `firmware/components/dptechnics__walter-modem/src/WalterModem.cpp`, with local
  patches listed in `PATCHES.md` next to it. Line parser `_addATByteToBuffer()` (:1120) splits on
  `\r`, queues buffers to a single FIFO shared with commands; `_processModemRSP()` (:2079) pairs a
  buffer with whatever `_curCmd` is current; a buffer with no current command is counted
  `rsp_no_cmd`. Page notification URC: `+SQNSMQTTONMESSAGE:0,"<topic>",<len>,<qos>,<mid>` parsed
  at :3928; the host then issues `AT+SQNSMQTTRCVMESSAGE=0,"<topic>",<mid>` from
  `firmware/main/xport_lte.cpp:348` (event task) and the payload flows to `msg.c`, which renders
  and publishes a `shown` ack on `/up`.
- `AT+SQNSMQTTRCVMESSAGE=0,"<topic>"` with no mid answers `+CME ERROR: 4` on an empty queue.
  Whether it returns a pending message is unverified: a window with 14 such polls scored 0 hits
  while two pages went missing, which is consistent with either "unsupported" or "the messages
  were not in the queue".

## 3. The sleep cycle today (release build, commit `2d4030c`)

`firmware/main/modes.c` `modes_run()` loop, constants at :127-173:

1. `net_sleep(20000)` (`firmware/main/net.cpp:1116`): arm timer + ext0 button (IO1) + ext1
   accelerometer (IO2); disable UART flow control; drive RTS high (= not ready) as a plain GPIO;
   `gpio_sleep_sel_dis(RTS)` so the pad really holds through sleep (before this fix the pad
   floated and pages were 35-182 s late); `esp_light_sleep_start()`. Display RST/DC/CS/VCC_EN
   pads are held the same way (`disp.c:707`).
2. On wake: `uart_set_pin()` + `uart_set_hw_flow_ctrl(CTS_RTS)` (RTS asserted again).
3. `net_urc_probe()` (`net.cpp:1343`): write `"\r\n"`, wait 60 ms, queue one bare `AT` with a
   15 s library timeout (1 attempt), fire-and-forget with a callback that stamps `answer_ms`.
4. `wait_for_probe_answer()` (`modes.c:1782`): stay awake, RTS asserted, polling every 20 ms until
   the probe is answered or `PAGER_PROBE_WAIT_MS` = 15 000 ms elapse.
5. Input, UI, `net_service_session()` (liveness re-SUBSCRIBE every 300 s idle), `msg_pump()`
   (acks). Then back to 1. The loop skips sleeping while a modem command is in flight or a publish
   is pending.

Both SLEEP and ACTIVE modes use the 20 s interval. The debug build (`PAGER_DEBUG_NO_LIGHT_SLEEP=1`)
never sleeps except inside a `sleeptest` window, and is the only build with a console.

## 4. Established facts, with the run that established each

Runs are named `phaseXX`; logs in `build/bench-logs/phaseXX-*`. `*-report.log` is the
`sleeptest` report printed from NVS after the window; `*-summary.txt` is the digest.

1. **The modem holds a page notification while the host's RTS is deasserted and releases it only
   after the host has been awake with RTS asserted for some time.** With a 200 ms wake window
   pages arrived 191-237 s late or never; with the host awake for 30 s (a stalled blocking command)
   the page came out inside that window (phaseAB, phaseAD, RCA_SLEEP_URC §2 timeline).
2. **The release of the notification does not depend on the probe being answered.** phaseAV
   (debug, 20 s cadence, 15 s wait): 2 of 10 probes answered, yet both pages delivered 12 s and
   16 s after the relay stamp. Same shape in phaseBA: 5 of 12 answered, one page delivered.
3. **Superseded by §0.** The 0xFF glue explained every instance; probes were not being credited when
   the stray byte preceded the OK response.
4. **Superseded by §0.** The 0xFF-prefixed OK/URC lines were the unpaired responses that appeared as
   `rsp_no_cmd` counters.
5. **Superseded by §0.** The 0xFF-glued notification was unrecognised and unrecoverable; the fix
   prevents both occurrences.
6. **Delivery latency when it works: 12-54 s after the relay stamp.** Budget: eDRX up to 20.48 s
   (a missed paging occasion adds another cycle) + up to one 20 s wake interval + the wait.
7. **The session and the radio are fine.** Broker `connected_at` unchanged across windows, zero
   `MQTT session LOST` in phaseAV/AW/AX, `modem_resets=0`, `datatx_retx=0`, `prompt_orphan=0`,
   buffer drop counters 0. The ESP-side "bytes in the first 50 ms after wake" counter is always
   0 (nothing is waiting in the UART when the host wakes).
8. **FreeRTOS ticks do not advance across light sleep**, so every library timeout is awake time.
   The 15 s wait is 15 s of awake time by construction.
9. **USB-Serial-JTAG console dies after the first light sleep** and needs an unplug/replug; the
   `sleeptest` report survives in NVS and prints on the next `sleeptest` command. There is no
   in-window serial trace of the release build. The AT trace (`ESP_LOGD` `TX:`/`RX:`) is compiled
   in since `9332a05` and prints in the debug build while awake.
10. Modem in-flight ack payload corruption (a publish's payload replaced by the next command line)
    is fixed by library patches 1.11/1.12 and no longer occurs (counters in fact 7).

## 5. Tried and rejected (do not retry as-is)

| lever | run | result |
|---|---|---|
| 200 ms wake, no probe, 5 s cadence | phaseAB/AC | pages 191-237 s late or never |
| bare `AT` probe, 200 ms window | phaseAK | 95% asleep, no page ever |
| probe + 4 s wait, 20 s cadence | phaseAO/AS | 1 of 2 pages |
| probe + 8 s wait | phaseAT | 1 of 2 |
| `\r\n` wake bytes + 60 ms before the probe, 4 s wait | phaseAU | 1 of 2 |
| **probe + 15 s wait, 20 s cadence (shipped)** | phaseAV, AW, AX | **2 of 2, then 2 of 3, 2 of 3** |
| `AT+SQNIPSCFG=0` (interface power saving off), 5 s cadence | phaseAP | worse: session LOST rc=-13, `answer_ms max 24225`. One confounded window; not conclusive about mode 0 itself |
| `AT+SQNIPSCFG=2,100` (mode 2, semantics unknown) | phaseAZ | 1 of 3 |
| per-wake `AT+SQNSMQTTRCVMESSAGE=0,"<topic>"` poll (no mid) | phaseAY | 0 hits of 14 while 2 pages lost |
| liveness re-SUBSCRIBE every 45 s | phaseBA | 1 of 3 and a session LOST |

## 6. What has not been measured, and would decide the design

Each item names the measurement and what each outcome implies.

- **A. What arrives on the UART during the wait, byte for byte.** Log the first four unpaired
  lines per window hex-escaped (a counter plus a small ring in the library where `rsp_no_cmd` is
  incremented), and log every RX line during a debug `sleeptest` window (the trace exists; USB may
  survive in the debug build long enough, and the NVS report can carry the ring). If the unpaired
  lines are the probe's `OK` arriving after 15 s, the modem is slow to wake. If they are truncated
  `+SQNSMQTTONMESSAGE` lines, the notification is being clipped at the host. If they are absent,
  the probe bytes were dropped by the modem.
- **B. Host CTS input (IO47) level over time after RTS re-assert.** Sample it every 10 ms for the
  first second and at the moment the probe is issued. If the modem drops CTS while its interface
  sleeps and raises it when ready, the correct wake sequence is "assert RTS, wait for CTS, then
  send", and the 60 ms wait is the bug. The ESP UART honours CTS in hardware only when flow control
  is on; it is on at that point, but the modem may leave CTS asserted while asleep.
- **C. Whether the notification is emitted while the host sleeps.** The "bytes in the first 50 ms
  after wake" counter is 0, so nothing is buffered in the ESP UART. If the modem transmits into a
  host whose RTS is deasserted it would have to ignore flow control; the lost-for-good pattern
  (fact 5) is only explained by the notification being emitted somewhere the host did not parse
  it: during sleep, during the `uart_set_pin`/`uart_set_hw_flow_ctrl` reconfiguration on wake, or
  in the seconds while the host is awake but the line is misparsed. Discriminate with A plus a
  per-wake count of bytes received in the wait window when no page was pending.
- **D. `+SQNIPSCFG` mode semantics.** Without the manual, an experiment on an awake debug build:
  set mode 0, leave the console idle 5 s, send `AT`, read latency; set mode 1, same; mode 2, same.
  Then the same with RTS toggled (flow control off, RTS high for 5 s, back on, send). This gives
  the interface wake latency per mode directly, with the trace on, no sleep involved.
- **E. Whether `esp_sleep_enable_uart_wakeup()` on UART1 works with RTS held asserted through
  sleep, and how many leading bytes are lost.** Needs `gpio_sleep_sel_dis(RX)` (the same GPIO
  isolation that floated RTS applies to RX). The URC's topic, length and mid are at the END of the
  line, so a resync rule that recognises `,"<topic>",<len>,<qos>,<mid>\r\n` with a clipped head
  recovers the page without a content-recovery command. Measure: wake cause counts, bytes lost
  between first RX edge and first parsed byte, clipped-line count.
- **F. WAKE0 (IO46).** A modem input. Unknown whether any AT command enables it or what it does
  in each `+SQNIPSCFG` mode. Cheap to try on the awake debug build after D: pulse IO46 before the
  probe and compare latency.

## 7. Candidate designs, in the order the evidence favours them

1. **Fix the wake handshake (if B or D shows a readiness signal or a latency).** Assert RTS, wait
   for the modem to be ready (CTS edge, or a measured fixed delay, or a WAKE0 pulse), then send
   the probe. Expected result: probe answered every wake in ms, page released on the first wake
   after it arrives, wait time drops from 15 s to the measured latency. This keeps today's
   architecture and is the smallest change.
2. **Modem wakes the host (phase 2).** RTS asserted through sleep, `esp_sleep_enable_uart_wakeup()`
   on UART1, RX pad kept alive, parser resync for the clipped head of the waking line, timer wake
   lengthened (the timer is then only the liveness cadence). Removes the wait entirely: ~97
   mAh/day at 20 s cadence by the existing power model. Risk: clipping (E) and the unverified
   behaviour of `+SQNIPSCFG` mode 1 when the host never deasserts RTS (the modem's interface may
   then never power-save, or may still hold URCs until spoken to).
3. **Hardware ring indicator.** Route RING0 to a wake-capable GPIO. Cleanest, but the pad is not
   exposed on the module footprint or any test point. Owner hardware call.

Hard rules from the owner: no ULP-coprocessor byte-capture receiver without explicit
authorization; a wire the owner has ruled out stays ruled out; no defensive changes that were not
verified on the bench; A/B behind a runtime flag on one flash where possible.

## 8. Bench recipe

- Build: `export PATH=$HOME/.espressif/python_env/idf5.2_py3.11_env/bin:$PATH;
  . ~/src/esp/esp-idf/export.sh; cd firmware; idf.py build` (release, sleeps) or
  `PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build` (debug, console, sleeps only inside
  `sleeptest`). Switching builds needs `reconfigure`. `firmware/sdkconfig` is untracked.
- Flash the app: `~/.espressif/python_env/idf5.2_py3.11_env/bin/esptool.py --chip esp32s3
  write_flash 0x120000 firmware/build/school_pager.bin`. A sleeping pager cannot be flashed: hold
  BOOT, tap RESET, or retry until a wake catches. Never run esptool while a serial capture holds
  the port.
- Console (debug build): `sleeptest <min> [yield_ms] [interval_ms] [probe_wait_ms]`, 0 = build
  default; `at <cmd>` sends raw AT with the trace; `mqtttest`; `disptest`.
- Capture: `~/.espressif/python_env/idf5.2_py3.11_env/bin/python tools/bench/serial_capture.py
  <outfile> <total_s> [<at_s>:<cmd> ...]` (no port arg).
- Send a page: `relay/.venv/bin/python tools/bench/send_test_page.py` (see its usage); relay
  ack lines come from `gcloud logging read ... --project kid-pager`. Broker client state:
  `relay/.venv/bin/python tools/bench/poll_emqx_client.py test-pager` (`connected_at`,
  `recv_pkt`, `subscriptions_cnt`).
- After a window: unplug/replug USB, connect, type `sleeptest` to print the saved report.
- One hardware agent at a time; kill stale captures and pollers first (`pgrep -f serial_capture`,
  `pgrep -f poll_emqx`).

## 9. Files

`firmware/main/net.cpp` (net_sleep :1116, probe :1343, IPSCFG :645), `firmware/main/modes.c`
(loop and constants :127-173, wait :1782, probe call :2210, report), `firmware/main/xport_lte.cpp`
(URC → fetch :348, liveness :159-330), `firmware/main/net_probe_guard.c`,
`firmware/components/dptechnics__walter-modem/src/WalterModem.cpp` and `PATCHES.md`,
`firmware/main/pins.h`. Host tests: `make -C firmware/host test`.

Evidence documents, only if a specific number is needed: `docs/SLEEP_URC_DESIGN.md` (§8-§10),
`docs/RCA_SLEEP_URC.md`, `docs/SLEEP_URC_TASKS.md` (S9 = phase 2 spec),
`build/bench-logs/phaseA[S-Z]-*`, `phaseBA-*`.
