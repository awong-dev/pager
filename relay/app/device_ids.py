"""Relay-issued device ids (owner decision 9 Oct 2026): `pgr-` + 8 lowercase
hex. Matches the wire format in docs/PROTOCOL.md section 1
(`^[a-z0-9][a-z0-9-]{2,23}$`)."""

from __future__ import annotations

import secrets

DEVICE_ID_ATTEMPTS = 5


def new_device_id() -> str:
    return "pgr-" + secrets.token_hex(4)
