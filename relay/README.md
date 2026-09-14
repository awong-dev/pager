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
EMQX, the Firestore/Auth emulators, the relay, and a Twilio Messages API
mock — configures EMQX's rule engine, and drives it end to end through
`tools/pager_client.py`'s combined device+server client — see
`tools/e2e_v2.py`'s module docstring for the scenarios, `docs/SERVER_PLAN.md`
§8):
```bash
python tools/e2e_v2.py
```

## Message backends (docs/SERVER_PLAN.md §6.4/§6.5)

`sms` (`app/backends/sms_twilio.py`) and `gchat` (`app/backends/gchat.py`)
are both real adapters against outside services — everything in this repo
(tests, `tools/e2e_v2.py`, this compose stack) exercises them against a
mock (`tools/mocks/twilio_mock.py`) or a locally-signed test JWT, never a
real Twilio/Google account. Two `PENDING_ACCOUNT` items block a real
deployment from using them:

- **`PENDING_ACCOUNT: Twilio`** — a human needs to buy a phone number and
  complete US A2P 10DLC (or toll-free) registration before real SMS can be
  sent from this deployment. This is a manual, **days-long** review process
  run by Twilio/the carriers, separate from any code here — it cannot be
  scripted or done from this repo. Steps: sign up at twilio.com, buy a
  number, register an A2P 10DLC brand + campaign (or apply for toll-free
  verification, faster but still manual), then set `TWILIO_ACCOUNT_SID`,
  `TWILIO_AUTH_TOKEN`, `TWILIO_FROM_NUMBER`, and `PUBLIC_BASE_URL` (so
  `X-Twilio-Signature` verification matches the exact webhook URL configured
  in the Twilio console) in the real deployment's environment/Secret
  Manager, and point the Twilio console's inbound-SMS webhook at
  `{PUBLIC_BASE_URL}/webhooks/twilio/sms`. Not started here per this
  project's "no signups, no paid services" rule — see `BUILD_LOG.md`'s
  Phase 7 entry.
- **`PENDING_ACCOUNT: Google Chat`** — docs/SERVER_PLAN.md §11 D4's caveat,
  still **not verified**: Google Chat apps can only be installed by accounts
  on **Google Workspace**, not consumer Gmail. If the family's Google
  accounts are consumer Gmail, this backend is dead on arrival and Email
  (§6.6, not built) is the documented fallback slot. A human needs to (1)
  confirm the family's account type, (2) if Workspace, create a Chat app in
  the Google Cloud console (Chat API → Configuration), note its **project
  number** as `GCHAT_AUDIENCE`, and point its webhook URL at
  `{PUBLIC_BASE_URL}/webhooks/gchat`, and (3) grant the relay's own service
  account (already provisioned by `infra/`, ADC — no separate secret) the
  `chat.bot` scope / "Chat Bot" role so `spaces.messages.create` outbound
  sends work. Not started here for the same reason as the Twilio item
  above.

## CLI Tools

For sending messages via the relay API and driving a simulated device +
server session:
```bash
python tools/send.py --help
python tools/pager_client.py --help
```

## Wire Protocol

See `docs/PROTOCOL.md` for the authoritative MQTT message schema, device-to-relay contract, and full system semantics.
