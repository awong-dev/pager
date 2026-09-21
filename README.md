# Low-powered Kid Pager

Using expensive smart watches with also expensive cellular plans just so kids
can message their parents is ridiculous.

This is an attempt to build a simple pager-style device that can be fitted
with an ultra-cheapo SIM card (eg the $4/month ones from US Mobile with
just the minmal data and messaging). It should allow sending very simple
messages and location to a custom endpoint and hopefully run for weeks on
one charge.

## Status

**v0.1: a message typed in the web app reaches the pager's screen**, over LTE-M, MQTT on TLS with
a pinned CA, signed in both directions, and acknowledged back. The relay is deployed (GCP Cloud
Run, Firestore, Firebase Hosting); the broker is EMQX Cloud Serverless; pushing to `main`
deploys.

**v0.2**, on `main` and running on the bench pager: the pager picks its carrier APN from the SIM;
falls back gracefully if it cannot verify the broker's certificate, and can be given a new CA from
the web app; answers location requests with short GNSS attempts and a backoff; and can text a
parent-managed list of phone numbers directly, with every text logged for the parent.

Not yet proven: a page arriving while the pager is asleep, which the whole power design rests on;
replies, location fixes and SMS on real hardware. See `docs/HARDWARE_TESTING.md`.

**Start with [`docs/OVERVIEW.md`](docs/OVERVIEW.md)**, then
[`docs/GOTCHAS.md`](docs/GOTCHAS.md). [`docs/README.md`](docs/README.md) indexes the rest.

## Running the tests

The full end-to-end suite (11 scenarios covering text, location, address book, provisioning)
passes in both `--wire json` and `--wire cbor` encoding modes. From the repo root:

```bash
# Start the real local stack (EMQX, Firebase emulators, relay, Twilio mock)
cd relay
docker compose up -d --build
cd ..
python3 tools/emqx_setup.py

# Run all scenarios (both text and CBOR)
relay/.venv/bin/python tools/e2e_v2.py
relay/.venv/bin/python tools/e2e_v2.py --wire cbor

# Or run individual scenarios
relay/.venv/bin/python tools/e2e_v2.py bootstrap setup_code address_book
```

Available scenarios: `bootstrap`, `text_roundtrip`, `allowlist`, `republish`, `location_periodic`,
`location_on_demand`, `fanout`, `retention`, `bytes`, `setup_code`, `address_book`. See
`relay/README.md` for details and `tools/e2e_v2.py` for the implementation.

Unit tests and the manual checklist (for web app and device provisioning flow) are documented in
`relay/README.md`, `web/README.md`, and `firmware/README.md`.

## Repo map

| Path | What it is |
|---|---|
| `docs/` | Start at `docs/README.md`. `OVERVIEW.md` explains the system, `GOTCHAS.md` lists what bites, `PROTOCOL.md` is the authoritative wire contract that code conforms to. |
| `relay/` | FastAPI relay: webhook ingest, routing, delivery backends, admin and conversation APIs, device provisioning, authentication, retention. See `relay/README.md` to run it locally. |
| `web/` | Next.js + MUI web app on Firebase (Auth, Firestore listeners, FCM). Device provisioning UI, contact approval flow, and lock controls. See `web/README.md`. |
| `firmware/` | ESP-IDF firmware for the Walter (ESP32-S3 + Sequans GM02SP) device. See `firmware/README.md` for hardware, build instructions, the measurement checklist, and known residual risks. |
| `infra/` | Terraform for GCP (Cloud Run, Firestore, Firebase Hosting/Auth, Scheduler, Tasks, Secret Manager, WIF) plus the deployment runbook in `infra/README.md`. |
| `tools/` | `pager_client.py` (simulated device + server driver with setup-code bootstrap support), `e2e_v2.py` (11-scenario end-to-end suite against the real docker-compose stack), `send.py` (send a message from the CLI), `provision.py` (type a setup code over USB serial or fetch one from the API), `emqx_setup.py`, `mocks/twilio_mock.py`. |
| `.github/workflows/` | `ci.yml` runs the relay unit tests and the 11-scenario end-to-end suite on push; `deploy.yml` is the deploy pipeline, a push to `main` builds the relay image, applies Terraform and deploys. |
