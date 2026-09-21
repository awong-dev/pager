# Bug reports for Sequans / DPTechnics (drafts, not yet sent)

Fill in the bracketed identity fields before sending. The first goes to Sequans (their forum, or
through DPTechnics); the rest to <https://github.com/QuickSpot/walter-esp-idf/issues>. Every library
bug below is fixed in this repo's vendored copy; `firmware/components/dptechnics__walter-modem/
PATCHES.md` has the patches, which can be offered upstream as they are.

Common details:

- Hardware: DPTechnics Walter, Sequans GM02SP. Modem firmware `LR8.2.1.0-61488`.
- IMEI `[fill in]`, IMEISV `[fill in]`, SVN 19 (from `AT+CGSN=2`).
- Library: `dptechnics/walter-modem` v1.5.0 (ESP-IDF port, commit `51b16ce`).

---

## 1. Security: `AT+SQNSMQTT*` silently falls back to plaintext when the TLS profile names no CA slot

**To: Sequans.** Severity: credentials disclosed in clear text; a TLS-only broker then hangs the
client forever.

**Steps**

```
AT+SQNSPCFG=2,2,"",0,,,,"","",0,1,0        (TLS 1.2, certValidLevel 0, no caCertificateID)
AT+SQNSMQTTCFG=0,"client","user","pass",2   (spId = 2)
AT+SQNSMQTTCONNECT=0,"<host>",8883,60
```

All three return `OK`.

**Observed**, captured on a TCP listener we control (`socat -x`): the first bytes the modem sends
to port 8883 are a plaintext MQTT 3.1.1 CONNECT, `10 2d 00 04 4d 51 54 54 04 c2 ...`, with the
client id, username and password readable. No ClientHello is ever sent. Against a real TLS broker
the server waits for a handshake, the modem waits for a CONNACK, and `+SQNSMQTTONCONNECT` never
fires: no URC, no `+CME ERROR`, indefinitely.

**Expected**: either a TLS handshake (the profile asks for TLS 1.2 with validation off, which is a
legitimate configuration), or an error from `AT+SQNSMQTTCFG`/`AT+SQNSMQTTCONNECT`. Never a silent
downgrade.

**Control**: change only the profile to name a CA slot, `AT+SQNSPCFG=2,2,"",0,12,,,"","",0,1,0`
(validation still off), and the same engine sends a normal TLS 1.2 ClientHello with SNI. With
`certValidLevel` 1 and slot 12 it does too. The generic socket layer (`AT+SQNSSCFG` + `AT+SQNSD`)
performs TLS with the *first* profile as well, so the fallback is specific to the MQTT client.

**Questions**: is this known, and is it fixed in a later release? What does the MQTT client do
when the named slot exists but is empty? Could we get `LR8.2.2.x`/`LR8.2.3.x` for **GM02SP**?

## 2. Crash: a result line with no command pending dereferences NULL

**To: DPTechnics (`walter-esp-idf`).** `WalterModem.cpp`, `_processModemRSP()`, the block after
`after_processing_logic:`:

```cpp
if((cmd != NULL && cmd->atRsp != NULL && memcmp(...) == 0) || result != WALTER_MODEM_STATE_OK) {
    _finishModemCMD(cmd, result);      // cmd can be NULL on the right-hand side of the ||
```

`_finishModemCMD()` starts with `cmd->rsp->result = result`. Any error-class line that arrives
when no command is in flight (`NO CARRIER` is the easy one) panics with `LoadProhibited`.

**Reproduce**: `socketDial()` to a host that accepts TCP but never completes TLS. The library's 30 s
command timeout fires and it re-sends `AT+SQNSD`; the modem answers `NO CARRIER` for the retry and,
about 30 s later, a second `NO CARRIER` for the original. The second one arrives with no command
pending. Backtrace: `_finishModemCMD` ← `_processModemRSP` ← `_cmdProcessingTask`.

**Fix**: guard the call with `cmd != NULL` (free the buffer and return otherwise). Separately, the
library's command timeout for `AT+SQNSD` is shorter than the modem's own connect timeout
(`AT+SQNSCFG` `connTo`, 30 s here) plus a TLS handshake, which is what produces the double dial.

## 3. `mqttReceive()` copies out of a 1540-byte buffer without a bounds check

**To: DPTechnics.** `WalterMQTT.cpp` clamps the requested size to 4096, but the response is
delivered in a `WalterModemBuffer` whose `data[]` is `WALTER_MODEM_RSP_BUF_SIZE` = 1540 bytes, and
`_processModemRSP()` then does `memcpy(cmd->payload, rspStr, cmd->payloadSize)`. A payload larger
than the buffer reads past it. We have not crashed it (our largest message is 1468 bytes) but the
API advertises sizes it cannot deliver. Either clamp to the real buffer size and document it, or
reassemble across buffers.

## 4. The raw receive path can write past the response buffer

**To: DPTechnics.** `_addATByteToBuffer()` and `_addATBytesToBuffer()` append to
`WalterModemBuffer::data[WALTER_MODEM_RSP_BUF_SIZE]` with no bounds check. A response or URC
longer than 1540 bytes writes past the end of the buffer: memory corruption, not a wrong answer.
Fix: cap the copy at the space left and drop the excess.

## 5. Only socket id 1 can be addressed

**To: DPTechnics.** `WalterModemSocket::id` defaults to `1` for every entry of `_socketSet`, and
only `_socketReserve()` assigns a real id. `_socketGet(id)` searches by id, so
`socketConfig(4)` (or any id but 1) fails locally with `NO_FREE_SOCKET` before an AT command is
sent. Fix: map id N onto slot N-1.

## 6. `AT+SQNSRECV` times out when the payload ends in a line feed

**To: DPTechnics.** Reading a TLS socket whose payload ends in `\n` (a PEM file), the payload
buffer is queued one byte early and the final result arrives as `\n\r\nOK\r\n`. The
leading-CRLF strip does not fire (the buffer starts with `\n`), `OK` never matches, and the
command times out after 30 s although the data arrived in 100 ms. We normalise
"only CR/LF, then OK" to `OK`; the one-byte miscount in the payload parser is the real bug.

## 7. `+CEREG` drops the location area and cell id without PSM timers

**To: DPTechnics.** The `+CEREG` parser only keeps `lac`/`ci` for report formats that also carry
the PSM timer fields, so with `AT+CEREG=2` the cell identity is lost.

## 8. Two-digit year from an unset clock

**To: DPTechnics.** Right after attach `AT+CCLK?` can still return `70/01/01,...`. `getClock()`
reads the year as 2070 and returns a valid-looking epoch of 3155760008. Reject years before the
library's own build year, or return failure.
