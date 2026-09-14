#!/usr/bin/env python3
"""
CLI to send a message via the v2 relay API -- docs/SERVER_PLAN.md §5.1.

`POST /api/conversations/{alias}/messages` requires a Firebase ID token.
Signing in follows the same dev-mode custom-token pattern
`tools/pager_client.py`'s `ServerClient.login` uses: `POST
/api/dev/token` (DEV_MODE=1 only) mints a Firebase custom token for a given
alias, which is exchanged for a real ID token at the Auth emulator's
Identity Toolkit REST endpoint -- no real email/phone sign-in flow needed
for a CLI tool driving a local/dev deployment.

Usage:
    send.py --as parent --to student --body "Pickup at 3:15 by the gym"
"""

from __future__ import annotations

import argparse
import json
import os

import httpx

DEFAULT_API_URL = os.environ.get("PAGER_API_URL", "http://localhost:8000")
DEFAULT_AUTH_URL = os.environ.get("PAGER_AUTH_URL", "http://localhost:9099")
# The Auth emulator does not validate this key -- any non-empty string works
# (same convention as tools/pager_client.py / relay/tests/firebase_test_utils.py).
FAKE_API_KEY = "fake-api-key"


def login(api_url: str, auth_url: str, alias: str) -> str:
    """Mints a dev-mode custom token for `alias` and exchanges it for a
    Firebase ID token, exactly like `tools/pager_client.py`'s
    `ServerClient.login`."""
    resp = httpx.post(f"{api_url}/api/dev/token", json={"alias": alias}, timeout=10.0)
    if resp.status_code == 404:
        raise SystemExit(
            f"error: unknown alias {alias!r}, or DEV_MODE is off on the relay at {api_url}"
        )
    resp.raise_for_status()
    custom_token = resp.json()["token"]

    exchange = httpx.post(
        f"{auth_url}/identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken",
        params={"key": FAKE_API_KEY},
        json={"token": custom_token, "returnSecureToken": True},
        timeout=10.0,
    )
    exchange.raise_for_status()
    return exchange.json()["idToken"]


def send_message(api_url: str, id_token: str, to_alias: str, body: str) -> dict:
    endpoint = f"{api_url.rstrip('/')}/api/conversations/{to_alias}/messages"
    try:
        resp = httpx.post(
            endpoint,
            json={"body": body},
            headers={"Authorization": f"Bearer {id_token}"},
            timeout=10.0,
        )
    except httpx.HTTPError as exc:
        raise SystemExit(f"error: could not reach relay at {endpoint}: {exc}") from exc
    if resp.status_code >= 400:
        raise SystemExit(f"error: relay returned HTTP {resp.status_code}: {resp.text}")
    return resp.json()


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--as", dest="from_alias", required=True, help="sender's alias (dev-mode sign-in)")
    parser.add_argument("--to", required=True, help="recipient's alias")
    parser.add_argument("--body", required=True, help="message body")
    parser.add_argument("--url", default=DEFAULT_API_URL, help="relay base URL")
    parser.add_argument("--auth-url", default=DEFAULT_AUTH_URL, help="Auth emulator base URL")
    return parser


def main() -> None:
    args = build_arg_parser().parse_args()

    id_token = login(args.url, args.auth_url, args.from_alias)
    result = send_message(args.url, id_token, args.to, args.body)
    print(f"sent: {json.dumps(result)}")


if __name__ == "__main__":
    main()
