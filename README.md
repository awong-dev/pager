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
| `docs/SERVER_PLAN.md` | Plan for the v2 server stack: user registry, allow-lists, location, multi-backend delivery (web app / SMS / Google Chat), Next.js+MUI web app on Firebase (Firestore, Auth, FCM, Hosting), scale-to-zero Cloud Run relay behind the broker's rule engine, all via Terraform, and the Python MQTT test client. Not started. |
| `HANDOFF_V2.md` | Execution brief for building the v2 server stack unattended on Sonnet: rules, phases, model policy, kickoff command (`docs/kickoff-v2.md`). |
| `HANDOFF.md` | Original build brief + phase-by-phase history + current status. |
| `relay/` | FastAPI relay + MQTT gateway + parent web page. See `relay/README.md` to run it locally. |
| `firmware/` | ESP-IDF firmware for the Walter (ESP32-S3 + Sequans GM02SP) device. See `firmware/README.md` for build instructions, hardware measurement checklist, and known residual risks. |
| `tools/` | `send.py` (send a message via the relay API), `pager_client.py` (v2 test client — simulated device + server driver, `docs/SERVER_PLAN.md` §8), `e2e_v2.py` (v2 end-to-end suite against the real docker-compose stack), `mocks/twilio_mock.py` (Twilio Messages API test double), `emqx_setup.py` (provisions the broker's rule engine for local dev). |
| `.github/workflows/ci.yml` | Runs the relay's unit tests and the end-to-end suite on push. |
