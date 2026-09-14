# School Pager Relay

FastAPI relay for the school pager system. Request-driven end to end
(docs/SERVER_PLAN.md §2 decision 1): the relay never holds an MQTT
connection. Device traffic (`/up`, `/status`, `/loc`) arrives over HTTPS on
`POST /webhooks/mqtt`, pushed by the broker's rule engine; outbound `/down`
publishes go through the broker's REST publish API (`app/broker.py`).

## Requirements

- Python 3.12+
- Docker (with the Compose plugin) for local development

## Quickstart

```bash
cp .env.example .env
# Edit .env and set RELAY_TOKEN to a secure value. The other defaults
# (BROKER_API_KEY/SECRET, WEBHOOK_KEY) already match relay/emqx/ below.
docker compose up -d --build
python3 ../tools/emqx_setup.py   # one-off: provisions EMQX's rule engine
```

The relay will be available at `http://localhost:8000/` once the stack
starts (`emqx` + `relay` containers). `tools/emqx_setup.py` is idempotent --
safe to re-run any time (e.g. after `docker compose restart emqx`, which
loses the rule-engine config a plain container `stop`/`start` would keep).

### Why a setup script instead of a declarative `emqx.conf`

docs/SERVER_PLAN.md §2 left this as an open choice ("via a mounted
`relay/emqx/emqx.conf` ... OR via `tools/emqx_setup.py`"). This repo uses
the script: EMQX 5's rule-engine connectors/actions/rules are far more
reliably exercised through its own REST Management API (which is
idempotent-safe, so `docker compose up` can call it every time without
caring whether it already ran) than through hand-written HOCON rule-engine
config, which is comparatively undocumented and fiddlier to get right for a
one-off local dev setup. `tools/emqx_setup.py` uses only the standard
library and is safe to re-run.

What it provisions, every time it runs:

- an HTTP **connector** (`relay_webhook`) pointing at the relay
  (`http://relay:8000` from inside the EMQX container's network — pass
  `--relay-url` to override);
- an HTTP **action** (`relay_webhook_action`) that `POST`s to
  `/webhooks/mqtt` with header `X-Relay-Webhook-Key` (must match the
  relay's `WEBHOOK_KEY`) and body `${.}` (the whole rule-engine event
  context, serialised as JSON — see `app/broker.py`'s module docstring for
  the exact shape `BrokerClient.parse_webhook` expects out of that);
- a **rule** (`pager_to_relay`) selecting `topic, payload, qos, clientid`
  from `pager/+/up`, `pager/+/status`, `pager/+/loc` and firing that action.

### The relay's own REST-publish credential

`relay/emqx/bootstrap_api_keys.txt` (mounted into the EMQX container via
`EMQX_API_KEY__BOOTSTRAP_FILE` in `relay/docker-compose.yml`) pre-seeds a
**known** `api_key:api_secret` pair, matching `BROKER_API_KEY`/
`BROKER_API_SECRET` in `.env.example`. This is deliberate: EMQX's
management API always *generates* a fresh key/secret when asked to create
one dynamically, which would leave the relay's `.env` needing to be told
about a value it can't know ahead of time. The bootstrap file sidesteps that
chicken-and-egg entirely for local dev. **Not a real secret** — never used
outside `docker-compose.yml`'s local stack. Production (EMQX Cloud
Serverless) provisions its own key out of band (docs/SERVER_PLAN.md §9.2).

## Firestore + Auth (docs/SERVER_PLAN.md §2 decision 3, §5.9)

Firestore is the only store and Firebase Auth is the identity provider —
both run as local emulators (`relay/emulator.Dockerfile`: `node:20-bookworm-
slim` + Eclipse Temurin 21 JRE + `firebase-tools`, since the Emulator Suite
needs a JRE ≥ 21 and Debian bookworm's own JRE packages only go up to 17).
`app/db/firestore.py` is emulator-aware via `FIRESTORE_EMULATOR_HOST` /
`FIREBASE_AUTH_EMULATOR_HOST` — set in `.env.example` (pointed at the
`firebase` compose service) and hard-set as `environment:` overrides on the
`relay` service in `docker-compose.yml` so the stack is correct regardless
of what a developer's `.env` has.

`docker compose up -d --build` now also brings up `firebase` (Firestore on
`:8080`, Auth on `:9099`, Emulator Suite UI on `:4000`); the `relay`
container waits for both `emqx` and `firebase` to report healthy before
starting.

The first admin (`python -m app.bootstrap --admin-email you@example.com`,
run inside the `relay` container or against the emulators from the host)
creates a Firebase Auth user + `users/{uid}` doc with `role: 'admin'` + the
`admin` custom claim — idempotent, safe to re-run.

## Tests

Unit tests need the Firestore + Auth emulators running (the broker is
still a fake `BrokerClient`, `tests/fake_transport.py` — no EMQX needed for
`pytest`). Two ways to get the emulators up:

```bash
# (a) just the emulators, via Docker Compose (fastest for pytest-only work)
docker compose up -d firebase

# (b) bare firebase-tools, if you have Node 20+ and a JRE >= 21 locally
firebase emulators:start --only firestore,auth --project demo-pager
```

Then, from `relay/`:
```bash
pip install -e ".[dev]"
pytest
```

`tests/conftest.py` wipes both emulators before every test (Firestore via
`DELETE /emulator/v1/projects/demo-pager/databases/(default)/documents`,
Auth via `DELETE /emulator/v1/projects/demo-pager/accounts` — note the path
is `/emulator/v1/...`, not `/emulation/v1/...`) and fails the whole session
up front with an actionable message if it can't reach `localhost:8080` /
`localhost:9099`, rather than letting every test drown in gRPC
connection-refused tracebacks.

`tests/test_rules.py` is the one place `relay/firestore.rules` itself is
exercised: it talks to the Firestore emulator's REST API directly with real
Firebase ID tokens (minted via a custom token + the Auth emulator's
Identity Toolkit REST endpoint, `tests/firebase_test_utils.py` — the same
two-step sign-in a real client does), so it proves the rules file enforces
access on its own, independent of anything the relay's Python code does
(the relay itself always uses admin credentials, which bypass rules
entirely).

End-to-end integration tests (brings up the real Docker Compose stack —
EMQX, the Firestore/Auth emulators, and the relay — configures EMQX's rule
engine, and drives it with real and simulated MQTT devices against the
legacy `RELAY_TOKEN` endpoints, now Firestore-backed — see
`tools/e2e_test.py`'s module docstring for the scenarios):
```bash
python tools/e2e_test.py
```

## CLI Tools

For sending messages and simulating devices:
```bash
python tools/send.py --help
python tools/sim_device.py --help
```

## Wire Protocol

See `docs/PROTOCOL.md` for the authoritative MQTT message schema, device-to-relay contract, and full system semantics.
