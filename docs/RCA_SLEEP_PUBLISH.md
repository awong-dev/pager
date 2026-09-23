# RCA — release-only "publish payload replaced by AT command text"

Status: mechanism **narrowed, not proven**. One instrumented experiment (below) settles it.
Supersedes the cause paragraph in `docs/GOTCHAS.md:171-178` and in `.overnight-handoff.md`
("NEW RELEASE-ONLY BUG"), both of which name a mechanism this analysis refutes.

## 1. What is certain

**The bad payload is produced by the walter-modem library's own retry, not by msg_pump's.**
`_processModemCMD()`, `WalterModem.cpp:1763-1787`: when a `WALTER_MODEM_CMD_TYPE_DATA_TX_WAIT`
command times out (`CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS=30000`, `firmware/sdkconfig:1966`) and
`cmd->attempt < cmd->maxAttempts` (default 3, `WalterModem.h:142`), line 1778 re-transmits **the
same AT command line** with no knowledge of whether the modem is still owed `payloadSize` bytes
from the first attempt. If it is, the modem consumes the first `payloadSize` bytes of that line as
the payload and publishes them.

`AT+SQNSMQTTPUBLISH=0,"pager/test-pager/up",1` is exactly 44 characters. The relay's
`first64` stops at that `1` (`build/bench-logs/phaseV-relay-raw.json`, 15:13:24.534Z), so the
published payload was exactly 44 bytes — i.e. the stuck command declared 44 and the bytes that
satisfied it were its own re-transmitted command line. 44 bytes is a signed `publish_ack()`
envelope (`firmware/main/msg.c:1919-1963`): map(6) + v + id + ts + ack + n + 10-byte sig suffix
(`auth.h:56,60`). Timing corroborates: clean `/webhooks/mqtt` at 15:12:48.882, bad-sig at
15:13:24.534 = **35.65 s**, i.e. one 30 s command timeout plus relay ingest.

**This retry path is unconditional.** No light sleep is needed. Any lost `>` prompt or lost
payload write, from any cause, becomes a published AT command line and a relay `SECURITY bad-sig`.

**Which task issues which publish** (asked explicitly):

| publish | task | blocking? | protected from `net_sleep()` today? |
|---|---|---|---|
| `/status` on the incoming-page mode edge | modem **event task** (prio 4): `_eventProcessingTask` → `pager_mqtt_event_handler` MESSAGE (`xport_lte.cpp:277-324`) → `s_msg_cb` → `modes_alert_incoming()` (`modes.c:1195-1218`) → `set_mode()` (`modes.c:802-846`) → `publish_status_online()` (`modes.c:734-755`) | yes | yes, but only incidentally — `s_handler_busy` (`xport_lte.cpp:289,323`) → `net_modem_busy()` → `skip_sleep` (`modes.c:1836`) |
| `/up` ack and reply | **modes task** (prio 1): `modes_run()` → `msg_pump()` (`modes.c:2354`) → `publish_ack()` (`msg.c:2143`) | yes | n/a |
| `/status` heartbeat, sleep-edge; `/loc`; `sms_log`; `book` | modes task | yes | n/a |
| `/up` setup ack (`setup.c:827`) | event task | yes | **no** |

`mqttPublish()` is synchronous: no `cb` is ever passed, so `_returnAfterReply()`
(`WalterDefines.h:282-296`) blocks the calling task until the command completes.

**Therefore commit 985a343's stated mechanism is refuted for this failure.** The stuck publish is
an ack from `msg_pump()` on the modes task; that task is blocked inside `mqttPublish()` for the
whole 30 s, so it cannot reach `net_sleep()` (`net.cpp:1079-1177`). "Our own light sleep during
our own publish" is not the mechanism for an ack. (985a343 is also a no-op as written:
`publish_quiet_gate_issued()` runs *after* the blocking call returns — `xport_lte.cpp:409-415` and
`436-440` — so the flag never covers the command.)

## 2. Mechanism I believe: the `>` prompt is orphaned inside the parser

`_parseRxData()`, `WalterModem.cpp:1445-1478`. The prompt is recognised **only** at buffer offset
2, i.e. only when the buffer holds exactly `\r\n> `:

```c
1449  if(_parserData.buf && _parserData.buf->size > 2 &&
1450     _getCRLFPosition(buf->data + 2, buf->size - 2, true)) { ... _queueRxBuffer(); continue; }
1468  bool prompt1 = (buf->size >= 4 && buf->data[2] == '>' && buf->data[3] == ' ');
```

`_getCRLFPosition(..., findWhole=true)` returns true for a CRLF **anywhere** in the range, not
only at the end. So if the parser buffer is non-empty when the prompt's leading `\r\n` arrives,
that CRLF terminates the residue, the residue is queued, and the remaining `> ` starts a fresh
buffer of size 2 — which fails line 1449 (`size > 2`) and fails `prompt1` (`size >= 4`). The
prompt is never handed to `_processModemRSP()`, so the payload write at `WalterModem.cpp:2159-2181`
never runs. The modem sits at the prompt owed 44 bytes; 30 s later line 1778 feeds it the command
line. Traced byte-for-byte, the orphan occurs for input `\r\nOK\r\n> ` (one CRLF between the
previous line and the prompt) and for any CRLF-less residue left in the buffer.

Two ways a CRLF-less residue appears, both consistent with "release build only":
- **bytes lost at the light-sleep boundary.** `net.cpp:1147-1151` disables hardware flow control
  *before* forcing RTS high, then sleeps. In that gap the modem is free to send into a UART that
  is about to be powered down, and whatever is already in the RX FIFO is lost. A line truncated
  mid-way leaves exactly this residue. The debug build never sleeps, never truncates, never
  orphans — which is why this has never reproduced on debug.
- **read-chunk shape.** On wake the modem dumps everything it held while RTS was high, so lines
  that normally arrive one per `uart_read_bytes()` arrive concatenated. Concatenation with a
  double CRLF is handled correctly; concatenation with a single CRLF is not.

### The competing candidates, with what supports or refutes each

**(a) modes task light-sleeps mid-publish — REFUTED for the ack.** `msg_pump()` blocks the modes
task (§1). Still live for the *event-task* publishes (`/status` on the mode edge, `setup.c:827`):
those are covered today only by `s_handler_busy`, which is set for the MESSAGE and CONNECTED cases
and nothing else. In phaseV the `/status` was delivered cleanly at 15:12:48, so it was not the
casualty this time.

**(b) URCs interleaved with the prompt break `prompt1` — PARTLY TRUE, and it is (§2).** Traced:
`\r\n+SQNSMQTTONPUBLISH: ...\r\n\r\n> ` parses correctly; the failure needs residue or a single
separating CRLF. Nothing in the logs confirms or refutes it — the USB log dies at
`phaseV-release-boot.log:77`.

**(c) RTS/flow-control choreography drops bytes — PLAUSIBLE, and it is the most likely *trigger*
of (§2), not an independent failure.** Nothing in the logs shows it directly. Note also
`_queueRxBuffer()` drops a whole buffer, prompt included, if the 8-slot `_taskQueue` is full
(`WalterModem.cpp:1133-1141`, `WalterModem.h:127`), and `_getFreeBuffer()` drops bytes when the
8-buffer pool is exhausted (`WalterModem.cpp:1038-1056`, `WalterModem.h:147`). A wake-time URC
burst is exactly the condition that fills both. Each logs one line, which USB never carried.

**(d) `net_publish_quiet_wait_ms()` interacting with an event-task publish — REFUTED.**
`lte_publish_quiet_wait_ms()` (`xport_lte.cpp:445-462`) is a bounded `vTaskDelay()` poll on the
modes task. It never sleeps, never touches the UART, and cannot lose a prompt.

## 3. The experiment

Extend the existing debug-build `sleeptest` (`modes.c:943-1182`) — it already opens a real
light-sleep window and persists its report to NVS, which is the only thing that survives the dead
USB port. No RTC-memory ring and no `pager/.../log` topic are needed; the NVS report already
re-prints at the next boot (`modes_debug_sleeptest_print_saved()`, `modes.c:1105-1116`).

**Instrumentation to add (spec for firmware-dev; all of it counters, no behaviour change):**

1. In the library, four `extern "C"` counters exported through one accessor struct:
   - `datatx_retx` — incremented at `WalterModem.cpp:1778` when the re-transmit is for
     `WALTER_MODEM_CMD_TYPE_DATA_TX_WAIT`, together with `cmd->payloadSize` of the last one.
   - `prompt_orphan` — at the end of each `_parseRxData()` call, if `_parserData.buf` is non-NULL
     with `size == 2 && data[0]=='>' && data[1]==' '`. **This is the discriminator.**
   - `buf_drop_queue` — the `xQueueSend` failure at `WalterModem.cpp:1139`.
   - `buf_drop_pool` — `_getFreeBuffer()` returning NULL, `WalterModem.cpp:1052`.
2. In `xport_lte.cpp`, a 12-entry RAM ring of `{t_s, topic_tail[8], len, rc, ms}` written around
   each `mqttPublish()` call (before and after), where `rc` is the bool it returned and `ms` the
   wall time it blocked.
3. In `modes.c`, print 1 and 2 inside `modes_debug_sleeptest_report()` (`modes.c:1118-1181`), i.e.
   into the NVS-saved text. Budget ~400 chars; raise `s_st_text` from 1800 to 2400.

**Console sequence** (debug build, bench-tester, USB power; the port dies during the window and
returns after the harness's own `watchdog_hard_reset()`):

```
sleeptest 6 0 0
# from the host, 60 s in and again 180 s in:
relay/.venv/bin/python tools/bench/send_test_page.py test-pager "rca1"
relay/.venv/bin/python tools/bench/send_test_page.py test-pager "rca2"
# window closes at 360 s; +25 s grace; report saved; hard reset; report re-printed at boot
```

**What each outcome means:**

| observation | conclusion |
|---|---|
| `prompt_orphan > 0` and `datatx_retx > 0` with the same `payloadSize` as a `/up` publish in the ring | §2 confirmed. Fix is the parser change in §4. |
| `datatx_retx > 0`, `prompt_orphan == 0`, `buf_drop_queue`/`buf_drop_pool > 0` | the prompt buffer was dropped for want of a slot/buffer. Fix is queue/pool depth (8 → 16) plus §4's retry change; power cost nil, ~12 kB of static RAM for the pool. |
| `datatx_retx > 0`, all three drop/orphan counters 0 | the prompt never reached the ESP32 — candidate (c) on the wire. Next step is a logic-analyser capture on RX/RTS across one sleep boundary; no firmware change yet. |
| `datatx_retx == 0` and every publish in the ring returned true | not reproduced in 6 minutes. Re-run at `sleeptest 30`; two pages in six minutes reproduced it twice on release, so 30 min is the right second bet before spending bench time elsewhere. |

The experiment is safe on the debug build: `sleeptest` already exercises real light sleep and
already ends in a deliberate reset.

## 4. Fix direction, with power cost

Ordered by confidence. **Only the first is implemented** (see §5).

1. **Accept the bare `> ` prompt in the parser** (`WalterModem.cpp:1466-1480`). 3 lines, inert
   unless the orphan actually happens, and harmless if it does not: `_processModemRSP()`'s prompt
   handler already refuses to act unless a `DATA_TX_WAIT` command with a payload is current
   (`WalterModem.cpp:2161`). **Power cost: zero** — parser-side only, no extra AT traffic. Removes
   up to 90 s of forced-awake per occurrence (3 × 30 s of a blocked modes task at ~40 mA ≈
   1.0 mAh, against ~0.025 mAh had it slept).
2. **Never re-transmit a `DATA_TX_WAIT` command line after a timeout** (`WalterModem.cpp:1775-1783`):
   fail the command instead. This is the change that removes the *security* consequence rather than
   the cause. It is **not** implemented here because it is incomplete on its own: a modem left at
   the prompt owed 44 bytes will consume the next AT line the host sends, whichever command that
   is. Closing the prompt safely needs to know what the Sequans accepts as an abort —
   **UNKNOWN: there is no Sequans AT manual under `firmware/components/` or `docs/reference/`**
   (only `wf_partial_2in9.h`), and `0x1B` is documented for `AT+CMGS`, not for
   `AT+SQNSMQTTPUBLISH`. Cheapest way to find out: on the bench, `at AT+SQNSMQTTPUBLISH=0,"pager/test-pager/up",1,44`
   from the console, then `at` alone, and watch whether the modem answers `OK` (prompt timed out by
   itself, and how long it took) or swallows the `AT`. Five minutes, no code. Until that is known,
   the honest recovery rule is the one M3 already implements: treat a `DATA_TX_WAIT` timeout as a
   dead session, `net_session_down()` (`AT+SQNSMQTTDISCONNECT`) and reconnect. Power cost of a
   reconnect: one TLS handshake, ~5 kB and a few seconds of RRC, ~0.05 mAh — cheap against losing
   a page.
3. **Mark publish-in-flight before the command is queued, not after** (`xport_lte.cpp:409-415`,
   `436-440`). Fixes 985a343's no-op gate. Does not fix this bug (the modes task is blocked
   anyway); it does close the event-task publish window, which is real but currently unobserved.
   Power cost: ~1.5 s of held-awake per event-task publish ≈ 0.017 mAh, ~0.35 mAh/day at 20
   pages/day — 0.3% of the 95-107 mAh/day budget.
4. **Gate `net_sleep()` itself**, immediately before `esp_light_sleep_start()` (`net.cpp:1151`), on
   connect-in-flight OR publish-in-flight, returning without sleeping. `skip_sleep` is computed
   ~500 lines earlier (`modes.c:1836`) and the state can change in between. The library exposes no
   public "queue non-empty" accessor (`_curCmd`/`_cmdQueue` are private, `WalterModem.h:3282,3491`),
   so "any AT command outstanding" would need a new patch; not worth it until 3 is measured. Power
   cost: bounded by the existing 15 s cap, so worst case 15 s × 40 mA = 0.17 mAh per lost URC.
5. **Never publish from the event task**: have `set_mode()` set a `s_status_pub_pending` flag and
   let `modes_run()` publish it. Removes the whole class rather than guarding it, and removes the
   dependence on `s_handler_busy` being set by the right event cases. Power cost: zero AT traffic
   change; up to one wake cycle (5 s) of extra `/status` latency, which no requirement bounds.

## 5. Change made

`firmware/components/dptechnics__walter-modem/src/WalterModem.cpp:1466-1484` — PAGER PATCH 1.11,
and its entry in `PATCHES.md`. Nothing else touched. Not committed.
