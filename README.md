# Low-powered Kid Pager

Using expensive smart watches with also expensive cellular plans just so kids
can message their parents is ridiculous.

This is an attempt to build a simple pager-style device that can be fitted
with an ultra-cheapo SIM card (eg the $4/month ones from US Mobile with
just the minmal data and messaging). It should allow sending very simple
messages and location to a custom endpoint and hopefully run for weeks on
one charge.

## Status

**v1.1.0 (8 Oct 2026)** is the current firmware, released with the matching relay and web app. It
adds the Soracom bearer. **v1.0.0** (8 Oct 2026) was the first stable release. It brought the
right-header rewire, per-device GNSS disable, the host partial LUT, relay SMS through Twilio (removed 9 Oct 2026, last at 05ec3ed) with
unknown senders held for parent approval, and local-time battery charts. Over-the-air updates with
rollback were verified on a bench pager on 7 Oct 2026.

What works end to end today: a message typed in the web app reaches the pager over LTE-M and
shows on its screen, the pager acknowledges it, and replies typed on the pager reach the web app.
A page sent to a pager that is asleep is delivered: on a Soracom bench unit running the release
image, pages sent at +10 and +40 minutes into a one-hour watch were on the pager within a minute.

Soracom: a pager on a Soracom SIM detects the SIM and connects through Soracom Beam, which takes
plain MQTT from the pager and opens TLS to the broker. The SIM is authenticated with PAP. On the
bench (8 Oct 2026) the Beam session was usable 17 s after boot, against 32 s on the direct bearer,
and provisioning through Beam took 10 s against 26 s. Beam can read message bodies, so encrypting
bodies is the open precondition before real pages go over Beam (`docs/ROADMAP.md`).

**Bridge phones (9 Oct 2026): built, not yet run on a phone.** A member's SIM texts, Google Voice
texts and subscribed Google Chat conversations go through a headless Android phone
(`bridge-android/`). SMS is bridge-only: Twilio was removed on 9 Oct 2026 (last at 05ec3ed), so a
member without a bridge phone has no SMS. The relay has 1268 tests passing, and the `bridge` and
`bridge_voice` end-to-end scenarios pass against the simulator. The web pages and the Android app
have landed, but the app has never run on a phone, and until the owner registers its Firebase
Android app it polls for work every 60 s. See
`docs/OVERVIEW.md` ("Bridge phones"), `relay/README.md` ("Bridge phones") and the open items in
`docs/ROADMAP.md`.

Still open, from `docs/ROADMAP.md`: cell-tower location end to end, pushing a CA from the web app,
GNSS outdoors, and the no-coverage backoff in the field. Modem SMS is dead on the US Mobile line
(7 Oct 2026), so texting goes through the relay. See `docs/HARDWARE_TESTING.md` for what has been
seen on hardware.

**Start with [`docs/OVERVIEW.md`](docs/OVERVIEW.md)**, then
[`docs/GOTCHAS.md`](docs/GOTCHAS.md). [`docs/README.md`](docs/README.md) indexes the rest.

## Running the tests

The full end-to-end suite (13 scenarios covering text, location, address book, provisioning)
passes in both `--wire json` and `--wire cbor` encoding modes. From the repo root:

```bash
# Start the real local stack (EMQX, Firebase emulators, relay)
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
| `tools/` | `pager_client.py` (simulated device + server driver with setup-code bootstrap support), `e2e_v2.py` (13-scenario end-to-end suite against the real docker-compose stack), `send.py` (send a message from the CLI), `provision.py` (type a setup code over USB serial or fetch one from the API), `emqx_setup.py`. |
| `.github/workflows/` | `ci.yml` runs the relay unit tests and the 13-scenario end-to-end suite on push; `deploy.yml` is the deploy pipeline, a push to `main` builds the relay image, applies Terraform and deploys. |
