#!/usr/bin/env python3
"""Provisions EMQX's rule engine so device traffic on
`pager/{id}/{up,status,loc}` reaches the relay's `POST /webhooks/mqtt`
(docs/SERVER_PLAN.md §2 decision 1, §5.1). This is the "OR via
tools/emqx_setup.py" half of that plan's choice (§2's compose entry) --
picked over a purely declarative `emqx.conf` because EMQX 5's rule-engine
connectors/actions/rules are exercised far more reliably through its own
REST Management API than through hand-written HOCON, and the API is
idempotent-safe to re-run, which a `docker compose up` should be able to do
without caring whether this has already run once.

Uses only the standard library (`urllib`) so it never needs its own
dependency install step -- it's meant to be runnable with the interpreter
already on the machine (see `relay/README.md`).

**What this does NOT provision**: the relay's own REST-publish credential
(`BROKER_API_KEY`/`BROKER_API_SECRET`). That is seeded declaratively via
`relay/emqx/bootstrap_api_keys.txt` + `EMQX_API_KEY__BOOTSTRAP_FILE`
(`relay/docker-compose.yml`), specifically to avoid the chicken-and-egg of
EMQX generating a key the relay would need to be told about after the fact
-- see that file's comment.

Usage (after `docker compose up -d` in `relay/`):
    python3 tools/emqx_setup.py
    python3 tools/emqx_setup.py --emqx-url http://localhost:18083 \\
        --relay-url http://relay:8000 --webhook-key dev-webhook-key

Safe to re-run: every resource is created if missing, updated in place if
already present (never errors on "already exists").
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from typing import Any

CONNECTOR_NAME = "relay_webhook"
ACTION_NAME = "relay_webhook_action"
RULE_ID = "pager_to_relay"
TOPICS = ["pager/+/up", "pager/+/status", "pager/+/loc"]


def _request(
    method: str, url: str, token: str | None = None, body: dict[str, Any] | None = None
) -> tuple[int, Any]:
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            try:
                return resp.status, (json.loads(raw) if raw else None)
            except (json.JSONDecodeError, UnicodeDecodeError):
                # /api/v5/status replies with a plain-text body, not JSON.
                return resp.status, raw.decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            return exc.code, (json.loads(raw) if raw else None)
        except (json.JSONDecodeError, UnicodeDecodeError):
            return exc.code, raw
    except urllib.error.URLError as exc:
        return 0, str(exc)


def wait_for_emqx(base_url: str, timeout: float = 90.0) -> None:
    deadline = time.monotonic() + timeout
    last: Any = None
    while time.monotonic() < deadline:
        status, data = _request("GET", f"{base_url}/api/v5/status")
        if status == 200:
            return
        last = (status, data)
        time.sleep(2)
    raise SystemExit(f"EMQX at {base_url} never became ready (last: {last})")


def login(base_url: str, user: str, password: str) -> str:
    status, data = _request(
        "POST", f"{base_url}/api/v5/login", body={"username": user, "password": password}
    )
    if status != 200:
        raise SystemExit(f"EMQX login failed: {status} {data}")
    return data["token"]


def upsert_connector(base_url: str, token: str, relay_url: str) -> None:
    get_status, _ = _request(
        "GET", f"{base_url}/api/v5/connectors/http:{CONNECTOR_NAME}", token
    )
    if get_status == 200:
        # PUT rejects "type"/"name" -- they're fixed by the URL, not the body.
        status, data = _request(
            "PUT",
            f"{base_url}/api/v5/connectors/http:{CONNECTOR_NAME}",
            token,
            {"url": relay_url, "enable": True},
        )
    else:
        status, data = _request(
            "POST",
            f"{base_url}/api/v5/connectors",
            token,
            {"type": "http", "name": CONNECTOR_NAME, "url": relay_url, "enable": True},
        )
    if status not in (200, 201):
        raise SystemExit(f"failed to upsert EMQX connector: {status} {data}")


def upsert_action(base_url: str, token: str, webhook_key: str) -> None:
    parameters = {
        "path": "/webhooks/mqtt",
        "method": "post",
        "headers": {
            "X-Relay-Webhook-Key": webhook_key,
            "content-type": "application/json",
        },
        # "${.}" = the whole rule-engine event context, serialised as JSON --
        # gives app.broker.BrokerClient.parse_webhook a superset of the
        # {"topic", "payload", "qos"} shape it needs (see that module's
        # docstring). `query_mode: sync` so the HTTP POST actually happens
        # inline with the rule firing rather than sitting in an async queue
        # (observed empirically against a bare EMQX 5.8.0: async mode left
        # the very first request permanently "inflight" until something
        # else nudged the connector).
        "body": "${.}",
    }
    resource_opts = {"query_mode": "sync", "request_ttl": "10s"}
    get_status, _ = _request("GET", f"{base_url}/api/v5/actions/http:{ACTION_NAME}", token)
    if get_status == 200:
        status, data = _request(
            "PUT",
            f"{base_url}/api/v5/actions/http:{ACTION_NAME}",
            token,
            {
                "connector": CONNECTOR_NAME,
                "enable": True,
                "parameters": parameters,
                "resource_opts": resource_opts,
            },
        )
    else:
        status, data = _request(
            "POST",
            f"{base_url}/api/v5/actions",
            token,
            {
                "type": "http",
                "name": ACTION_NAME,
                "connector": CONNECTOR_NAME,
                "enable": True,
                "parameters": parameters,
                "resource_opts": resource_opts,
            },
        )
    if status not in (200, 201):
        raise SystemExit(f"failed to upsert EMQX action: {status} {data}")


def upsert_rule(base_url: str, token: str) -> None:
    topics_sql = ", ".join(f'"{t}"' for t in TOPICS)
    sql = f"SELECT topic, payload, qos, clientid FROM {topics_sql}"
    get_status, _ = _request("GET", f"{base_url}/api/v5/rules/{RULE_ID}", token)
    if get_status == 200:
        status, data = _request(
            "PUT",
            f"{base_url}/api/v5/rules/{RULE_ID}",
            token,
            {"sql": sql, "actions": [f"http:{ACTION_NAME}"], "enable": True},
        )
    else:
        status, data = _request(
            "POST",
            f"{base_url}/api/v5/rules",
            token,
            {"id": RULE_ID, "sql": sql, "actions": [f"http:{ACTION_NAME}"], "enable": True},
        )
    if status not in (200, 201):
        raise SystemExit(f"failed to upsert EMQX rule: {status} {data}")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--emqx-url",
        default="http://localhost:18083",
        help="EMQX dashboard/management API base URL, as reachable from wherever this script runs",
    )
    parser.add_argument(
        "--relay-url",
        default="http://relay:8000",
        help="relay base URL as reachable from *inside* the EMQX container (compose service name)",
    )
    parser.add_argument("--webhook-key", default="dev-webhook-key")
    parser.add_argument("--admin-user", default="admin")
    parser.add_argument("--admin-password", default="public")
    return parser


def main() -> None:
    args = build_arg_parser().parse_args()

    wait_for_emqx(args.emqx_url)
    token = login(args.emqx_url, args.admin_user, args.admin_password)
    upsert_connector(args.emqx_url, token, args.relay_url)
    upsert_action(args.emqx_url, token, args.webhook_key)
    upsert_rule(args.emqx_url, token)
    print(
        f"EMQX rule engine configured: {', '.join(TOPICS)} -> "
        f"{args.relay_url}/webhooks/mqtt"
    )


if __name__ == "__main__":
    try:
        main()
    except SystemExit as exc:
        print(f"emqx_setup.py failed: {exc}", file=sys.stderr)
        raise
