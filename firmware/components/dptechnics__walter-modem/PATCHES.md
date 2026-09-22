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
