# Unfinished and unverified

Hardware tests still to run are listed in `HARDWARE_TESTING.md`. This is everything else.

## Decisions waiting on the owner

- **Soracom** (`SORACOM_EVAL.md`). If adopted, the pager's TLS and CA handling become unnecessary
  on those SIMs, and texting an ordinary phone number from the pager becomes impossible.
- Whether to send the three vendor bug reports (`VENDOR_BUG_REPORTS.md`).

## Known gaps

Firmware
- "Set up again" on the device menu is a stub; setup is console-only.
- A received SMS lives in RAM only; multipart texts arrive as separate messages; the boot-time
  scan of stored texts reads slots one by one.
- Location: the PSM-window radio route (cheaper than dropping the radio) is not built;
  accelerometer thresholds are datasheet defaults. Cell-based fallback position
  (`PROTOCOL.md` §13.2): the relay/tools half is built, but firmware does not yet send `cell` on a
  `no_fix` answer -- that is a separate, later firmware task, byte-compatible with the wire shape
  already in `PROTOCOL.md`.
- The payload parser in the modem library miscounts by one byte when a payload ends in a newline;
  the symptom is patched, the cause is not.
- The temporary diagnostics (`nettest`, `mqtttest`, the raw AT trace) are still compiled in.

Relay
- No retention sweep for the SMS audit log; it grows for ever.
- `ca_resolve.resolve_broker_ca()` is never called at startup, so the CA comes only from
  `BROKER_CA_PEM`.
- No end-to-end scenario for a CA push.
- The per-device APN field has an API but no web UI; the pager's own detection makes it rarely
  needed.
- Cell-tower location fallback (`PROTOCOL.md` §13.2) needs a real `CELL_GEO_API_KEY` before it
  resolves anything in production -- `CELL_GEO_PROVIDER=none` (the default, and every deployment's
  value until an owner does `infra/README.md`'s Google Geolocation API key step) stores only
  `devices/{d}.status.lastCell`, no coarse position. `opencellid`'s request/response shape is also
  `UNVERIFIED` (`relay/app/cellgeo.py`'s docstring) -- confirm it against the current API docs
  before ever selecting that provider.

Web
- No test runner. Validation logic is kept in pure modules so it can be tested later.

## Ideas on hold

- **Background location.** Try for a fix when the pager moves or changes cell, not only when
  asked, and keep a warm last-known position. Parked: it spends power speculatively and needs a
  rewrite of `PROTOCOL.md` §13.3.
- **Counter resync handshake.** A signed, challenged exchange in which the relay tells a pager the
  last counter it saw. Only needed if a pager ever loses its flash but keeps its identity, which
  cannot happen today (`DEVICE_PLAN.md` §2.5).
- **Encrypting message bodies** under the device key (`DEVICE_PLAN.md` §2.2, option E). Would hide
  pages and positions from the broker and from the path. A precondition for Soracom.
