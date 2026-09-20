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
flight. `BRINGUP_NOTES.md`: "a stray `NO CARRIER` with no command pending
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
   `BRINGUP_NOTES.md`: "receive buffers are 1540 bytes and `mqttReceive()`
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

## Not applied here

**Patch 1.4 (SMS)** — `smsSend()`, a `+CMTI` SMS event handler, `smsRead()`,
`smsDelete()` — is `docs/V02_DESIGN.md` §6 territory and is left for that task.
