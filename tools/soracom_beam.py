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
docs/SORACOM_TASKS.md T1; a live run on 8 Oct 2026 created `pager-beam`, had the entry
accepted as written and the pager then connected through Beam (docs/SORACOM_DESIGN.md §6 E).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "relay"))

from app.soracom import (
    BASE_URL,
    BEAM_KEY,
    DEFAULT_DESTINATION,
    GROUP_ID_PLACEHOLDER,
    Call,
    SoracomClient,
    SoracomError,
    beam_entry,
    build_auth_call,
    build_calls,
    enrol,
    ensure_group_created,
)

__all__ = ["BASE_URL", "BEAM_KEY", "GROUP_ID_PLACEHOLDER", "beam_entry"]
REDACTED = "<redacted>"


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
    key_id = os.environ.get("SORACOM_AUTH_KEY_ID", "")
    key = os.environ.get("SORACOM_AUTH_KEY", "")
    if not key_id or not key:
        print("SORACOM_AUTH_KEY_ID and SORACOM_AUTH_KEY must be set in the environment", file=sys.stderr)
        return 2
    try:
        with SoracomClient(key_id, key) as client:
            group_id, created = ensure_group_created(client, group, destination)
            print(f"{'created' if created else 'reusing'} group {group} ({group_id})")
            enrol(client, imsi, group_id)
            final: Any = client.send(Call("GET", f"/groups/{group_id}"))
    except SoracomError as e:
        print(f"soracom: {e.detail}", file=sys.stderr)
        return 1
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
