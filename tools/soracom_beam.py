#!/usr/bin/env python3
r"""Configure Soracom Beam for the pager through the Soracom API (docs/SORACOM_DESIGN.md §5).

    SORACOM_AUTH_KEY_ID=keyId-... SORACOM_AUTH_KEY=secret-... \\
        relay/.venv/bin/python tools/soracom_beam.py --imsi 295050... [--dry-run]

Steps (Global coverage, base https://g.api.soracom.io/v1):
  POST /auth                                       -> apiKey, token
  GET  /groups?tag_name=name&tag_value=<group>     -> reuse the group, else
  POST /groups {"tags": {"name": <group>}}
  PUT  /groups/{id}/configuration/SoracomBeam      (MQTT entry point, pass-through creds)
  POST /subscribers/{imsi}/set_group {"groupId"}
  GET  /groups/{id}                                -> prints the resulting Beam config

Idempotent: rerunning reuses the group, overwrites the same Beam entry, and re-sets the
group. The auth key id and secret come only from the environment, are never printed
(--dry-run shows <redacted>) and are never written to disk; the API key and token live in
memory only. Paths verified against the soracom-cli copy of the API spec (8 Oct 2026);
the field names inside the Beam entry `value` are not in the public spec and come from
docs/SORACOM_TASKS.md T1 (unverified until a live run).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from typing import Any

BASE_URL = "https://g.api.soracom.io/v1"
BEAM_KEY = "mqtt://beam.soracom.io:1883"
DEFAULT_DESTINATION = "mqtts://s1289801.ala.us-east-1.emqxsl.com:8883"
GROUP_ID_PLACEHOLDER = "{group_id}"
REDACTED = "<redacted>"


@dataclass(frozen=True)
class Call:
    method: str
    path: str
    json: Any = None
    params: dict[str, str] | None = None
    note: str = ""


def beam_entry(destination: str) -> list[dict[str, Any]]:
    return [
        {
            "key": BEAM_KEY,
            "value": {
                "name": "pager-emqx",
                "destination": destination,
                "enabled": True,
                "version": "201912",
                "useClientCredentials": True,
                "useClientCert": False,
                "addSubscriberHeader": False,
            },
        }
    ]


def build_auth_call(key_id: str, key: str) -> Call:
    return Call("POST", "/auth", {"authKeyId": key_id, "authKey": key}, note="authenticate")


def build_calls(group: str, imsi: str, destination: str, group_id: str | None = None) -> list[Call]:
    """The calls after /auth. With `group_id` unknown, the lookup/create calls come first and
    later paths carry the `{group_id}` placeholder the executor fills in."""
    gid = group_id or GROUP_ID_PLACEHOLDER
    calls: list[Call] = []
    if group_id is None:
        calls.append(
            Call("GET", "/groups", params={"tag_name": "name", "tag_value": group, "limit": "10"},
                 note="find group by name tag")
        )
        calls.append(Call("POST", "/groups", {"tags": {"name": group}},
                          note="create group only if the lookup found none"))
    calls.append(Call("PUT", f"/groups/{gid}/configuration/SoracomBeam", beam_entry(destination),
                      note="MQTT entry point, credential pass-through"))
    calls.append(Call("POST", f"/subscribers/{imsi}/set_group", {"groupId": gid},
                      note="move the SIM into the group"))
    calls.append(Call("GET", f"/groups/{gid}", note="print the resulting configuration"))
    return calls


def describe(call: Call) -> str:
    out = f"{call.method} {BASE_URL}{call.path}"
    if call.params:
        out += "?" + "&".join(f"{k}={v}" for k, v in call.params.items())
    if call.json is not None:
        out += " " + json.dumps(call.json)
    return out


def dry_run_lines(group: str, imsi: str, destination: str) -> list[str]:
    lines = [describe(build_auth_call(REDACTED, REDACTED))]
    lines += [describe(c) + (f"   # {c.note}" if c.note else "") for c in build_calls(group, imsi, destination)]
    lines.append("(later calls send X-Soracom-API-Key: <redacted>, X-Soracom-Token: <redacted>)")
    return lines


def run(group: str, imsi: str, destination: str) -> int:
    import httpx

    key_id = os.environ.get("SORACOM_AUTH_KEY_ID", "")
    key = os.environ.get("SORACOM_AUTH_KEY", "")
    if not key_id or not key:
        print("SORACOM_AUTH_KEY_ID and SORACOM_AUTH_KEY must be set in the environment", file=sys.stderr)
        return 2
    with httpx.Client(base_url=BASE_URL, timeout=30) as http:
        auth = build_auth_call(key_id, key)
        resp = http.post(auth.path, json=auth.json)
        if resp.status_code != 200:
            print(f"auth failed: HTTP {resp.status_code}", file=sys.stderr)
            return 1
        body = resp.json()
        headers = {"X-Soracom-API-Key": body["apiKey"], "X-Soracom-Token": body["token"]}

        def send(call: Call) -> Any:
            r = http.request(call.method, call.path, json=call.json, params=call.params, headers=headers)
            if r.status_code >= 300:
                raise SystemExit(f"{call.method} {call.path}: HTTP {r.status_code} {r.text[:300]}")
            return r.json() if r.content else None

        lookup, create, *_ = build_calls(group, imsi, destination)
        found = send(lookup) or []
        exact = [g for g in found if (g.get("tags") or {}).get("name") == group]
        if exact:
            group_id = exact[0]["groupId"]
            print(f"reusing group {group} ({group_id})")
        else:
            group_id = send(create)["groupId"]
            print(f"created group {group} ({group_id})")
        final = None
        for call in build_calls(group, imsi, destination, group_id):
            final = send(call)
        beam = ((final or {}).get("configuration") or {}).get("SoracomBeam")
        print(json.dumps(beam, indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--imsi", required=True)
    ap.add_argument("--group", default="pager-beam")
    ap.add_argument("--destination", default=DEFAULT_DESTINATION)
    ap.add_argument("--dry-run", action="store_true", help="print the requests, send nothing")
    args = ap.parse_args(argv)
    if args.dry_run:
        print("\n".join(dry_run_lines(args.group, args.imsi, args.destination)))
        return 0
    return run(args.group, args.imsi, args.destination)


if __name__ == "__main__":
    sys.exit(main())
