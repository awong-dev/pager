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

# §3.1 (v2): `from`/`to` carry an alias, not a fixed enum. `system` matches
# this regex on its own (six lowercase letters), so the "or the literal
# `system`" clause in §3.1's table is redundant with the regex, not an
# additional allowance -- it is written out here anyway to match the
# protocol's own wording. `system` is a *reserved* alias (§3.1): the relay
# MUST NOT ever issue it to a real user, but that is a store-layer rule for
# a later phase, not a wire-shape rule -- wire.py only checks the shape.
ALIAS_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,15}$")
SYSTEM_ALIAS = "system"
ACK_VALUES = {"shown", "read"}


def is_valid_alias(value: str) -> bool:
    """§3.1: `from`/`to` shape check -- the alias regex, or the literal
    `system` (which already matches the regex; kept explicit per the
    protocol table's own wording)."""
    return value == SYSTEM_ALIAS or bool(ALIAS_RE.match(value))


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
    to: str | None = None
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

    @field_validator("to")
    @classmethod
    def _check_to(cls, value: str | None) -> str | None:
        # §3.1: same alias shape as `from`. Whether an actual `to:"system"`
        # is routable is a store-layer allow-list decision (§4.2), not a
        # wire-shape one.
        if value is not None and not is_valid_alias(value):
            raise ValueError("invalid 'to' alias format")
        return value

    @model_validator(mode="after")
    def _check_shape(self) -> UpEnvelope:
        if self.ack is not None:
            if self.ack not in ACK_VALUES:
                raise ValueError("invalid ack value")
            if self.body:
                # §3.2: non-null ack + non-empty body is malformed.
                raise ValueError("ack payload must not carry a body")
            if self.to is not None:
                # §3.1: `to` is restricted to up content messages; an ack
                # carrying `to` is malformed.
                raise ValueError("ack payload must not carry a 'to'")
        else:
            if self.from_ is None or not is_valid_alias(self.from_):
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
        if self.state == "online" and (
            self.mode is None or self.batt_mv is None or self.ts is None
        ):
            raise ValueError("online status missing a required field")
        return self


class DownEnvelope(BaseModel):
    """A payload published on `pager/{device_id}/down` (relay -> device),
    per §3.1/§3.2. The relay is the only publisher of this shape, but it is
    modelled here (not just assembled ad hoc in `build_down_payload`) so the
    shape rules -- `loc_req` has no `body`, `ack` is always `null` -- are
    checked in one place and are testable independent of the transport."""

    model_config = ConfigDict(extra="ignore")

    v: int = 1
    id: str
    ts: int
    kind: Literal["msg", "loc_req"] = "msg"
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

    @field_validator("from_")
    @classmethod
    def _check_from(cls, value: str | None) -> str | None:
        if value is not None and not is_valid_alias(value):
            raise ValueError("invalid 'from' alias format")
        return value

    @model_validator(mode="after")
    def _check_shape(self) -> DownEnvelope:
        if self.ack is not None:
            # §3.2: every down envelope (msg or loc_req) has ack:null.
            raise ValueError("down envelope must have ack:null")
        # §3.1: `from` is required on every content message, and every down
        # envelope (msg or loc_req) is a content message -- acks are
        # up-only.
        if self.from_ is None or not is_valid_alias(self.from_):
            raise ValueError("down envelope requires a valid 'from'")
        if self.kind == "loc_req":
            if self.body:
                # §3.2: a loc_req has no body.
                raise ValueError("loc_req down envelope must not carry a body")
        else:
            if not self.body:
                raise ValueError("msg down envelope requires a non-empty body")
            validate_body(self.body)
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


def build_down_payload(
    *,
    msg_id: str,
    ts: int,
    body: str | None = None,
    v: int = 1,
    kind: Literal["msg", "loc_req"] = "msg",
    from_: str = "parent",
) -> bytes:
    """Minified UTF-8 JSON. Field order v,id,ts,kind,from,body,ack per §3.1
    (publishers SHOULD emit `kind` right after `ts`; `kind` is omitted
    entirely when it is the default `msg`, per §3.1's "SHOULD omit kind
    when it is msg"). §3.2: a `loc_req` down envelope has no `body`; a `msg`
    down envelope, conversely, always requires a real (non-empty) body --
    `DownEnvelope` rejects an empty body for `kind="msg"`, so building one
    here would silently produce a payload the device drops as malformed
    (§3.4)."""
    if kind != "loc_req" and not body:
        raise ValueError("msg down payload requires a non-empty body")
    obj: dict[str, Any] = {"v": v, "id": msg_id, "ts": ts}
    if kind != "msg":
        obj["kind"] = kind
    obj["from"] = from_
    if kind != "loc_req":
        obj["body"] = body
    obj["ack"] = None
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


class LocFix(BaseModel):
    """`loc` object inside a `/loc` envelope, per §13.2."""

    model_config = ConfigDict(extra="ignore")

    lat: float = Field(ge=-90, le=90)
    lon: float = Field(ge=-180, le=180)
    acc: int | None = Field(default=None, ge=0)
    fix_ts: int
    src: Literal["gnss", "cell"] = "gnss"

    @field_validator("fix_ts")
    @classmethod
    def _check_fix_ts(cls, value: int) -> int:
        # §13.2: `fix_ts` is the epoch time of the fix itself, not the
        # envelope's `ts` -- the §3.5 "ts=0 means no network time yet"
        # convention does not apply here: a real fix always has a real
        # timestamp, so 0 is rejected outright rather than substituted.
        if value == 0:
            raise ValueError("loc.fix_ts must not be 0")
        validate_ts(value)
        return value


class LocEnvelope(BaseModel):
    """A payload received on `pager/{device_id}/loc` (device -> relay), per
    §13.2. `loc` and `req` are required *keys* (the value may be null)."""

    model_config = ConfigDict(extra="ignore")

    v: int = 1
    id: str
    ts: int
    loc: LocFix | None
    req: str | None
    cached: bool = False
    err: Literal["no_fix", "disabled"] | None = None

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

    @field_validator("req")
    @classmethod
    def _check_req(cls, value: str | None) -> str | None:
        if value is not None and not ID_RE.match(value):
            raise ValueError("invalid 'req' id format")
        return value

    @model_validator(mode="after")
    def _check_shape(self) -> LocEnvelope:
        # §13.2: `loc` is null only when `err` is set, and vice versa.
        if self.loc is None and self.err is None:
            raise ValueError("loc:null requires err to be set")
        if self.loc is not None and self.err is not None:
            raise ValueError("loc and err are mutually exclusive")
        return self
