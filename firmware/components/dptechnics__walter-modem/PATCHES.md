# Local patches to dptechnics/walter-modem v1.5.0

This component was vendored from the component manager
(`dptechnics/walter-modem` v1.5.0, `managed_components/dptechnics__walter-modem/`,
upstream commit `51b16ce0627647acc8106c2c0d93b074fd98ea88`) into this tracked local
copy so it could be patched — see `docs/V02_DESIGN.md` §1. `examples/`,
`.component_hash` and `.gitignore`/`.clang-format` were dropped; everything else
(`src/`, `CMakeLists.txt`, `Kconfig`, `LICENSE`, `README.md`, `CHANGELOG.md`,
`idf_component.yml`) was kept, with every file header intact per the DPTechnics
5-clause licence's redistribution terms.

Every change below is marked `// PAGER PATCH:` (or, in `Kconfig`, `# PAGER PATCH`)
at the exact spot in the source, so a future upstream sync can find and re-apply
them by grepping for that string.

## Patch 1.1 — NULL command crash (`src/WalterModem.cpp`, `_processModemRSP()`)

A URC or error line (observed on real hardware as a stray `NO CARRIER`) can
reach the end of `_processModemRSP()`'s dispatch chain (`after_processing_logic:`)
with no command pending (`cmd == NULL`). The original code's finishing
condition was:

```c
if((cmd != NULL && cmd->atRsp != NULL && memcmp(...) == 0) || result != WALTER_MODEM_STATE_OK) {
    _finishModemCMD(cmd, result);
    ...
}
```

When `cmd == NULL` and `result != WALTER_MODEM_STATE_OK` (an error/URC with no
command pending), this called `_finishModemCMD(NULL, result)`, which
unconditionally dereferences `cmd->rsp` — a crash/reboot with no ATE0 command in
flight. `GOTCHAS.md`: "a stray `NO CARRIER` with no command pending
dereferences a null command and reboots."

**Fix**: an explicit `cmd == NULL && result != WALTER_MODEM_STATE_OK` branch
ahead of the existing condition frees the buffer and returns — nothing to
finish when no command is pending.

## Patch 1.2 — Receive bounds (`src/WalterModem.cpp`)

Three separate unbounded-copy sites, all writing into or out of a
`WalterModemBuffer` (`data[WALTER_MODEM_RSP_BUF_SIZE]`, 1540 bytes, `size`
tracking how much is actually valid):

1. **`_addATByteToBuffer()` / `_addATBytesToBuffer()`** (the raw UART-ingest
   path, called from `_parseRxData()`): wrote directly into `buf->data[buf->size++]`
   / `memcpy(&buf->data[buf->size], data, length)` with no bound against
   `WALTER_MODEM_RSP_BUF_SIZE` at all. A single AT response/URC/payload chunk
   longer than the buffer's capacity overflowed the fixed-size `data[]` array —
   memory corruption, not just a wrong answer. Fixed to cap both at
   `WALTER_MODEM_RSP_BUF_SIZE - 1` (reserving the last byte for the NUL
   `_buffStr()` writes at `buf->data[buf->size]`), silently dropping excess
   bytes rather than writing past the array. This does not reassemble a
   payload across multiple buffers (out of scope, per design); a
   too-large response is simply truncated, so the buffer stays memory-safe and
   the eventual command completion sees a short/incomplete buffer and fails
   cleanly instead of corrupting adjacent memory.
2. **Socket receive** (`+SQNSRECV: ` response handler): `memcpy(cmd->payload,
   payload, dataReceived)` where `dataReceived` is a value the modem itself
   reported in the response header, unbounded by `cmd->payloadSize` (the
   caller's real buffer size) or by how many bytes are actually present in
   this `WalterModemBuffer` after the header. Fixed to bound the copy by the
   minimum of all three, and to report back the amount actually copied via
   `cmd->rsp->data.socketResponse.bytesReceived` (previously always the raw,
   unverified `dataReceived`), so `socketReceive()` cannot claim more was
   delivered than actually was.
3. **MQTT receive** (`AT+SQNSMQTTRCVMESSAGE=0,` response handler):
   `memcpy(cmd->payload, rspStr, cmd->payloadSize)` unconditionally — if the
   modem delivered fewer bytes than the caller asked to read
   (`buff->size < cmd->payloadSize`), this read past the end of `buff->data`.
   `GOTCHAS.md`: "receive buffers are 1540 bytes and `mqttReceive()`
   copies out of them unchecked (the setup bundle with the DigiCert root is
   1468)" — close enough to the 1540-byte cap that a slightly larger bundle
   would have silently read out of bounds. Fixed to bound the copy by the
   bytes actually present in `buff` from `rspStr` onward.

## Patch 1.3 — TLS profile range (`Kconfig`)

`WALTER_MODEM_MAX_TLS_PROFILES` was capped `range 1 3` / `default 3`.
`tlsConfigProfile()` checks `profile_id >= WALTER_MODEM_MAX_TLS_PROFILES`, so
this cap made `profile_id == 3` fail immediately with `NO_SUCH_PROFILE`, even
though the Sequans modem's own `spId` range is 1-6. `docs/V02_DESIGN.md` §1:
profile 1 stays BlueCherry's, 2 is this project's MQTT profile, 3 is the CA
fetch socket (§4.4, a later task). Raised to `range 1 6` / `default 6` so a
later task does not need to touch this vendored component again just to use
profile 3.

## Patch 1.5 — CEREG lac/ci dropped for non-PSM report types (`src/WalterModem.cpp`, `_processModemRSP()`)

Found wiring `docs/V02_DESIGN.md` §5's cell/tracking-area-change location
trigger, which needs the `+CEREG` URC's `lac`/`ci` fields
(`WMNetworkEventData.cereg.lac`/`.ci`). The URC handler's `sscanf()` already
parses `lac`/`ci`/`act` correctly for both extended-URC variants (with or
without the trailing PSM timer fields), storing them in local `lac[16]`/
`ci[16]`/`act` — but the code that copies those locals into the dispatched
`WalterModemEvent` gated ALL FIVE fields (`lac`, `ci`, `act`, `activeTime`,
`periodicTau`) behind one `hasPsmInfo` flag, which is only true for the two
report types that ALSO carry the PSM active-timer/periodic-TAU fields
(`AT+CEREG=4` or `=5`). Requesting the plain "with location" report type
(`AT+CEREG=2`, `WALTER_MODEM_CEREG_REPORTS_ENABLED_WITH_LOCATION`, this
project's own choice — no PSM timers needed) meant every dispatched event
carried `lac[0]='\0'`/`ci[0]='\0'`, even though the modem sent them on the
wire and this function's own `sscanf()` had already parsed them into `lac`/
`ci`.

**Fix**: copy `lac`/`ci`/`act` unconditionally (they are always correctly
populated-or-empty by the `sscanf()` above, independent of `hasPsmInfo`);
keep `hasPsmInfo` gating only `activeTime`/`periodicTau`, which really are
absent unless that specific report type was requested.

## Patch 1.4 — SMS (`src/WalterModem.h`, `src/WalterModem.cpp`)

`docs/V02_DESIGN.md` §6 (device-direct SMS). Adds text-mode (3GPP TS 27.005)
SMS send/read/delete/new-message-event support, following this library's own
established patterns rather than introducing new ones:

- **New event type** `WALTER_MODEM_EVENT_TYPE_SMS` (mirrors GNSS/network/
  voltage: enum entry, `WMSmsEventType`/`WMSmsEventData`, a
  `walterModemSmsEventHandler` typedef, a `smsHandler` union member, a
  `WalterModemEvent.sms` union member, a `setSmsEventHandler()` method, and a
  `_dispatchEvent()` case) — unconditional, no `CONFIG_WALTER_MODEM_ENABLE_SMS`
  guard, same as temperature/voltage. Dispatches on `+CMTI: "<mem>",<index>`
  (3GPP TS 27.005 §3.4.1), parsed the same way the existing `+SQNSVMONS` URC
  handler builds and queues a `WalterModemEvent`.
- **`smsConfig()`**: one-time text-mode setup, fail-fast on the first
  sub-command that errors — `AT+CMGF=1` (text mode), `AT+CSCS="GSM"` (the
  *resting* TE charset; `smsSend()` toggles to `"UCS2"` only for the duration
  of a UCS-2 send and always restores `"GSM"` afterward, so a message read
  while idle is always in the charset `main/sms.c`'s decoder assumes),
  `AT+CSMP=17,167,0,0` (DCS 0 = GSM 7-bit default alphabet; `smsSend()`
  overrides the last parameter to 8 for a UCS-2 send and restores 0
  afterward), `AT+CNMI=2,1,0,0,0` (buffered new-message URCs, index only),
  `AT+CPMS="ME","ME","ME"` (device memory, not the SIM). Each sub-command
  runs as its own `_runCmd`/`WalterModemCmd` pair (a private free function,
  `_waitCmdResult()`, blocks on the command's own condition variable and
  returns a plain `bool` without the usual `_returnAfterReply()` macro's
  unconditional `return`, so `smsConfig()`/`smsSend()` can bail out — or run
  cleanup — between steps).
- **`smsSend(number, text, useUcs2)`**: follows the
  `WALTER_MODEM_CMD_TYPE_DATA_TX_WAIT` pattern `tlsWriteCredential()` already
  uses (wait for the modem's `"> "` data prompt, then write the payload
  raw), with the one SMS-specific addition that the payload is `text`
  followed by Ctrl-Z (`0x1A`), which terminates a text-mode `AT+CMGS` body
  (3GPP TS 27.005 §4.3). `useUcs2` selects the caller-pre-encoded form
  (`main/sms.c` decides GSM-7 vs. UCS-2 and does the actual character
  encoding — this layer never touches encoding beyond the `AT+CSCS`/`AT+CSMP`
  toggle described above).
- **`smsRead(index)`**: `AT+CMGR=<index>` (text mode). The response is two
  physical lines (`+CMGR: <stat>,<oa>,[<alpha>],<scts>` then the message body)
  which this library's line-at-a-time response processor does not otherwise
  reassemble — `_processModemRSP()` gained a small two-step scratch state
  (`_smsCmgrAwaitingBody`, checked *first*, ahead of every other prefix
  match, precisely so an SMS body that happens to start with `"OK"` or
  `"ERROR"` is never misread as the command's own terminator) plus a small
  quoted-CSV field splitter (`_smsSplitCmgrFields()`) for the header line.
  Result lands in `rsp->data.smsRead` (`WALTER_MODEM_RSP_DATA_TYPE_SMS`).
- **`smsDelete(index)`**: `AT+CMGD=<index>` (text mode), single-line `"OK"`
  response, no new parsing needed.
- **`+CMS ERROR: `** (3GPP TS 27.005 §3.2.5, the SMS-specific error report)
  is now handled the same way `+CME ERROR: ` already was: fails the in-flight
  command with `WALTER_MODEM_STATE_ERROR`.

Deliberate simplification vs. the rest of this library's public API:
`smsConfig()`/`smsSend()`/`smsRead()`/`smsDelete()` all keep the usual
`rsp`/`cb`/`args` parameters for consistency, but `net.cpp` (this project's
only caller) never passes a `cb` — every call is synchronous. The async
branch in `_waitCmdResult()` is therefore unreachable in practice; kept only
so the class's calling convention stays uniform.

Every AT command sequence and response shape above is **UNVERIFIED** on the
Sequans GM02SP: 3GPP TS 27.005 is the source, not a datasheet for this exact
modem, and no production SIM had SMS tested against it when this was
written. In particular: whether SMS works on the production SIM at all
(a data-only SIM may reject every one of these commands), the exact
`+CMTI`/`+CMGR` wording and timing under eDRX, and whether the
`AT+CSCS`/`AT+CSMP` toggle is even necessary for this modem to accept UCS-2
text. `smstest`/`smslist` (the debug console commands, `main/main.c`) exist
to settle these on real hardware.

## 1.6 Explicit socket ids (`src/proto/WalterSocket.cpp`, `_socketGet()`)

`WalterModemSocket::id` defaults to `1` for every entry of `_socketSet` and only
`_socketReserve()` assigns a real id, so a lookup for any explicit id other than 1 returned NULL
and `socketConfig(4)` failed locally with `NO_FREE_SOCKET`. `_socketGet(id)` now maps id N onto
slot N-1 and claims it. Found on hardware 2026-09-20 (the CA fetch uses socket 4).

## 1.7 Stray line break before a final `OK` (`src/WalterModem.cpp`, response pre-processing)

With `AT+SQNSRECV` on a TLS socket, a payload ending in a bare `\n` (a PEM file) left the final
result queued as `\n\r\nOK\r\n`. It never matched the expected `OK`, so the command timed out
after 30 s although the data had arrived in 100 ms. A buffer that is only CR/LF characters
followed by exactly `OK` is now normalised to `OK`. Found on hardware 2026-09-20. The one-byte
miscount in the payload parser that causes it is not fixed; the last payload byte (the `\n`) is
lost from such a read, which the CA fetch tolerates only because it hashes what the relay serves
... see `cafetch.c`: if a hash mismatch is ever seen on a body ending in `\n`, look here first.

## 1.8 Restricted SIM access (`+CRSM`) (`src/WalterModem.cpp`, `src/WalterModem.h`)

The library has no `AT+CRSM` support and `sendCmd()` returns no response text. The response
processor now keeps the last `+CRSM: <sw1>,<sw2>[,<hex>]` line, readable through
`WalterModem::simLastCRSM()`. The pager uses it to read `EF_GID1` and recognise an MVNO SIM so it
can choose the right APN (`firmware/main/carrier.c`).

## 1.9 Serving-cell MNC digit width (`src/WalterModem.cpp`, `src/WalterModem.h`)

`docs/PROTOCOL.md` §13.2's `cell.mnc` is a **string**, 2 or 3 digits, specifically so a leading
zero survives (`"05"` vs `"5"`). `getCellInformation()`'s `+SQNMONI` parser already converts the
`Nc:` field straight to a `uint8_t` (`WalterModemCellInformation.nc`) via `strToUint8()`, which
throws the original digit width away — `nc=5` is indistinguishable from a raw `"05"` and a raw
`"5"`. Added `WalterModemCellInformation.ncDigits`, set from the same `value_len` the existing
`Nc:` parse already computes (0 if the field was empty/longer than 3 digits, i.e. "unknown" —
never guessed). `firmware/main/net.cpp`'s `net_get_cell_info()` uses it to zero-pad correctly, and
falls back to an MCC-based NANP heuristic only when `ncDigits` is 0 (e.g. against an unpatched
component) — see that function's own doc comment.

**Widened 2026-09-21 (bench finding, `build/bench-logs/08-locreq2.log`):** on the bench SIM
(AT&T 310/410) the raw response was `+SQNMONI: US Mobile Cc:310 Nc:410 ...` but the pager's `/loc`
went out with `cell.mnc:"000"`. Root cause was **not** a missing `Nc:` field — the modem sends one
even while idle/attached, as the raw line shows — it was `nc`'s width: `uint8_t` cannot hold "410"
(3-digit MNCs run 0-999, > 8 bits), so `strToUint8()` returned `false` on the range check and left
`nc` at its zero-initialised value, while the surrounding code set `ncDigits = 3` **unconditionally
from the field's text width**, regardless of whether the numeric parse actually succeeded. The
result was indistinguishable on the wire from a genuine `"000"` MNC. Fixed by widening
`WalterModemCellInformation.nc` to `uint16_t` and switching the parser to `strToUint16()` (0-999
fits easily), and by gating `ncDigits` on that call's own success — an `Nc:` field that fails to
parse numerically for any other reason now reports `ncDigits = 0` ("unknown digit width") instead
of a confident-looking width next to a wrong value. `firmware/main/net.cpp`'s
`net_get_cell_info()` needed no change: it already treats `ncDigits` 0 as "fall back to the NANP
heuristic", which now only fires on a genuine parse failure, not on every 3-digit NANP MNC.

## 1.10 Bounded, watchdog-feeding synchronous command wait (`src/WalterDefines.h`, `src/WalterModem.cpp`)

Found on the bench: the command queue is single-threaded, and a slow command holds it for the
retry logic's full `3 * CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS` (30 s in this project's sdkconfig, so
90 s) before giving up — 90 s is already past the 60 s task watchdog timeout (`CONFIG_ESP_TASK_WDT_TIMEOUT_S`,
the IDF maximum), and the three synchronous wait sites below waited on their condition variable
with no timeout at all, so nothing fed either watchdog while a command was stuck. On real hardware
this was `AT+COPS=0` (`firmware/main/net.cpp:826`, `net_bringup()`'s `setNetworkSelectionMode`,
issued after 30 min dark) during a network search: it held the queue long enough that the next
queued command — `net_check()`'s bare `AT` health-check probe, `firmware/main/modes.c:1435` —
never got a turn before the task watchdog fired:

```
W (11113735) WalterModem: TX: AT+COPS=0
W (11143735) WalterModem: Command time-out (TX) Attempt 1 of 3
W (11173735) WalterModem: TX: AT+COPS=0
E (11175345) task_wdt: Task watchdog got triggered. ... - main (CPU 0)
...
W (296) watchdog: RESET REASON: TASK WATCHDOG. The main loop's last stage was: modem health check.
```

(`build/bench-logs/r2-coverage-recover2-serial.log` lines 111-127, 203.)

**Fix**: all three untimed `cond.wait(lock, pred)` sites in this component now loop on
`cond.wait_for(lock, std::chrono::milliseconds(1000), pred)`, calling a new
`walter_modem_block_tick()` hook every second the predicate is still false, each site keeping its
own original predicate unchanged:

- `src/WalterDefines.h`'s `_returnAfterReply()` macro (used by every ordinary blocking command):
  `[cmd] { return cmd->state == WALTER_MODEM_CMD_STATE_SYNC_LOCK_NOTIFIED; }`
- `src/WalterModem.cpp`'s `_waitCmdResult()` (the `smsConfig()`/`smsSend()` step helper):
  `[cmd] { return cmd->state == WALTER_MODEM_CMD_STATE_SYNC_LOCK_NOTIFIED; }`
- `src/WalterModem.cpp`'s `getNetworkRegState()`: the same predicate, kept exactly as the original
  had it (including its harmless stray trailing `;` statement inside the lambda body).

`walter_modem_block_tick()` is declared `extern "C"` in `WalterDefines.h` and given a weak no-op
default in `WalterModem.cpp` so the component still links standalone; `firmware/main/watchdog.c`
provides the real, strongly-overriding definition, feeding the RTC watchdog and (only from the
subscribed main-loop task) the task watchdog for up to `WD_MODEM_BLOCK_BUDGET_MS` = 95 s per stage
(> the 90 s worst case above, < the 180 s RTC watchdog) — past that budget it does nothing, so a
command that is genuinely wedged (not just slow) still trips the task watchdog with the
"modem health check" breadcrumb intact rather than looping forever. `watchdog_kick()` resets that
per-stage budget so each main-loop stage starts fresh. See `firmware/main/watchdog.h`/`.c` for the
full arithmetic and `firmware/main/watchdog.h`'s doc comment on `walter_modem_block_tick()`.

## 1.11 Orphaned `> ` data prompt (`src/WalterModem.cpp`, `_parseRxData()`)

`_parseRxData()` recognises the modem's data prompt only as `prompt1`:
`buf->size >= 4 && buf->data[2] == '>' && buf->data[3] == ' '` — i.e. only when the prompt's own
leading CRLF is still at `data[0..1]`. The check immediately above it queues the buffer as soon as
`_getCRLFPosition(buf->data + 2, buf->size - 2, /* findWhole */ true)` finds a CRLF **anywhere**
past offset 2, not only at the end. So when the parser buffer is non-empty at the moment the
prompt's `\r\n` arrives, that CRLF terminates the residue and is consumed with it; the remaining
`> ` starts a new buffer of size 2, which is too short for the CRLF check (`size > 2`) and too
short for `prompt1` (`size >= 4`). The prompt is never queued, `_processModemRSP()`'s `"> "`
handler never runs, and the `WALTER_MODEM_CMD_TYPE_DATA_TX_WAIT` payload is never written.

The consequence is not a lost publish but a corrupt one. The modem stays at the prompt owed
`payloadSize` bytes; `_processModemCMD()` times out after `CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS`
and **re-transmits the same AT command line**, which the modem consumes as the payload. Observed
on the bench as the relay logging
`SECURITY bad-sig ... first64=b'AT+SQNSMQTTPUBLISH=0,"pager/test-pager/up",1'` — 44 bytes of the
pager's own command line published as a message (`build/bench-logs/phaseV-relay-raw.json`,
2026-09-23 15:13:24Z). Release builds only, because the residue that consumes the prompt's CRLF
comes from bytes truncated at a light-sleep boundary; the debug build never light-sleeps.

**Fix**: a `prompt3` case for the bare, already-stripped prompt (`size == 2 && data[0] == '>' &&
data[1] == ' '`). Safe by construction — `_processModemRSP()`'s prompt handler acts only when the
current command is a `DATA_TX_WAIT` with a payload, so a spurious match with nothing pending frees
the buffer without effect. Parser-side only, no power effect.

**Not fixed here** (see `docs/RCA_SLEEP_PUBLISH.md` §4.2): `_processModemCMD()` re-transmitting a
`DATA_TX_WAIT` command line at all while the modem may still be in data mode. Failing the command
instead is correct but incomplete on its own — the modem would then eat the *next* AT line — and
what the Sequans accepts as a prompt abort is not documented in this repo.

## 1.12 DATA_TX_WAIT timeout sends the payload instead of the AT command line, plus RCA §3 instrumentation counters (`src/WalterModem.cpp`, `src/WalterModem.h`, `src/WalterDefines.h`)

Bench evidence settling `docs/RCA_SLEEP_PUBLISH.md` §4 item 2's open question ("what does the
Sequans accept as a prompt abort — UNKNOWN"), `build/bench-logs/phaseW-prompt.log`:

```
D (827577) WalterModem: TX: AT+SQNSMQTTPUBLISH=0,"pager/test-pager/up",1,44
D (827587) WalterModem: RX: \r\n> 
W (857577) WalterModem: Command time-out (TX) Attempt 1 of 3
D (857577) WalterModem: TX: AT+SQNSMQTTPUBLISH=0,"pager/test-pager/up",1,44
D (857587) WalterModem: RX: \r\n+SQNSMQTTPUBLISH: 5\r\n
D (857587) WalterModem: RX: \r\n\r\n
D (857587) WalterModem: RX: OK\r\n
D (857587) WalterModem: RX: \r\n+CME ERROR: 4\r\n
D (858137) WalterModem: RX: \r\n+SQNSMQTTONPUBLISH:0,5,0\r\n
```

The modem answered the `> ` prompt and then waited the full 30 s command timeout with **no
self-timeout of its own** — it was still sitting at the prompt, owed 44 bytes, when
`_processModemCMD()`'s old retry re-transmitted the 44-byte AT command line itself. The modem
consumed exactly 5 of those bytes as the promised payload (`AT+SQ`, hence `+SQNSMQTTPUBLISH: 5`
acking a 5-byte publish) and answered the leftover 39 bytes with `+CME ERROR: 4` (bad
parameter) — i.e. the *bytes owed to the prompt* are satisfied by whatever the host sends next,
genuine payload or not, and once satisfied the modem happily reports success on the truncated
mess. `+SQNSMQTTONPUBLISH:0,5,0` (rc 0 = success) confirms the modem's own bookkeeping believed
that 5-byte publish succeeded.

**Fix**: `_processModemCMD()`'s `DATA_TX_WAIT`/`TX_WAIT` retry branch, on a **timeout** (not a
`RETRY_AFTER_ERROR` resend, and only when `cmd->type == WALTER_MODEM_CMD_TYPE_DATA_TX_WAIT &&
cmd->payload != NULL`): on the FIRST such timeout, send `cmd->payload`/`cmd->payloadSize` bytes
directly (the same `uart_write_bytes()`/`_uart->write()` the prompt handler itself uses) instead
of re-transmitting the AT command line, and keep waiting for the normal `OK`. This is a late but
genuine completion of the original publish if the modem is still at the prompt (as the bench log
shows happening *by accident* via the old command-line retransmit); if the modem was not actually
at the prompt, these bytes are read as a bogus command line and answered with `+CME ERROR`/timeout,
which the existing error/retry path already handles — strictly better than the old behaviour, which
published the AT command *text* as the message body. On a SECOND timeout (tracked by the new
`WalterModemCmd::dataTxPayloadRetried` flag, reset to `false` for every new command in
`_queueModemCMD()`), the command fails outright (`WALTER_MODEM_STATE_TIMEOUT`) instead of trying a
third time — retrying blind against a modem in an unknown state is what caused the original bug;
`net.cpp`'s own `DATA_TX_WAIT`-timeout recovery (session teardown/reconnect) is the honest way to
recover from there, per §4 item 2's "treat a DATA_TX_WAIT timeout as a dead session" rule.

**Power effect**: none beyond the pre-existing 30s-per-attempt command timeout window; the failure
path now takes at most 2 attempts (~60s) instead of up to 3 (~90s) before `net.cpp`'s reconnect
recovery runs.

### RCA §3 instrumentation (same patch, no behaviour change)

Four free-running counters, declared in `src/WalterDefines.h` as
`walter_modem_pager_counters_t`/`walter_modem_pager_counters()` (the read-only accessor,
`extern "C"`, defined once in `WalterModem.cpp`) so `firmware/main/net.cpp` can expose them to
`modes.c`'s sleeptest report — the only thing that survives the USB-dead light-sleep window:

- `datatx_retx` — incremented in `_processModemCMD()` (above) each time the payload-instead-of-
  command-line retransmit runs.
- `prompt_orphan` — incremented in `_parseRxData()` each time patch 1.11's bare `"> "` `prompt3`
  case matches. This is the discriminator for whether RCA §2's orphaned-prompt mechanism is what is
  actually happening on a given bench run.
- `buf_drop_queue` — incremented in `_queueRxBuffer()` (`WalterModem.cpp`) when the 8-slot
  `_taskQueue` is full and a fully-parsed buffer is dropped instead of queued.
- `buf_drop_pool` — incremented in `_getFreeBuffer()` (`WalterModem.cpp`) when the 8-buffer pool is
  exhausted and a buffer allocation fails.

`firmware/main/xport_lte.cpp` separately keeps a 12-entry RAM ring of recent `mqttPublish()`
attempts (topic tail, length, issue time, outcome, elapsed ms), exposed via `net_get_publish_ring()`
(`net.h`) — outside this vendored component, documented in `net.h`'s own comment on
`net_publish_ring_entry_t`. Both are printed by `modes_debug_sleeptest_report()`
(`firmware/main/modes.c`) and, for the counters, by the `mqtttest` debug console command
(`firmware/main/main.c`).

## 1.13 Per-command timeout for MQTT publish/subscribe/disconnect/config, plus stall-attribution counters (`src/WalterModem.cpp`, `src/WalterModem.h`, `src/WalterDefines.h`, `src/proto/WalterMQTT.cpp`)

`docs/SLEEP_URC_TASKS.md` S3/S6, `docs/RCA_SLEEP_URC.md` §5 fix 3-4 and §1's own arithmetic
(`build/bench-logs/phaseAB-report.log:101`, `phaseAD-report.log:29`: two 30 010 ms `/up` publish
ring entries): every AT command shared one global timeout/attempt count
(`CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS` × `WALTER_MODEM_DEFAULT_CMD_ATTEMPTS`, 30 s × 3 = 90 s worst
case per stall), which §1 could not use to tell two simultaneous 30 s stalls apart, and which
`docs/SLEEP_URC_DESIGN.md` §6's "Watchdog arithmetic" shows **two** such stalls in one watchdog
stage (180 s) exceed the 95 s budget (`watchdog.c:25`) and stop feeding — the likely mechanism of
the uncaptured phaseAA reboot.

**Fix (1): per-command timeout.** `WalterModemCmd` gains a `timeoutTicks` field (default `0`,
meaning "use the library's own `WALTER_MODEM_CMD_TIMEOUT_TICKS` default" — resolved inside
`_queueModemCMD()`'s own translation unit, since that macro is private to `WalterModem.cpp` and not
visible from the header). `_queueModemCMD()` gains a matching optional last parameter,
`cmdTimeoutTicks`; every existing caller that does not pass it keeps the exact 30 s × 3 default.
`_processModemCMD()`'s `TX_WAIT`/`DATA_TX_WAIT`/`WAIT` deadline checks now read `cmd->timeoutTicks`
instead of the raw macro. `src/proto/WalterMQTT.cpp`'s `mqttPublish()`, `mqttSubscribe()`,
`mqttDisconnect()` and `mqttConfig()` — the four commands PROTOCOL.md's own session/keepalive
traffic actually blocks on — pass `10 s / 2 attempts` (`PAGER_MQTT_CMD_ATTEMPTS`,
`PAGER_MQTT_CMD_TIMEOUT_TICKS`). `mqttConnect()` (attach/TLS/connect) is left at the 30 s / 3
default: it already has its own 30 s M1 connect-watchdog in `xport_lte.cpp` on top of the command's
own timeout, and the task brief asks for attach/TLS/connect to stay untouched. `WalterModem::sendCmd()`
(the generic raw-AT entry point `xport_lte.cpp`'s liveness-ping re-SUBSCRIBE and the debug console's
`at` command both use) is also left untouched — it has no way to know which underlying AT command a
caller is sending, so a blanket override there would change every raw-AT caller's timeout, not just
MQTT's.

Arithmetic after this fix (docs/SLEEP_URC_DESIGN.md §6): one stalled MQTT command = 10 s × 2 = 20 s
(was 90 s); the worst case for a stage with four such commands outstanding in sequence is
4 × 20 s = 80 s, still under the 95 s watchdog budget (S6 confirms patch 1.10 already feeds both
watchdogs from inside the blocking wait, so this budget is what actually matters). Patch 1.12's
`DATA_TX_WAIT` payload-write-on-timeout recovery is unchanged — it still fires on a command's first
timeout, just after 10 s instead of 30 s for these four commands.

**Fix (2): stall attribution.** Four more counters/fields in `walter_modem_pager_counters_t`
(`WalterDefines.h`): `prompt_handled` (the `"> "` prompt handler in `_processModemRSP()` actually
wrote a payload — previously implicit, now counted), `payload_bytes_written` (the return value of
`uart_write_bytes()` at both payload-write sites — the prompt handler and patch 1.12's
timeout-triggered write — previously discarded entirely), `txdone_timeouts` (`_uartWrite()`'s
`uart_wait_tx_done(_uartNo, pdMS_TO_TICKS(10))` call now keeps its return value instead of dropping
it, and counts every non-`ESP_OK` result — 10 ms at 115200 baud is ~115 bytes, so a longer command
line can legitimately return unflushed), and a stall snapshot (`stall_cmd` — first 24 characters of
the AT command line; `stall_elapsed_ms`; `stall_cts_level` — `gpio_get_level()` on the CTS pin;
`stall_tx_ring_bytes` — `uart_get_tx_buffer_free_size()`, which reads 0 on this UART's 0-byte TX
ring, `uart_driver_install(uartNo, UART_BUF_SIZE * 2, 0, 0, NULL, 0)` in `begin()` — `txdone_timeouts`
above is the real discriminator for a wire-level stall, this field is included because the task
asked for it). `WalterModem::_pagerSnapshotStall()` (new private static method) is called from
`_processModemCMD()` every time it re-evaluates a still-pending command whose current attempt has
run for >= 5 s, overwriting the previous snapshot — it always reflects the most recently observed
slow command, not necessarily one still stalled. `firmware/main/net.cpp`/`modes.c` print all of the
above as two new sleeptest report lines, `tx counters: ...` and (only when `stall_cmd` is non-empty)
`stalled command: "..." elapsed=... ms cts=... tx_ring_free=... B`.

**Power effect**: none — the timeout/attempt changes only shorten how long a stall can block before
the existing recovery paths run (a net power *saving* per occurrence, 0.33 mAh → 0.13 mAh per S3's
own estimate); the counters are plain reads/increments, no extra AT traffic.

## 1.14 `checkComm()` takes a per-command attempt/timeout budget, so the URC drain probe can be expendable (`src/WalterModem.cpp`, `src/WalterModem.h`)

23 Sep S7 post-mortem (`build/bench-logs/phaseAF-report.log`, lines 27-28/33/41). `checkComm()`
sends a bare `AT` and is the one library call `firmware/main/net.cpp`'s per-wake URC drain probe
uses (`net_urc_probe()`, S1). The probe is specified as fire-and-forget and **non-retrying**
(`docs/SLEEP_URC_DESIGN.md` §5(1)), but `checkComm()` gave it the library default of
`CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS` × `WALTER_MODEM_DEFAULT_CMD_ATTEMPTS` = 30 s × 3. Because the
library runs exactly one command at a time (`_curCmd`, `WalterModem.cpp:1650-1706`), one unanswered
probe held that single slot for a full 30 s attempt and every later command queued behind it:
`stalled command: "AT" elapsed=30000 ms`, two `/up` ack publishes completing in 29 824 ms in one
6 min window, and 8 of 13 probes unanswered for >= 4 wake intervals.

**Fix.** Patch 1.13 already put `timeoutTicks`/`maxAttempts` on `WalterModemCmd` and an optional
`maxAttempts`/`cmdTimeoutTicks` pair on `_queueModemCMD()`. This patch adds the same two optional
parameters to `checkComm()` and passes them straight through `_runCmd`. Defaults are
`WALTER_MODEM_DEFAULT_CMD_ATTEMPTS` and `0` ("use the library default"), so **every pre-existing
caller — including `net_check()`'s blocking health check — is bit-for-bit unchanged**. Only
`net_urc_probe()` passes the short budget (1 attempt / 2 s, `PAGER_URC_PROBE_ATTEMPTS` /
`PAGER_URC_PROBE_TIMEOUT_MS` in `net.cpp`).

Why a short budget cannot lose a page: the modem releases its queued URCs when it **accepts** a
command, not when the host observes the answer, so the flush the probe exists to trigger has already
happened by the time the 2 s deadline matters. The only thing given up is the knowledge that a late
`OK` arrived, which nothing uses. It also un-inverts a bound: `net_probe_guard`'s stuck bound is
`NET_PROBE_GUARD_STUCK_WAKES` (3) further wake intervals, i.e. >= 6 s even in ACTIVE mode, so with
2 s the library always releases the slot before the guard reissues — previously the guard gave up
first and queued a second probe behind a still-live one, spending one of the 8 shared queue slots
for nothing.

**Power effect**: a saving. Each avoided 30 s block is ~0.33 mAh of ESP awake time (30 s × 40 mA),
plus up to 15 s of `publish_quiet`'s sleep-hold (`PUBLISH_SLEEP_HOLD_MAX_US`) and up to 30 s of
`net_modem_busy()`'s, i.e. up to ~0.8 mAh per page. Assumptions: 40 mA awake / 1 mA light sleep
(`docs/SLEEP_URC_DESIGN.md` §2) — estimated, not measured on this board. No extra AT traffic; no
change to the 4 bytes on the wire.

## 1.15 Stall-mechanism discriminator counters, not a trace (`src/WalterModem.cpp`, `src/WalterModem.h`, `src/WalterDefines.h`)

`docs/SLEEP_URC_TASKS.md` S10, `docs/SLEEP_URC_DESIGN.md` §8.1-§8.2. `phaseAF`'s
`stalled command: "AT" elapsed=30000 ms` rules out the wire (`txdone_timeouts=0`,
`buf_drop_queue=0 buf_drop_pool=0`, `probe_answered=13` — the bytes went out, nothing was dropped,
the retry was answered), which leaves three candidates on the response-pairing side: (1) a response
completes the wrong command inside a URC flush burst (the shared FIFO pairs whatever buffer arrives
next with whatever command happens to be `_curCmd` at dequeue time, `_processModemRSP(_curCmd, …)`
at `_cmdProcessingTask`); (2) `_receivingPayload` sticks true across a burst, so every byte is
consumed as payload and nothing is queued at all until the command times out; (3) the modem
genuinely did not answer. There is no in-window AT trace to settle it with — light sleep kills the
USB CDC — so this patch adds counters instead.

**`rsp_no_cmd`** (`WalterModem.cpp`'s `_processModemRSP()`, `RSP_PROC_FINISH` region): incremented
when a buffer reaches the function's completion test with `cmd == NULL` and `result == OK`, i.e. it
falls through unused instead of completing a command or being claimed by an earlier region's own
`if (cmd == NULL) return`. By the time this point is reached, `cmd == NULL` already implies
`result == OK` (the `cmd == NULL && result != OK` case returns earlier, patch 1.1). Hypothesis 1
(desync) predicts >= 1 of these per stall; hypotheses 2 and 3 predict 0.

**`payload_stuck_ms`** (`WalterModem.cpp`'s `_processModemCMD()`, the `TX_WAIT`/`DATA_TX_WAIT` and
`WAIT` timeout paths, plus one new `TickType_t _receivingPayloadSetAt` on `WalterModem.h` stamped
wherever `_receivingPayload` is set true in `_parseRxData()`): how long `_receivingPayload` had
already been true when a command genuinely timed out with it still set. Hypothesis 2 predicts > 0;
1 and 3 predict 0. Overwritten on every such observation — most recently observed, not necessarily
still stuck.

**`probe_first_attempt_ms`** (`firmware/main/net.cpp`, not this component): the URC drain probe's
own issue-to-answer elapsed time, stamped immediately before `WalterModem::checkComm()` and read
inside `probe_cb()` whichever way the probe resolved. Hypothesis 3 predicts this stays near the
probe's own 2 s budget during a stall; 1 and 2 predict the probe itself completes quickly, because
the flush that unblocks the queue happens when the modem *accepts* a command, not when the host
sees the answer.

All three are printed on one new sleeptest report line, `stall discriminator: rsp_no_cmd=…
payload_stuck_ms=… probe_first_attempt_ms=…` (`firmware/main/net.cpp`'s `net_get_pager_counters()`/
`net_get_probe_counters()`, `firmware/main/modes.c`'s report).

**Power effect**: none — three plain reads/increments, no extra AT traffic, no behaviour change.
This patch does not attempt a fix; the fix depends on which hypothesis the counts support.

## 1.16 A failed hardware `reset()` no longer costs ~91 s: the discarded `+SYSSTART` banner is remembered, and the wait budget matches the datasheet, not the library default (`src/WalterModem.cpp`, `src/WalterModem.h`)

`docs/SLEEP_URC_TASKS.md` S17, `docs/SLEEP_URC_DESIGN.md` §9.1-§9.2 item 3. `phaseAI-report.log`'s
`asleep 253 s` against `80 light sleeps` at a 2 s interval (160 s of real sleep) left ~93 s
unaccounted for; the arithmetic that explains it is `reset()`'s own failure mode: `reset()` pulses
the reset pin, waits 1000 ms for the line to settle, then queues a `TX_WAIT` for `"+SYSSTART"` with
the library's unmodified default (`WALTER_MODEM_DEFAULT_CMD_ATTEMPTS` x
`CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS` = 3 x 30 s = 90 s) — but `_parseRxData()` discards every byte
received while `_hardwareReset` is true, which is exactly that 1000 ms window. If the modem's boot
banner is transmitted inside it (plausible: it is one of the first things a GM02SP sends, and this
board's own README notes the datasheet boot time is ~2-3 s, so a 1000 ms window can catch the leading
edge of it depending on exact timing), the wait then genuinely never sees it and burns the full 90 s
every time. 1 s + 90 s = 91 s, matching `phaseAI`'s ~93 s excess to within the measurement.

**Fix, two independent halves (per the task brief, either one alone is an improvement; both are
applied).**

**(a) Remember the banner instead of parsing it.** A new instance-wide flag,
`_sawSysStartDuringReset` (`WalterModem.h`, next to `_hardwareReset`), is set by `_parseRxData()`
when the raw bytes it is about to discard (because `_hardwareReset` is true) contain the literal
substring `"+SYSSTART"` (`memmem()`, already used elsewhere in this file for the same kind of
substring match). No buffer is queued and no command completes from this check alone — it is purely
a memory of "the banner arrived, even though nothing can use it yet". `reset()` clears the flag at
its own start (so a stale `true` from an earlier hardware reset can never leak forward), and
`_processModemCMD()`'s very first evaluation of a `TX_WAIT`/`DATA_TX_WAIT` command whose `atRsp` is
literally `"+SYSSTART"` consults and clears it: if set, the command completes immediately with
`WALTER_MODEM_STATE_OK` instead of transmitting nothing and waiting. Scoped to the exact string, not
the type, because `softReset()` (`AT^RESET`, no pin pulse) queues an identical `"+SYSSTART"` wait
without ever setting `_hardwareReset` — the flag is simply never true when its command runs, so this
change cannot affect that path. Known limitation, stated where the check lives: a `"+SYSSTART"` split
across two separate UART reads at exactly the discard/no-discard boundary is still missed; rare for a
9-byte token, and bounded by half (b) below regardless.

**(b) A 10 s, 1-attempt budget for the wait itself** (`PAGER_RESET_SYSSTART_TIMEOUT_TICKS` /
`PAGER_RESET_SYSSTART_ATTEMPTS`, patches 1.13/1.14's existing `maxAttempts`/`cmdTimeoutTicks`
parameters on `_queueModemCMD()`, now also passed from `reset()`'s own `_runCmd()` call). The GM02SP
boots in ~2-3 s; 10 s leaves ample margin without inheriting the library's 30 s x 3 default built for
ordinary AT commands over an already-attached radio. Applied unconditionally, independent of (a): if
(a)'s substring scan ever misses the banner (the split-read case above, or a genuinely slow boot),
this still fails in 10 s instead of 90 s, and `net_recover_modem()`'s caller already handles a false
return from `reset()` — no new recovery machinery needed.

**Power effect**: (a) removes the ~91 s stall entirely on the common case (the banner reaches the
flag, the command completes at once); (b) bounds whatever (a) does not catch to 10 s. Worst case per
failed F4 falls from ~91 s to ~11 s: (91-11) s x 40 mA / 3600 = ~0.9 mAh per occurrence saved, and the
F4 is rate-limited to 6/hour, so the worst case this removes is ~5 mAh/h (`docs/SLEEP_URC_DESIGN.md`
§2: 40 mA awake / 1 mA light sleep, estimated, not measured on this board). No extra AT traffic; no
change to the 4-byte reset sequence itself.

## 1.17 Application-installable UART/response trace hook, for a debug-build flight recorder (`src/WalterModem.h`, `src/WalterModem.cpp`, `src/WalterDefines.h`)

`docs/SLEEP_PAGE_LOSS_BRIEF.md` §6 item A: nobody has ever seen, byte for byte, what the modem
sends during a wake's post-probe wait -- the USB console dies the moment the pager first light-
sleeps, so there is no live AT trace across the window that matters. `firmware/main/flightrec.c`
(debug build only, `PAGER_DEBUG_NO_LIGHT_SLEEP`) adds a PSRAM ring that survives light sleep and
is dumped after the fact, but it needs a way to see the library's own UART traffic without
duplicating `WalterModem.cpp`'s RX/TX/response-pairing logic in `firmware/main`.

**Fix.** A single, null-checked function pointer, `walter_pager_trace_fn` (`WalterModem.h`, right
before `class WalterModem`): `void (*)(char kind, const uint8_t *data, size_t len)`.
`WalterModem::setPagerTraceHook(fn)` installs it (or clears it with `NULL`) into a file-static
`s_pagerTraceHook` in `WalterModem.cpp`, next to the existing pager counters (same "plain static,
single UART/URC task, no atomics" reasoning as those). Called, if non-NULL, at exactly three
sites, none of which change behaviour or control flow:

- **`'R'`** -- `_handleRxData(void*)`, the ESP-IDF task variant only (the `ARDUINO` variant is
  dead code on this target and is left untouched), right after `_uartRead()` returns, with
  `incomingBuf`/`uartBufLen`, only when `uartBufLen > 0`. The rawest possible view of the UART:
  before `_parseRxData()` does anything with the bytes.
- **`'T'`** -- inside the `_transmitCmd` macro's ESP-IDF branch (`WalterDefines.h`), once per
  non-NULL `atCmd[i]` element, right before that element's own `uart_write_bytes()`. The trailing
  `"\r\n"`/`"\n"` is deliberately not traced separately -- the hook's own doc comment states one
  call with the command string is enough, and `firmware/main/net.cpp`'s own wake-byte write
  records its `"\r\n"` itself (kind `'K'`/`'T'` pair, `net_urc_probe()`).
- **`'U'`** -- `_processModemRSP()`'s `RSP_PROC_FINISH` region, at the exact `s_pagerCntRspNoCmd++`
  site added by patch 1.15: the same unpaired buffer (`buff->data`, `buff->size`), so the flight
  recorder sees exactly what that counter counts, not a re-derived approximation of it.

**Power effect**: none -- a null pointer check and, only while a debug build's `sleeptest` window
has called `flightrec_set_recording(true)`, one function call per already-occurring UART
read/write/unpaired-buffer event. No extra AT traffic, no new task, no change to any timeout,
retry, or parsing decision. Release builds link the same three call sites (the hook is not
`#ifdef`-gated in this component -- only `firmware/main/flightrec.c`'s own body is, per its module
comment) but `s_pagerTraceHook` is never set to non-NULL outside a `PAGER_DEBUG_NO_LIGHT_SLEEP`
build, so the branch is always false there.

## 1.18 A stray leading 0xFF at the start of a wake's first message was glued onto the next line, so the probe's own `OK` was never credited and URCs were lost (`src/WalterModem.cpp`, `src/WalterDefines.h`)

`docs/SLEEP_PAGE_LOSS_BRIEF.md`, `build/bench-logs/phaseBB-report.log` (the flight recorder,
patch 1.17): on every observed wake from light sleep (13/13 cycles), the modem UART delivers
exactly one `0xFF` byte in the same millisecond as the wake-path UART reconfiguration, before
anything else. `_parseRxData()` assumes every message starts with `"\r\n"` and looks for the
trailing CRLF from `buf->data + 2`, so the `0xFF` was glued onto the *next* line instead of being
its own (zero-length) message: cycle c01, `"\xff\r\nOK\r\n"` -- the probe's own `OK`, not
credited, so the probe fell through to its 15 s wait and every later command queued behind it;
cycle c08, `"\xff\r\n+SQNSMQTTONMESSAGE:0,...\r\n"` -- the page URC, not recognised, page lost for
good (`m_23f9bd83`). Lines that arrive after some other line has already absorbed the `0xFF` (e.g.
c03's `"\xff\r\n+CME ERROR: 4"`, still parsed as one malformed-looking-but-harmless URC) show the
same byte, just attached somewhere it does not break anything -- the failure mode above is what
happens when it lands on a line the modem code actually keys off of.

**Fix.** One rule, at the top of `_parseRxData()`'s per-message loop body, before the known-size
payload branch: if the current message is not part of a binary payload (`!_receivingPayload` --
covers both the known-size branch below and the unknown-size-payload case, which reuses the
CRLF-search path further down), the parser buffer is empty (`_parserData.buf == NULL ||
_parserData.buf->size == 0`), and the first byte of the message is `0xFF`, drop that one byte
(`offset += 1; continue;`) and count it (`s_pagerCntGlitchDropped`, exposed as `glitch_dropped` on
`walter_modem_pager_counters_t` next to patch 1.15's `rsp_no_cmd`/`payload_stuck_ms`, and printed
in `modes.c`'s `modem counters:` line after `modem_resets=`). Safe by construction: a legitimate AT
response or URC always begins a fresh, empty parser buffer with `"\r\n"`, never `0xFF`, so the
condition can only ever match a genuine glitch byte at the very start of a message -- never a byte
already inside a binary payload (the known-size branch is excluded by `!_receivingPayload`, and
the unknown-size case only ever reaches this buffer-empty check on its first byte, before any
payload content has accumulated).

**Power effect**: none of its own -- one comparison and, at most once per wake, one byte dropped
and one counter incremented. What it *enables*: the probe's `OK` is credited on the wake it
actually arrives, instead of falling through to the 15 s wait `docs/SLEEP_PAGE_LOSS_BRIEF.md`
attributes to this bug -- that is where the time (and the page) was actually being lost. No
change to the wake bytes, the probe, the wait, or any cadence constant.

## 1.19 A finished command left in `_curCmd` gets re-finished by a stray post-wake response and wedges the command slot forever (`src/WalterModem.cpp`, `src/WalterDefines.h`)

24/25 Sep task-watchdog resets, `stage=mqtt status/retry` (relay status log, `rst=6`), pager
untouched on the bench, 5-6 min after boot. `docs/SLEEP_PAGE_LOSS_BRIEF.md`'s own flight-recorder
evidence (patch 1.17, `build/bench-logs/phaseBE-report.log`) already showed the mechanism next
door: wakes c06/c15/c17 drew a `+CME ERROR: 4` from the modem right after the wake bytes'
`"\r\n"`; wakes c07/c16 did not. Since patch 1.18 fixed the leading-0xFF glue bug, that
`"+CME ERROR: 4"` line now parses cleanly on its own -- which turned an already-latent race into
a real one.

**Mechanism.** `_finishModemCMD()` (`WalterModem.cpp`) moves a command to
`WALTER_MODEM_CMD_STATE_SYNC_LOCK_NOTIFIED` ("done, waiting for the caller") and notifies the
caller's condition variable; the blocking caller (`WalterDefines.h`'s `_returnAfterReply()`) wakes,
reads the result, and only THEN sets the command's state to `WALTER_MODEM_CMD_STATE_COMPLETE` and
unlocks. Between those two moments -- and, worse, between a command actually finishing and
`_cmdProcessingTask`'s own next loop iteration clearing `_curCmd` at all (`WalterModem.cpp`,
the `SYNC_LOCK_NOTIFIED`/`COMPLETE` cases) -- the finished command is still `_curCmd`. FreeRTOS
ticks stop during light sleep, so this window can span an entire sleep-and-wake cycle: the command
processing task is simply not scheduled to advance past it. On wake, the "\r\n" wake bytes
(`net.cpp`) sometimes draw a `+CME ERROR: 4` from the modem (phaseBE c06/c15/c17); before this
patch, `_cmdProcessingTask`'s response-pairing site (`else if(qItem.rsp != NULL) {
_processModemRSP(_curCmd, qItem.rsp); }`) handed that stray line to whatever `_curCmd` happened to
be -- the already-finished command sitting in the slot. The error handler
(`_processModemRSP()`'s error-line region) calls `_finishModemCMD()` on it a SECOND time, which
sets its state back to `SYNC_LOCK_NOTIFIED` and calls `notify_one()` on a condition variable
nobody is waiting on any more (the original caller already consumed the first notification,
already returned, and its `std::unique_lock` is gone). `_cmdProcessingTask`'s own switch statement
then hits the `SYNC_LOCK_NOTIFIED` case forever ("We need to wait until the other thread is
ready") -- the other thread never comes back. `_curCmd` is wedged permanently: `_curCmd = qItem.cmd`
only runs `if(_curCmd == NULL)`, so every later command queues behind a slot that will never
empty. The first blocking command downstream (the liveness `sendCmd` re-SUBSCRIBE,
`xport_lte.cpp`) waits forever, and the task watchdog fires roughly 155 s later in whichever stage
happens to be running (`watchdog.c`'s 95 s block-tick budget plus the task watchdog's own timeout
from the last real feed) -- `stage=mqtt status/retry` in the observed resets.

**Fix.** At the exact response-pairing site (`_cmdProcessingTask`, the `qItem.rsp != NULL`
branch): pair a response with `_curCmd` only when `_curCmd != NULL && _curCmd->state ==
WALTER_MODEM_CMD_STATE_PENDING` -- the one state that means "sent, waiting for its reply". Any
other state (already finished and waiting for its caller, mid-retry, or a stale
`FREE`/`POOLED`/`NEW` leftover) means this response cannot legitimately belong to it, so it is
paired with `NULL` instead. This is already a safe, exercised path: patch 1.1 drops an error line
with no command pending without crashing, and a stray `OK` with no command is freed unused (patch
1.15's `rsp_no_cmd` counts exactly this case).

**Instrumentation.** A new free-running counter, `s_pagerCntRspStaleCmd`
(`walter_modem_pager_counters_t.rsp_stale_cmd`, next to patch 1.15's `rsp_no_cmd`), incremented at
the same site whenever `_curCmd` is non-NULL but not `PENDING` -- i.e. every time this fix actually
prevented a re-finish. `firmware/main/modes.c`'s `stall discriminator:` sleeptest report line
prints it right after `rsp_no_cmd`, so a non-zero count on a bench run is direct confirmation the
race fired (and, before this patch existed to prevent it, would have wedged the command slot).

**Power effect**: a large saving on the (previously silent) wedge case -- what used to end in a
~155 s task-watchdog stall and a full reset now costs nothing beyond the one comparison and
(rarely) one counter increment this fix adds to a code path that already ran on every response.
No extra AT traffic, no change to the wake bytes, the probe, or any retry/timeout constant.

## 1.20 Optional per-call attempt/timeout budget on `getVoltage()`, `getRSSI()`, `sendCmd()` and `mqttReceive()` (`src/WalterModem.cpp`, `src/WalterModem.h`, `src/proto/WalterMQTT.cpp`)

Companion to 1.19 above (same task, same evidence): patch 1.19 closes the mechanism that let one
wedged command block every later one; this patch bounds how long any single one of these four
commands can legitimately block in the first place, the same `maxAttempts`/`cmdTimeoutTicks`
pattern patches 1.13/1.14 already established for the MQTT publish/subscribe/disconnect/config
commands and `checkComm()`. Without it, a genuinely slow (not wedged) reply to any of these four
still costs the library's global default, `CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS` x
`WALTER_MODEM_DEFAULT_CMD_ATTEMPTS` = 30 s x 3 = 90 s, and the task brief's own arithmetic shows
two such 90 s waits inside one 95 s watchdog stage-tick budget still stalls it (`watchdog.c`'s
`WD_MODEM_BLOCK_BUDGET_MS`) even with 1.19 applied.

**Fix.** `getVoltage()` and `getRSSI()` (`src/WalterModem.cpp`) and `sendCmd()` (`src/WalterModem.cpp`)
each gain the same two optional trailing parameters 1.13/1.14 added to `checkComm()`/`mqttPublish()`
etc: `uint8_t maxAttempts = WALTER_MODEM_DEFAULT_CMD_ATTEMPTS, TickType_t cmdTimeoutTicks = 0`,
passed straight through to `_runCmd`/`_queueModemCMD` exactly as those patches did. `mqttReceive()`
(`src/proto/WalterMQTT.cpp`) gains the same two parameters on both of its branches (qos-0 and
qos>0). Every existing caller that does not pass the new parameters keeps the library's exact
30 s x 3 default -- bit-for-bit unchanged, same guarantee 1.13/1.14 gave. `firmware/main`'s call
sites (net.cpp's `net_get_battery_mv()`/`net_get_rssi()`, xport_lte.cpp's liveness `sendCmd()` and
`mqttReceive()`) are the only callers that pass a shorter budget -- see this task's own arithmetic
at each site for the numbers.

**Power effect**: none of its own -- purely additional optional parameters with defaults that
reproduce every existing caller's behaviour exactly. The power effect belongs to whichever caller
actually passes a shorter budget (documented at each of those call sites in `firmware/main`).
