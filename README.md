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
the Python test client, device provisioning via setup codes, device authentication with per-device
HMAC signing, device address book and passcode lock, and the Terraform deployment are all
implemented, with unit tests and an end-to-end suite (11 scenarios) running against a real local
stack in CI.

**Device provisioning workflow:** a household admin uses the web app's *Add device* page to
create a new device (specifying the device ID, label, owner, and default recipient). The page
displays a one-time setup code (40–55 characters) as text and QR code, with a live countdown
expiring in 10 minutes. The device owner or admin types or scans this code into a physical pager
over LTE, which fetches an encrypted bootstrap bundle from the relay, decrypts it using a
key derived from the code, stores the device credentials and CA certificate, and publishes its
first signed status message to come online. The page then shows `online` with no further action
needed. The same flow works for credential rotation via *Rotate* and revocation via *Revoke*.

**Never deployed, never run on hardware.** Three things need a human before this is a real
system: a Walter board to flash (see `firmware/README.md`'s measurement checklist and residual
risks), a GCP project and EMQX Cloud account to deploy into (see `infra/README.md`'s runbook),
and Twilio / Google Workspace accounts if you want the SMS and Google Chat backends (see
`relay/README.md`). Nothing about those is code work.

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
| `docs/PROTOCOL.md` | Authoritative wire contract — topics, message schema, ack state machine, location, power/latency budget. Code conforms to this, not the reverse. |
| `docs/SERVER_PLAN.md` | Design reference for the server stack: user registry, allow-lists, location, multi-backend delivery, the web app, Firestore schema and security rules, cost analysis, and the Terraform layout. |
| `docs/DEVICE_PLAN.md` | Design plan for SIM-only device provisioning via a typed setup code, per-device HMAC authentication of every CBOR envelope, the server-approved on-device address book, the passcode lock, and the multi-screen e-paper UI. |
| `docs/DEVICE_TASKS.md` | The execution plan for `DEVICE_PLAN.md`: ordered, self-contained tasks per track (docs, server, tools, web, firmware) with files, steps and verification commands. |
| `relay/` | FastAPI relay: webhook ingest, routing, delivery backends, admin and conversation APIs, device provisioning, authentication, retention. See `relay/README.md` to run it locally. |
| `web/` | Next.js + MUI web app on Firebase (Auth, Firestore listeners, FCM). Device provisioning UI, contact approval flow, and lock controls. See `web/README.md`. |
| `firmware/` | ESP-IDF firmware for the Walter (ESP32-S3 + Sequans GM02SP) device. See `firmware/README.md` for hardware, build instructions, the measurement checklist, and known residual risks. |
| `infra/` | Terraform for GCP (Cloud Run, Firestore, Firebase Hosting/Auth, Scheduler, Tasks, Secret Manager, WIF) plus the deployment runbook in `infra/README.md`. |
| `tools/` | `pager_client.py` (simulated device + server driver with setup-code bootstrap support), `e2e_v2.py` (11-scenario end-to-end suite against the real docker-compose stack), `send.py` (send a message from the CLI), `provision.py` (type a setup code over USB serial or fetch one from the API), `emqx_setup.py`, `mocks/twilio_mock.py`. |
| `.github/workflows/` | `ci.yml` runs the relay unit tests and the 11-scenario end-to-end suite on push; `deploy.yml` is the deploy pipeline, inert until a human wires up Workload Identity Federation. |
