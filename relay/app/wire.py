"""Wire-format validation for the MQTT envelope, per docs/PROTOCOL.md §3.

This module is the single source of truth for what makes a payload
"malformed" per §3.4. It is deliberately independent of the HTTP API layer:
the relay's HTTP API (app/main.py) applies the parent-facing body rules
(strip-then-validate) before a message is ever created; this module applies
the wire rules (validate, never strip) to payloads arriving from the device
on `/up` and `/status`.
"""

from __future__ import annotations

import json
import logging
import re
import time
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

logger = logging.getLogger("relay.wire")

MAX_ENVELOPE_BYTES = 640
BODY_MAX_CODEPOINTS = 160
BODY_MAX_UTF8_BYTES = 320

# §1: id is opaque; validators accept this generic shape for all id kinds.
ID_RE = re.compile(r"^[a-z0-9_]{3,16}$")
DEVICE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{2,23}$")
SESSION_RE = re.compile(r"^s_[0-9a-f]{8}$")
# U+0000-001F and U+007F.
CONTROL_CHAR_RE = re.compile(r"[\x00-\x1f\x7f]")

FROM_VALUES = {"parent", "student", "system"}
ACK_VALUES = {"shown", "read"}


def strip_control_chars(text: str) -> str:
    """Used by the HTTP ingest path only (§3.1: relay strips on ingest from
    the parent API)."""
    return CONTROL_CHAR_RE.sub("", text)


def validate_body(body: str) -> None:
    """Raise ValueError if `body` violates §3.1. Never strips -- used for
    wire-received (device-originated) bodies, which are validated, not
    sanitised (§3.4: a body-rule violation makes the whole payload
    malformed)."""
    if not body:
        raise ValueError("body must not be empty")
    if CONTROL_CHAR_RE.search(body):
        raise ValueError("body contains control characters")
    if len(body) > BODY_MAX_CODEPOINTS:
        raise ValueError("body exceeds 160 Unicode code points")
    if len(body.encode("utf-8")) > BODY_MAX_UTF8_BYTES:
        raise ValueError("body exceeds 320 UTF-8 bytes")


def validate_ts(ts: int) -> None:
    if ts == 0:
        return
    if not (1_000_000_000 <= ts <= 2_000_000_000):
        raise ValueError("ts out of range")


def resolve_ts(ts: int) -> int:
    """§3.5: ts=0 means "no network time yet"; relay substitutes its own
    receive time. Applies to message ts, ack ts and status ts alike."""
    return ts if ts != 0 else int(time.time())


class UpEnvelope(BaseModel):
    """A payload received on `pager/{device_id}/up`: either an ack (§3.2,
    `ack` non-null, no `body`) or a student up-message (`from`/`body`
    present, `ack: null`)."""

    model_config = ConfigDict(extra="ignore")

    v: int = 1
    id: str
    ts: int
    from_: str | None = Field(default=None, alias="from")
    body: str | None = None
    ack: str | None = None

    @field_validator("id")
    @classmethod
    def _check_id(cls, value: str) -> str:
        if not ID_RE.match(value):
            raise ValueError("invalid id format")
        return value

    @field_validator("ts")
    @classmethod
    def _check_ts(cls, value: int) -> int:
        validate_ts(value)
        return value

    @model_validator(mode="after")
    def _check_shape(self) -> UpEnvelope:
        if self.ack is not None:
            if self.ack not in ACK_VALUES:
                raise ValueError("invalid ack value")
            if self.body:
                # §3.2: non-null ack + non-empty body is malformed.
                raise ValueError("ack payload must not carry a body")
        else:
            if self.from_ not in FROM_VALUES:
                raise ValueError("content message requires a valid 'from'")
            if not self.body:
                raise ValueError("content message requires a non-empty body")
            validate_body(self.body)
        return self

    @property
    def is_ack(self) -> bool:
        return self.ack is not None


class StatusEnvelope(BaseModel):
    """A payload received on `pager/{device_id}/status` (device publish or
    broker-generated LWT), per §5.1/§5.2."""

    model_config = ConfigDict(extra="ignore")

    v: int = 1
    state: Literal["online", "offline"]
    mode: str | None = None
    batt_mv: int | None = None
    rssi: int | None = None
    session: str
    ts: int | None = None
    fw: str | None = None

    @field_validator("session")
    @classmethod
    def _check_session(cls, value: str) -> str:
        if not SESSION_RE.match(value):
            raise ValueError("invalid session format")
        return value

    @field_validator("ts")
    @classmethod
    def _check_ts(cls, value: int | None) -> int | None:
        if value is not None:
            validate_ts(value)
        return value

    @field_validator("batt_mv")
    @classmethod
    def _check_batt(cls, value: int | None) -> int | None:
        if value is not None and not (2000 <= value <= 4500):
            raise ValueError("batt_mv out of range")
        return value

    @field_validator("rssi")
    @classmethod
    def _check_rssi(cls, value: int | None) -> int | None:
        if value is not None and not (-140 <= value <= 0):
            raise ValueError("rssi out of range")
        return value

    @model_validator(mode="after")
    def _check_online_fields(self) -> StatusEnvelope:
        if self.state == "online":
            if self.mode is None or self.batt_mv is None or self.ts is None:
                raise ValueError("online status missing a required field")
        return self


def parse_envelope_bytes(raw: bytes) -> dict[str, Any] | None:
    """Size/UTF-8/JSON-object check per §3.3/§3.4. Returns the parsed JSON
    object, or None if the payload is malformed at this coarse level."""
    if len(raw) > MAX_ENVELOPE_BYTES:
        return None
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return None
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return None
    if not isinstance(data, dict):
        return None
    return data


def log_malformed(topic: str, raw: bytes, reason: str) -> None:
    """§3.4: relay logs the topic and first 64 bytes, then drops."""
    logger.warning("malformed payload on %s (%s): %r", topic, reason, raw[:64])


def build_down_payload(*, msg_id: str, ts: int, body: str, v: int = 1) -> bytes:
    """Minified UTF-8 JSON, field order v,id,ts,from,body,ack per §3.1."""
    obj = {"v": v, "id": msg_id, "ts": ts, "from": "parent", "body": body, "ack": None}
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
