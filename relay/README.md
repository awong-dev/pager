# School Pager Relay

FastAPI relay and MQTT gateway for the school pager system.

## Requirements

- Python 3.12+
- Docker (with the Compose plugin) for local development

## Quickstart

```bash
cp .env.example .env
# Edit .env and set RELAY_TOKEN to a secure value
docker compose up
```

The relay will be available at `http://localhost:8000/` once the stack starts (broker + relay container).

## Tests

Unit tests:
```bash
pip install -e ".[dev]"
pytest
```

End-to-end integration tests (requires the real Docker Compose stack running):
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
