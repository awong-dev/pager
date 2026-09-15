#!/usr/bin/env python3
"""
`tools/provision.py` -- docs/DEVICE_TASKS.md T3.7, docs/DEVICE_PLAN.md §3.7.

One code format, one decoder, three ways in (this is one of the three; the
other two are the `setup <code>` console command itself,
`firmware/main/setup.c`'s `setup_run()`, and `tools/pager_client.py
--bootstrap <code>`'s simulated-device fetch):

  --port /dev/tty... --code "<code>"
      Types `setup <code>` at the pager's USB-serial console (the real
      command `firmware/main/main.c` registers, backed by
      `firmware/main/setup.c`'s `setup_run()`) and streams the console log
      until one of the four docs/DEVICE_PLAN.md §3.2 step 5 error strings
      appears as a `SETUP error: <msg>` line, or the device logs `SETUP
      done` and reboots. The code contains a literal space (" @ ",
      docs/DEVICE_PLAN.md §3.1's `format_code()`) -- quote it in the shell.

  --from-api --relay <url> --admin-token <token> --device-id <id>
      --owner <alias> --label <text> [--default-to <alias>]
      Calls `POST /api/admin/devices` (docs/DEVICE_TASKS.md S2.2) first,
      with a Firebase ID token for an admin user as a bearer token, and
      extracts `setupCode` from the JSON response
      (`{device, setupCode, expiresAt, brokerPush, manualAcl}`). Combine
      with --port/--baud to feed that code straight into the serial flow
      above; without --port, this just fetches and prints the code, which
      is enough for a dry run with no hardware attached.

Exit status is non-zero on any error: an HTTP failure or malformed response
from --from-api, a serial timeout, or a `SETUP error: ...` line from the
device.

TODO(orchestrator): --port needs `pyserial`, and --from-api needs `httpx`;
neither is declared as a dependency anywhere in this repo (T3.7's `Files`
list is only this one new file, so nothing here adds them to
`relay/pyproject.toml`). Both are imported lazily so `--help` and each mode
that doesn't need the other's dependency both work without it; the actual
`pip install pyserial` (httpx already ships with the relay venv other tools
use) still needs a home, e.g. a `[dev]` extra or a `tools/`-local
requirements file, next time someone touches that file's scope.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from typing import Any

DEFAULT_API_URL = os.environ.get("PAGER_API_URL", "http://localhost:8000")
DEFAULT_BAUD = 115200
# Comfortably above the firmware's own SETUP_CONNECT_TIMEOUT_MS (30 s) +
# SETUP_BUNDLE_TIMEOUT_MS (15 s) plus modem-attach slack
# (firmware/main/setup.c), so a real device that is going to succeed or
# report one of the four error strings on its own is never cut off early.
DEFAULT_SERIAL_TIMEOUT_S = 90.0


def _httpx() -> Any:
    try:
        import httpx
    except ImportError as exc:  # pragma: no cover - environment problem, not logic
        raise SystemExit(
            "error: --from-api needs httpx -- run under the relay virtualenv "
            "(relay/.venv/bin/python tools/provision.py ...), same as "
            "tools/send.py and tools/pager_client.py"
        ) from exc
    return httpx


def _serial() -> Any:
    try:
        import serial
    except ImportError as exc:  # pragma: no cover - environment problem, not logic
        raise SystemExit(
            "error: --port needs pyserial (pip install pyserial); see the "
            "TODO(orchestrator) note in this file's module docstring"
        ) from exc
    return serial


def fetch_setup_code(
    relay: str,
    admin_token: str,
    device_id: str,
    owner: str,
    label: str,
    default_to: str | None,
) -> str:
    """`POST /api/admin/devices` (docs/DEVICE_TASKS.md S2.2) and return the
    `setupCode` from its response. Prints `expiresAt`/`brokerPush` (and, if
    the broker push failed, `manualAcl`) to stderr for visibility."""
    httpx = _httpx()
    endpoint = f"{relay.rstrip('/')}/api/admin/devices"
    body: dict[str, str] = {"deviceId": device_id, "ownerAlias": owner, "label": label}
    if default_to:
        body["defaultToAlias"] = default_to
    try:
        resp = httpx.post(
            endpoint,
            json=body,
            headers={"Authorization": f"Bearer {admin_token}"},
            timeout=10.0,
        )
    except httpx.HTTPError as exc:
        raise SystemExit(f"error: could not reach relay at {endpoint}: {exc}") from exc
    if resp.status_code >= 400:
        raise SystemExit(f"error: relay returned HTTP {resp.status_code}: {resp.text}")

    data = resp.json()
    try:
        setup_code: str = data["setupCode"]
    except KeyError as exc:
        raise SystemExit(f"error: relay response missing 'setupCode': {data!r}") from exc

    print(
        f"setup code issued for {device_id!r} "
        f"(brokerPush={data.get('brokerPush')!r}, expiresAt={data.get('expiresAt')!r})",
        file=sys.stderr,
    )
    manual_acl = data.get("manualAcl")
    if data.get("brokerPush") == "manual" and manual_acl:
        print("broker push did not happen; add these ACL rules by hand:", file=sys.stderr)
        for line in manual_acl:
            print(f"  {line}", file=sys.stderr)
    return setup_code


def run_serial_setup(port: str, baud: int, code: str, timeout: float) -> int:
    """Types `setup <code>` at the pager's USB console and streams its log
    to stdout until a `SETUP done`/`SETUP error: ...` line appears or
    `timeout` seconds pass with neither. Returns a process exit status."""
    serial = _serial()
    try:
        ser = serial.Serial(port, baudrate=baud, timeout=1.0)
    except serial.SerialException as exc:
        print(f"error: could not open {port}: {exc}", file=sys.stderr)
        return 1

    try:
        # Best-effort settle: opening some USB-serial adapters toggles
        # DTR/RTS, which resets boards wired that way -- give the console a
        # moment to come back up before writing to it.
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(f"setup {code}\r\n".encode())
        ser.flush()

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue  # readline's own 1 s timeout with no data yet
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if line:
                print(line)
            if "SETUP error" in line:
                return 1
            if "SETUP done" in line:
                return 0
        print(
            f"error: timed out after {timeout:.0f}s waiting for SETUP done|error",
            file=sys.stderr,
        )
        return 1
    finally:
        ser.close()


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--port", help="serial device of the pager's USB console, e.g. /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="serial baud rate (default: %(default)s)")
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_SERIAL_TIMEOUT_S,
        help="seconds to wait for SETUP done|error before giving up (default: %(default)s)",
    )
    parser.add_argument("--code", help="the setup code to type, exactly as issued (quote it: it contains spaces)")

    parser.add_argument(
        "--from-api", action="store_true", help="call POST /api/admin/devices first and use its setupCode"
    )
    parser.add_argument("--relay", default=DEFAULT_API_URL, help="relay base URL for --from-api (default: %(default)s)")
    parser.add_argument("--admin-token", help="Firebase ID token for an admin user (required with --from-api)")
    parser.add_argument(
        "--device-id",
        help="new device's id, e.g. its MAC (required with --from-api: "
        "POST /api/admin/devices' CreateDeviceRequest.deviceId is required)",
    )
    parser.add_argument("--owner", help="owner's alias (required with --from-api)")
    parser.add_argument("--default-to", help="default recipient's alias (optional, --from-api only)")
    parser.add_argument("--label", help="device label (required with --from-api)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)

    code: str
    if args.from_api:
        missing = [
            name
            for name, value in (
                ("--admin-token", args.admin_token),
                ("--device-id", args.device_id),
                ("--owner", args.owner),
                ("--label", args.label),
            )
            if not value
        ]
        if missing:
            print(f"error: --from-api requires {', '.join(missing)}", file=sys.stderr)
            return 2
        code = fetch_setup_code(
            args.relay, args.admin_token, args.device_id, args.owner, args.label, args.default_to
        )
        print(f"setup code: {code}")
    elif args.code:
        code = args.code
    else:
        print("error: need --code or --from-api", file=sys.stderr)
        return 2

    if not args.port:
        if not args.from_api:
            print("error: --code with no --port has nothing to do (pass --port too)", file=sys.stderr)
            return 2
        return 0  # --from-api-only dry run: code fetched and printed above

    return run_serial_setup(args.port, args.baud, code, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
