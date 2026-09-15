# Low-powered Kid Pager

Using expensive smart watches with also expensive cellular plans just so kids
can message their parents is ridiculous.

This is an attempt to build a simple pager-style device that can be fitted
with an ultra-cheapo SIM card (eg the $4/month ones from US Mobile with
just the minmal data and messaging). It should allow sending very simple
messages and location to a custom endpoint and hopefully run for weeks on
one charge.

## Status

**Server side: built and tested.** The relay, protocol, Firestore data model, routing and
delivery backends (web app, SMS, Google Chat, the pager itself), location, the Next.js web app,
the Python test client and the Terraform deployment are all implemented, with unit tests and an
end-to-end suite running against a real local stack in CI.

**Never deployed, never run on hardware.** Three things need a human before this is a real
system: a Walter board to flash (see `firmware/README.md`'s measurement checklist and residual
risks), a GCP project and EMQX Cloud account to deploy into (see `infra/README.md`'s runbook),
and Twilio / Google Workspace accounts if you want the SMS and Google Chat backends (see
`relay/README.md`). Nothing about those is code work.

## Repo map

| Path | What it is |
|---|---|
| `docs/PROTOCOL.md` | Authoritative wire contract — topics, message schema, ack state machine, location, power/latency budget. Code conforms to this, not the reverse. |
| `docs/SERVER_PLAN.md` | Design reference for the server stack: user registry, allow-lists, location, multi-backend delivery, the web app, Firestore schema and security rules, cost analysis, and the Terraform layout. |
| `docs/DEVICE_PLAN.md` | Design plan (not yet implemented) for SIM-only device provisioning via a typed setup code, per-device HMAC authentication of every CBOR envelope, the server-approved on-device address book, the passcode lock, and the multi-screen e-paper UI. |
| `docs/DEVICE_TASKS.md` | The execution plan for `DEVICE_PLAN.md`: ordered, self-contained tasks per track (docs, server, tools, web, firmware) with files, steps and verification commands, written for an implementing agent. |
| `relay/` | FastAPI relay: webhook ingest, routing, delivery backends, admin and conversation APIs, retention. See `relay/README.md` to run it locally. |
| `web/` | Next.js + MUI web app on Firebase (Auth, Firestore listeners, FCM). See `web/README.md`. |
| `firmware/` | ESP-IDF firmware for the Walter (ESP32-S3 + Sequans GM02SP) device. See `firmware/README.md` for hardware, build instructions, the measurement checklist, and known residual risks. |
| `infra/` | Terraform for GCP (Cloud Run, Firestore, Firebase Hosting/Auth, Scheduler, Tasks, Secret Manager, WIF) plus the deployment runbook in `infra/README.md`. |
| `tools/` | `pager_client.py` (simulated device + server driver), `e2e_v2.py` (end-to-end suite against the real docker-compose stack), `send.py` (send a message from the CLI), `emqx_setup.py` (provisions the broker's rule engine for local dev), `mocks/twilio_mock.py`. |
| `.github/workflows/` | `ci.yml` runs the relay unit tests and the end-to-end suite on push; `deploy.yml` is the deploy pipeline, inert until a human wires up Workload Identity Federation. |
