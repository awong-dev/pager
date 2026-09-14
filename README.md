# Low-powered Kid Pager

Using expensive smart watches with also expensive cellular plans just so kids
can message their parents is ridiculous.

This is an attempt to build a simple pager-style device that can be fitted
with an ultra-cheapo SIM card (eg the $4/month ones from US Mobile with
just the minmal data and messaging). It should allow sending very simple
messages and location to a custom endpoint and hopefully run for weeks on
one charge.

## Status

MVP (text relay only — see `HANDOFF.md` §1 for what's explicitly out of scope) is code-complete:
relay, protocol, device simulator, parent web page, and firmware are all implemented and verified
everywhere possible without a physical device. Nothing has run on real hardware yet — that's the
actual next step, not more code. See `HANDOFF.md`'s status note at the top for the current
punch list.

## Repo map

| Path | What it is |
|---|---|
| `docs/PROTOCOL.md` | Authoritative wire contract — topics, message schema, ack state machine, power/latency budget. Code must conform to this, not the reverse. |
| `HANDOFF.md` | Original build brief + phase-by-phase history + current status. |
| `relay/` | FastAPI relay + MQTT gateway + parent web page. See `relay/README.md` to run it locally. |
| `firmware/` | ESP-IDF firmware for the Walter (ESP32-S3 + Sequans GM02SP) device. See `firmware/README.md` for build instructions, hardware measurement checklist, and known residual risks. |
| `tools/` | `send.py` (send a message via the relay), `sim_device.py` (fake device for testing), `e2e_test.py` (integration test suite against a real broker). |
| `.github/workflows/ci.yml` | Runs the relay's unit tests and the end-to-end suite on push. |
