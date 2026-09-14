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

## Tests

Unit tests (no Docker required — the broker is a fake `BrokerClient`,
`tests/fake_transport.py`):
```bash
pip install -e ".[dev]"
pytest
```

End-to-end integration tests (brings up the real Docker Compose stack,
configures EMQX's rule engine, and drives it with real and simulated MQTT
devices — see `tools/e2e_test.py`'s module docstring for the scenarios):
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
