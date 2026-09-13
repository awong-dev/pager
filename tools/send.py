#!/usr/bin/env python3
"""
CLI to send a message via the relay API — see HANDOFF.md Phase 2.

Usage:
    send.py --device-id pgr-0001 --body "Pickup at 3:15 by the gym"
"""

from __future__ import annotations

import argparse
import json
import os
import urllib.error
import urllib.request


def send_message(url: str, token: str, device_id: str, body: str) -> dict:
    endpoint = f"{url.rstrip('/')}/api/devices/{device_id}/messages"
    payload = json.dumps({"body": body}).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=payload,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {token}",
        },
    )
    try:
        with urllib.request.urlopen(request) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise SystemExit(f"error: relay returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"error: could not reach relay at {endpoint}: {exc.reason}") from exc


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send a message via the relay")
    parser.add_argument("--device-id", required=True, help="Target device ID")
    parser.add_argument("--body", required=True, help="Message body")
    parser.add_argument("--url", default="http://localhost:8000", help="Relay base URL")
    parser.add_argument(
        "--token",
        default=os.environ.get("RELAY_TOKEN"),
        help="Bearer token (defaults to $RELAY_TOKEN)",
    )
    return parser


def main() -> None:
    args = build_arg_parser().parse_args()

    if not args.token:
        raise SystemExit("error: no bearer token given (use --token or set RELAY_TOKEN)")

    result = send_message(args.url, args.token, args.device_id, args.body)
    print(f"sent: id={result.get('id')} state={result.get('state')}")


if __name__ == "__main__":
    main()
