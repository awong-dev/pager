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

from app import wirecbor

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


#: docs/V02_DESIGN.md §3 / docs/PROTOCOL.md §3.1, §14.2 (v0.2): `n` widens
#: from a 32-bit counter to a 52-bit one (`epoch` grows from 12 to 32 bits;
#: `lo` stays 20 bits) so a 12-bit epoch's ~4096-cold-boot lifetime cannot be
#: exhausted. `2**53` (not `2**52`) is the exact bound the design gives --
#: "n becomes a 52-bit unsigned integer (< 2^53, exact in JSON and in a
#: Firestore int64)" -- one bit of headroom above the tightest packing, kept
#: because that is the number every other document (PROTOCOL.md §3.1's `n`
#: row, §7's keymap table) states directly.
N_MAX_EXCLUSIVE = 2**53


def _check_n_range(value: int | None) -> int | None:
    """§14.2: `n` is a replay counter, on every signed envelope (`/up`,
    `/status`, `/loc`). Shared by the three envelope models below that carry
    it."""
    if value is not None and not (0 <= value < N_MAX_EXCLUSIVE):
        raise ValueError("n out of range")
    return value


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
    # §3.2: the only up `kind` this relay implements today is the implicit
    # default (an ack or a plain content message, `kind` omitted). `kind:
    # "contact_req"` is on the wire (§3.2) but its handling is task S4.1's,
    # not this one's -- until then it (and any other/future kind) takes the
    # unknown-kind path below, same as an unrecognised `/down` kind (§3.4).
    kind: str | None = None
    # §14.2: present on every signed (`authMode: "hmac"`) envelope; absent
    # on an unsigned (`authMode: "password"`) one.
    n: int | None = None

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

    @field_validator("n")
    @classmethod
    def _check_n(cls, value: int | None) -> int | None:
        return _check_n_range(value)

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
        if self.kind is not None:
            # §3.4: "A kind the receiver does not recognise is handled
            # exactly like loc_req is handled by text-only firmware: do not
            # render, do not ack, count it, drop it." `log_malformed`
            # (called by the ingest.py caller that catches this
            # ValueError) is that log/count/drop for the relay side.
            raise ValueError(f"unknown kind on /up: {self.kind!r}")
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
    # §4.5/§5.1 (v2): display/diagnosis only -- the relay stores whatever the
    # device reports and never writes them back (the device owns its own
    # location duty cycle).
    loc_period_s: int | None = None
    loc_min_s: int | None = None
    # docs/V02_DESIGN.md §4.3/§7 (CA trust, optional, absent = older
    # firmware): trust state and the first 16 hex chars of the pinned CA's
    # SHA-256, per `CA_TRUST_PLAN.md` §3.3.
    tls: Literal["unpinned", "pinned", "broken"] | None = None
    ca_fp: str | None = None
    # docs/V02_DESIGN.md §5 (location, optional): seconds until the device's
    # own backoff next allows an attempt, 0 = now. Generous upper bound
    # (matches loc_period_s/loc_min_s's own 86400 rather than the design's
    # 12 h/43200s ceiling) so a future retune of the backoff schedule is not
    # a relay-side rejection.
    loc_backoff_s: int | None = None
    # docs/V02_DESIGN.md §6/§7 (device SMS, optional): count of sms_log
    # audit entries dropped for lack of NVS queue space, normally 0. Modelled
    # here (not left to `extra="ignore"`) because the ground rule (§0) is
    # explicit that a relay must accept this *before* any firmware sends it.
    sms_lost: int | None = None
    # §14.2: present on every signed envelope; absent on the unsigned LWT
    # exception (§14.6) and on an unsigned (`authMode: "password"`) device.
    n: int | None = None

    @field_validator("loc_period_s", "loc_min_s", "loc_backoff_s")
    @classmethod
    def _check_loc_timing(cls, value: int | None) -> int | None:
        if value is not None and not (0 <= value <= 86400):
            raise ValueError("loc_period_s/loc_min_s/loc_backoff_s out of range")
        return value

    @field_validator("ca_fp")
    @classmethod
    def _check_ca_fp(cls, value: str | None) -> str | None:
        if value is not None and not re.fullmatch(r"[0-9a-f]{16}", value):
            raise ValueError("ca_fp must be 16 lowercase hex chars")
        return value

    @field_validator("sms_lost")
    @classmethod
    def _check_sms_lost(cls, value: int | None) -> int | None:
        if value is not None and value < 0:
            raise ValueError("sms_lost must be >= 0")
        return value

    @field_validator("n")
    @classmethod
    def _check_n(cls, value: int | None) -> int | None:
        return _check_n_range(value)

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


EnvelopeEncoding = Literal["json", "cbor"]


def is_oversize(raw: bytes) -> bool:
    """§3.3/§14.4 step 1: the 640-byte check, done *before* anything about
    the payload (encoding, signature, shape) is inspected."""
    return len(raw) > MAX_ENVELOPE_BYTES


def decode_envelope_bytes(raw: bytes) -> tuple[dict[str, Any], EnvelopeEncoding] | None:
    """§14.4 step 4 ("parse the message, now safe"): the post-verification
    (or, for a `password`-mode device, only) decode -- CBOR via
    `wirecbor.decode` when the first byte is in CBOR's range (§3's
    first-byte dispatch, `wirecbor.is_cbor`), else JSON. Returns
    `(dict, encoding)`, or None if the payload is malformed at this coarse
    level (oversize, non-UTF-8/non-JSON, non-CBOR-map, or not an object) --
    the same class of check `parse_envelope_bytes` does for the JSON-only
    caller, extended to also accept CBOR."""
    if is_oversize(raw):
        return None
    if wirecbor.is_cbor(raw):
        try:
            data = wirecbor.decode(raw)
        except Exception:  # noqa: BLE001 -- cbor2 raises several distinct
            # exception types on malformed input (truncated maps, bad
            # additional-info bytes, etc.); all of them mean "malformed"
            # here, same as JSONDecodeError below. An *unknown* integer key
            # is not one of these any more -- wirecbor.translate_to_names
            # drops it rather than raising, per §3.1's "unknown fields MUST
            # be ignored" (a newer device's new field must not make this
            # decode -- and therefore the whole envelope -- fail).
            return None
        if not isinstance(data, dict):
            return None
        return data, "cbor"
    try:
        text = raw.decode("utf-8")
        data = json.loads(text)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    return data, "json"


# §14.6: the one unsigned inbound envelope the relay accepts from an
# `authMode: "hmac"` device -- the broker-generated LWT, which cannot itself
# carry a signature (it is registered at CONNECT). DEVICE_TASKS.md's S1.3
# wording ("exactly {v, state:offline, session}") is used verbatim here; it
# matches §5.2's own LWT wire example (`{"v":1,"state":"offline","session":
# "s_3ab91c02"}`), but is stricter than §14.6's own parenthetical ("no live
# fields (no mode, batt_mv, rssi, session, etc.)"), which lists `session` as
# a field that must be *absent* -- contradicting §5.2's example, where it is
# present. Flagged per this task's brief rather than silently resolved;
# implemented to match the actual wire shape in §5.2 and DEVICE_TASKS.md.
_LWT_ALLOWED_KEYS = {"v", "state", "session"}


def is_unsigned_lwt_shape(data: dict[str, Any]) -> bool:
    if not set(data.keys()) <= _LWT_ALLOWED_KEYS:
        return False
    return data.get("state") == "offline" and isinstance(data.get("session"), str)


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
    # §14.2: present on every signed envelope.
    n: int | None = None

    @field_validator("n")
    @classmethod
    def _check_n(cls, value: int | None) -> int | None:
        return _check_n_range(value)

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


# docs/V02_DESIGN.md §6/§7: `peer` is always a real phone number (never an
# alias reference the way `contact_req`'s overloaded `ph` can be, §4.2) --
# same E.164 shape as `app/ingest.py`'s private `_PHONE_E164_RE` and
# `app/backends/sms_twilio.py`'s `_E164_RE`, duplicated rather than imported
# across modules for the same reason those two don't share one either (a
# private regex is not a public contract worth coupling two unrelated
# modules to).
_SMS_PEER_RE = re.compile(r"^\+[1-9]\d{6,14}$")
SMS_LOG_DIR_VALUES = ("out", "in")
SMS_LOG_ST_VALUES = ("sent", "failed", "recv", "blocked")
# §6: "body" (the SMS text itself, not the wire envelope's usual body rules)
# is "<=160 chars but allow empty" -- deliberately *not* `validate_body`
# above: an SMS log entry's body must be allowed empty (some real handsets
# and gateways deliver a body-less/heartbeat SMS) and is not itself the
# thing a §3.4 "malformed" rejection should key on the way a `msg` kind's
# `body` is.
SMS_LOG_BODY_MAX_CODEPOINTS = 160


class SmsLogEnvelope(BaseModel):
    """A payload received on `pager/{device_id}/up` with `kind:"sms_log"`
    (docs/V02_DESIGN.md §6/§7): the pager's own modem sending/receiving SMS
    directly, audited back to the relay. Not a thread entry, not routed to
    any user -- `app/ingest.py`'s `Ingest._handle_sms_log` only ever writes
    it to `devices/{deviceId}/smsLog/{logId}` (`app/store/sms.py`).

    Modelled here (unlike `contact_req`, which lives in `app/ingest.py`)
    per this task's own file list. Dispatched by `app/ingest.py`'s
    `handle_up` on the raw decoded dict's `kind`, *before*
    `UpEnvelope.model_validate` -- exactly the same "why not `app/wire.py`'s
    `UpEnvelope`" reasoning `ContactReqEnvelope`'s own docstring gives (that
    model treats any non-null `kind` as unrecognised, §3.4)."""

    model_config = ConfigDict(extra="ignore")

    v: int = 1
    id: str
    ts: int
    peer: str
    dir: Literal["out", "in"]
    st: Literal["sent", "failed", "recv", "blocked"]
    body: str = ""
    sms_ts: int
    # §14.2: present on every signed envelope (this kind is only ever
    # signed -- an SMS audit log from an unsigned v1 device is not a real
    # deployment shape, since device-side SMS is a v0.2 feature -- but `n`
    # is still optional here, not required, for the same uniform reason
    # every other `/up` envelope model in this file leaves it optional: an
    # `authMode: "password"` device is never rejected by *shape* for
    # lacking `n`, only by `app/ingest.py`'s `_verify_and_decode`, which
    # never signs/verifies at all for such a device.
    n: int | None = None

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

    @field_validator("peer")
    @classmethod
    def _check_peer(cls, value: str) -> str:
        if not _SMS_PEER_RE.match(value):
            raise ValueError("sms_log peer is not a valid E.164 phone number")
        return value

    @field_validator("body")
    @classmethod
    def _check_body(cls, value: str) -> str:
        if len(value) > SMS_LOG_BODY_MAX_CODEPOINTS:
            raise ValueError("sms_log body exceeds 160 Unicode code points")
        return value

    @field_validator("sms_ts")
    @classmethod
    def _check_sms_ts(cls, value: int) -> int:
        if value < 0:
            raise ValueError("sms_ts must be >= 0")
        return value

    @field_validator("n")
    @classmethod
    def _check_n(cls, value: int | None) -> int | None:
        return _check_n_range(value)
