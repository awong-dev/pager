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
# The defaults (BROKER_API_KEY/SECRET, WEBHOOK_KEY) already match relay/emqx/
# below, so this runs as-is for local dev.
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

Then, from `relay/`, in a virtualenv (the tools below and `pytest` both
need the dependencies, and a Homebrew/system Python will refuse to install
into itself):
```bash
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
.venv/bin/pytest
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

### End-to-end test scenarios

End-to-end integration tests (brings up the real Docker Compose stack —
EMQX, the Firestore/Auth emulators, and the relay — configures EMQX's rule engine, and drives it end to end through
`tools/pager_client.py`'s combined device+server client):

```bash
relay/.venv/bin/python tools/e2e_v2.py                                       # all 12 scenarios
relay/.venv/bin/python tools/e2e_v2.py bootstrap text_roundtrip              # named scenarios
relay/.venv/bin/python tools/e2e_v2.py --wire cbor                           # repeat in CBOR encoding
```

Scenarios available (see `tools/e2e_v2.py`'s module docstring for details):
- **bootstrap**: device provisioning via setup code (docs/DEVICE_PLAN.md §3.2)
- **text_roundtrip**: text messages and acks round-trip end to end
- **allowlist**: approving contacts and routing per allow-list
- **republish**: messages re-deliver on reconnect
- **location_periodic**: periodic location publishing with TTL sync
- **location_on_demand**: on-demand location requests with coalescing
- **retention**: per-device retention policy with sweep
- **bytes**: data budget accounting and SIM constraints
- **setup_code**: real admin-create → code → bootstrap → provisioned flow
- **address_book**: device requests contact approval, book/cfg ingest and ack
- **relay_sms**: a member's relay SMS number: inbound from an approved contact, a held
  unknown number approved, and a pager text out through the Twilio mock (docs/RELAY_SMS_DESIGN.md)
- **bridge**: the bridge phone (docs/BRIDGE_PHONE_DESIGN.md) against the `bridge-sim` compose service
  (`tools/bridge_sim.py`, control plane on `localhost:8020`: `/_pair`, `/_inject`, `/_outbox`,
  `/_fail_next`, `/_reset`, `/_status`): pair, a SIM text held and approved, a pager reply through the
  outbox with an ack, an unknown Google Chat group held, subscribed and replied to, and a Voice-only variant

## Message backends (docs/SERVER_PLAN.md §6.5)

`gchat` (`app/backends/gchat.py`)
is a real adapter against an outside service — everything in this repo
(tests, `tools/e2e_v2.py`, this compose stack) exercises it against a
locally-signed test JWT, never a
real Google account. One account-level chore blocks a real
deployment from using it:

- **Google Chat** — docs/SERVER_PLAN.md §10 D4's prerequisite, still
  **unverified**: Google Chat apps can only be installed by accounts
  on **Google Workspace**, not consumer Gmail. If the deployment's Google
  accounts are consumer Gmail, this backend is dead on arrival and Email
  (§6.6, not built) is the documented fallback slot. A human needs to (1)
  confirm the account type, (2) if Workspace, create a Chat app in
  the Google Cloud console (Chat API → Configuration), note its **project
  number** as `GCHAT_AUDIENCE`, and point its webhook URL at
  `{PUBLIC_BASE_URL}/webhooks/gchat`, and (3) grant the relay's own service
  account (already provisioned by `infra/`, ADC — no separate secret) the
  `chat.bot` scope / "Chat Bot" role so `spaces.messages.create` outbound
  sends work. None of this has been done either.

`sms` (`app/backends/sms_twilio.py`, docs/RELAY_SMS_DESIGN.md) is the Twilio adapter. Its backend row
lives on an SMS contact (an external), and every person who should text through the relay holds their
own number (`users.smsNumber`, set by a family admin under People or by the super under Admin → Users).
A member without a number keeps the modem path (`cfg.sms`).

### Twilio

One-time console steps, done by hand (the relay never buys numbers or edits Twilio configuration):

1. Create a **Messaging Service** named `pager` and attach the 10DLC campaign (standard brand, EIN) to it.
2. Buy one number per member and add **each number to the service's sender pool**. Then set the member's
   number in the web app (People → member → SMS number). Outbound passes `From=<member number>`
   explicitly, so no `MessagingServiceSid` is needed.
3. Set the service's **inbound request URL** to `PUBLIC_BASE_URL/webhooks/twilio/sms` (HTTP POST). One
   place, not per number.
4. Secrets: `TWILIO_ACCOUNT_SID` and `TWILIO_AUTH_TOKEN` (the auth token also verifies
   `X-Twilio-Signature`; blank means the webhook answers 401). Plain env: `TWILIO_BASE_URL`
   (`https://api.twilio.com`; the compose stack points it at `tools/mocks/twilio_mock.py`) and
   `PUBLIC_BASE_URL`. There is no `TWILIO_FROM_NUMBER`.
5. `PUBLIC_BASE_URL` must be the relay's origin **as Twilio calls it** (the Cloud Run `run.app` origin) and
   match the URL in the console **byte for byte**: Twilio signs the exact URL, so a trailing-slash or host
   difference fails every request with 401.
6. Turn **Advanced Opt-Out off**: the relay answers STOP/START/HELP itself (keywords, replies and the
   once-a-day disclosure live in `app/sms_compliance.py`; consent rows in `smsConsent/{e164}`). The
   operator name and support email in those texts come from `SMS_OPERATOR_NAME` (default `Albert Wong`)
   and `SMS_SUPPORT_EMAIL` (default `awong.dev@gmail.com`).

Outbound format: `<Name> says: "<text>" - Pager (<operator>)`, with URLs and phone numbers in the text
defanged by spaces, and `. Reply STOP to opt out, HELP for help.` appended to the first relayed message
to a number each UTC day. Opt-in rule: nothing is sent to a number that has not opted in, either by
texting START (or IN/OPTIN) or by an admin adding the contact / approving its held text or contact
request (the relay then sends the welcome from the member's number). A refused send is `failed` with
`code=not_opted_in` or `code=opted_out` in the `sms out` log line and is never retried; STOP
(UNSUBSCRIBE/END/QUIT) opts out, HELP (INFO/SUPPORT) answers with the help text. Keyword texts are never
stored or routed and log `outcome=keyword_start|keyword_stop|keyword_help` on the `sms in` line.

Inbound texts from a number the family has no approved contact for are stored in `heldSms` and raised as
an `sms_unknown` alert to the family admins; they are delivered only when an admin approves. The relay
stores message bodies in Firestore, and Twilio keeps its own copy in its message logs. Log lines to watch:
`sms in to=@alias from=...1234 sid=SM... outcome=...` and `sms out to=...1234 from=...5678 sid=... status=...`.

## CLI Tools

For sending messages via the relay API and driving a simulated device +
server session. Both import `httpx`/`paho-mqtt`, so run them with the
virtualenv's interpreter (`ModuleNotFoundError: No module named 'httpx'`
means a bare `python3` was used instead):
```bash
relay/.venv/bin/python tools/send.py --help          # from the repo root
relay/.venv/bin/python tools/pager_client.py --help
```
`tools/emqx_setup.py` is the exception -- it is stdlib-only, so a plain
`python3` runs it fine.

## Wire Protocol

See `docs/PROTOCOL.md` for the authoritative MQTT message schema, device-to-relay contract, and full system semantics.
