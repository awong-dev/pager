"""`deviceSecrets/{deviceId}` -- docs/DEVICE_PLAN.md §2.6, §2.5.

Server-only: this collection has no `firestore.rules` `match` block, so it
is default-deny for every client read and write -- `relay/tests/test_rules.py`
pins that, the same pattern it already uses for `phoneIndex`. The MQTT
password hash belongs here rather than on `devices/{d}` because the owner's
own browser can read that document (`docs/DEVICE_PLAN.md` §2.6): the two
secrets that let someone impersonate a device -- the HMAC key that
authenticates its signed MQTT payloads, and the password that authenticates
its MQTT session -- live together in the one collection nothing but
firebase-admin (this relay) can ever read.

`hmacKey` is stored base64-encoded, matching the wire/document shape
`DEVICE_PLAN.md` §2.6 gives (`{hmacKey: base64, mqttPasswordHash, upN, upBits,
downN, sigFailures, createdAt}`); this module's own `create`/`rotate`/`get`
take and return raw bytes so callers never see the encoding.

**Replay window** (§2.5): `upN` is the highest `n` accepted from the device
so far, `upBits` a 64-bit bitmap of the 64 values immediately below `upN`
(bit `g - 1` set means `upN - g` has already been accepted). An inbound `n`
is accepted if `n > upN` (it becomes the new top; the old `upN` slides into
bit position `n - upN - 1` of the new bitmap so a later replay of it is still
caught) or `0 < upN - n <= 64` and that bit is clear (a gap left by
at-least-once/reordered webhook delivery, filled in); `n == upN` or a gap bit
that is already set is a replay and is dropped without changing state. (§2.5
writes the second half of this as "upN - 64 < n <= upN"; read literally that
is only 63 usable positions, one short of the "64-bit bitmap"/"64 wide, the
IPsec/DTLS convention" it says twice in the same section, so this
implementation uses the full 64-wide window -- `0 < upN - n <= 64` -- to
match those two explicit numbers rather than the inequality's literal
boundary.)

`accept_up_n`, `next_down_n` and `bump_sig_failures` are all Firestore
transactions on this one document -- the same "transactions for anything
monotonic or unique" habit `app/store/rate_limits.py` and
`app/store/messages.py` already document, and (per §2.5) the same document
`ingest.py`'s per-envelope verification touches anyway, so this adds no
extra round trip.
"""

from __future__ import annotations

import base64
import re
from datetime import datetime
from typing import Any

from google.cloud.firestore import SERVER_TIMESTAMP, Transaction
from pydantic import BaseModel, ConfigDict, field_validator

from app.db.firestore import get_db, run_transaction

_WINDOW = 64
_WINDOW_MASK = (1 << _WINDOW) - 1

# docs/WIFI_DESIGN.md §4, docs/WIFI_TASKS.md W7: `deviceSecrets/{d}.wifiEnabled`
# / `.wifiNets`, next to `hmacKey`/`mqttPasswordHash` -- a WPA2 PSK is a
# credential exactly like those two, and this collection is the one place
# nothing but firebase-admin (this relay) can ever read (this module's own
# docstring). `devices/{d}` (`app/store/devices.py`), by contrast, is
# readable by the device's own owner from the web app, so a raw PSK must
# never land there -- `GET /api/devices/{id}/wifi` (`app/routers/devices.py`)
# reports only `{s, set: true}` per network, never `p`.
_WIFI_CONTROL_CHAR_RE = re.compile(r"[\x00-\x1f\x7f]")
WIFI_SSID_MIN_BYTES = 1
WIFI_SSID_MAX_BYTES = 32
WIFI_PSK_MIN_BYTES = 8
WIFI_PSK_MAX_BYTES = 63
# docs/WIFI_DESIGN.md §4: "at most 2" (`WIFICRED_MAX_NETS`,
# `firmware/main/wificred.h`) -- kept in lockstep with that firmware constant
# by inspection, not by import (no shared header between the two languages).
WIFI_MAX_NETS = 2


class WifiNet(BaseModel):
    """One `cfg.wifi.nets[]` entry (docs/PROTOCOL.md §10) -- `s` (SSID) and
    `p` (WPA2-PSK passphrase), matching `firmware/main/wificred.c`'s
    `wificred_valid_ssid`/`wificred_valid_psk` byte-length bounds and their
    "no embedded NUL" rule (a NUL would silently truncate through this
    relay's own JSON/CBOR round-trip the same way it would through
    `nvs_set_str()`/`nvs_get_str()` on the device)."""

    model_config = ConfigDict(extra="ignore")

    s: str
    p: str

    @field_validator("s")
    @classmethod
    def _check_ssid(cls, value: str) -> str:
        if _WIFI_CONTROL_CHAR_RE.search(value):
            raise ValueError("wifi ssid contains control characters")
        length = len(value.encode("utf-8"))
        if not (WIFI_SSID_MIN_BYTES <= length <= WIFI_SSID_MAX_BYTES):
            raise ValueError(
                f"wifi ssid must be {WIFI_SSID_MIN_BYTES}-{WIFI_SSID_MAX_BYTES} UTF-8 bytes, "
                f"got {length}"
            )
        return value

    @field_validator("p")
    @classmethod
    def _check_psk(cls, value: str) -> str:
        if _WIFI_CONTROL_CHAR_RE.search(value):
            # Never interpolate `value` itself into this message -- this
            # file's own hard "never a PSK near a log" rule, applied to
            # exception text too (pydantic's `ValidationError` renders a
            # `ValueError`'s message verbatim, and that message can end up in
            # a log or an HTTP 422 body).
            raise ValueError("wifi psk contains control characters")
        length = len(value.encode("utf-8"))
        if not (WIFI_PSK_MIN_BYTES <= length <= WIFI_PSK_MAX_BYTES):
            raise ValueError(
                f"wifi psk must be {WIFI_PSK_MIN_BYTES}-{WIFI_PSK_MAX_BYTES} UTF-8 bytes, "
                "got a different length"
            )
        return value


def _to_signed64(bits: int) -> int:
    """Firestore integers are signed int64 (protobuf), so a 64-bit bitmap
    with the top bit set (>= 2**63) does not fit as a positive value -- store
    its two's-complement equivalent instead; `_from_signed64` undoes this on
    read. The bit pattern round-trips exactly either way."""
    bits &= _WINDOW_MASK
    return bits - (1 << 64) if bits >= (1 << 63) else bits


def _from_signed64(stored: int) -> int:
    return (stored + (1 << 64)) & _WINDOW_MASK if stored < 0 else stored & _WINDOW_MASK


class DeviceSecret(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    hmacKey: bytes
    mqttPasswordHash: str
    upN: int = 0
    upBits: int = 0
    downN: int = 0
    sigFailures: int = 0
    createdAt: datetime | None = None
    rotatedAt: datetime | None = None


def _secrets():
    return get_db().collection("deviceSecrets")


def _decode(device_id: str, data: dict) -> DeviceSecret:
    fields = dict(data)
    hmac_b64 = fields.pop("hmacKey", "")
    fields["hmacKey"] = base64.b64decode(hmac_b64) if hmac_b64 else b""
    if "upBits" in fields:
        fields["upBits"] = _from_signed64(fields["upBits"])
    return DeviceSecret.model_validate({"id": device_id, **fields})


def create(device_id: str, *, hmac_key: bytes, mqtt_password_hash: str) -> DeviceSecret:
    ref = _secrets().document(device_id)
    ref.set(
        {
            "hmacKey": base64.b64encode(hmac_key).decode("ascii"),
            "mqttPasswordHash": mqtt_password_hash,
            "upN": 0,
            "upBits": 0,
            "downN": 0,
            "sigFailures": 0,
            "createdAt": SERVER_TIMESTAMP,
        }
    )
    fetched = get(device_id)
    assert fetched is not None
    return fetched


def get(device_id: str) -> DeviceSecret | None:
    snap = _secrets().document(device_id).get()
    if not snap.exists:
        return None
    return _decode(device_id, snap.to_dict() or {})


def rotate(device_id: str, hmac_key: bytes, mqtt_password_hash: str) -> DeviceSecret:
    """Replaces both secrets and zeroes every counter -- a rotated device has
    a fresh replay window and a fresh down-counter, same as a newly created
    one (`DEVICE_PLAN.md` §3.5: rotation is "the same as" issuing a new
    device's credentials, just against an existing document)."""
    ref = _secrets().document(device_id)
    if not ref.get().exists:
        raise KeyError(f"no such device secret: {device_id!r}")
    ref.update(
        {
            "hmacKey": base64.b64encode(hmac_key).decode("ascii"),
            "mqttPasswordHash": mqtt_password_hash,
            "upN": 0,
            "upBits": 0,
            "downN": 0,
            "sigFailures": 0,
            "rotatedAt": SERVER_TIMESTAMP,
        }
    )
    fetched = get(device_id)
    assert fetched is not None
    return fetched


def delete(device_id: str) -> None:
    _secrets().document(device_id).delete()


def accept_up_n(device_id: str, n: int) -> bool:
    """§2.5's replay window. Returns True and advances/fills the window if
    `n` is new; returns False, with no write at all, if `n` is a replay."""
    ref = _secrets().document(device_id)

    def _txn(transaction: Transaction) -> bool:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            raise KeyError(f"no such device secret: {device_id!r}")
        data = snap.to_dict() or {}
        up_n = data.get("upN", 0)
        up_bits = _from_signed64(data.get("upBits", 0))

        if n > up_n:
            shift = n - up_n
            new_bits = ((up_bits << shift) | (1 << (shift - 1))) & _WINDOW_MASK
            transaction.update(ref, {"upN": n, "upBits": _to_signed64(new_bits)})
            return True

        gap = up_n - n
        if 0 < gap <= _WINDOW:
            bit = 1 << (gap - 1)
            if up_bits & bit:
                return False
            transaction.update(ref, {"upBits": _to_signed64(up_bits | bit)})
            return True

        return False

    return run_transaction(_txn)


def accept_request_n(device_id: str, n: int) -> bool:
    """docs/PROTOCOL.md §14.7 step 4: the book-pull HTTPS request's own
    counter check. Strictly `n > upN` only -- unlike `accept_up_n`, there is
    no 64-wide fallback window here ("the device draws `n` just before it
    dials, so a genuine request is always the newest value, and the window
    exists only for the broker's reordering of MQTT pushes"). On accept,
    advances `upN`/shifts `upBits` with exactly `accept_up_n`'s `n > up_n`
    arithmetic -- "this consumes the counter for `/up` too" (§14.7): a
    `/up`/`/status`/`/loc` envelope drawn with a smaller `n` earlier but
    ingested later is still accepted through the ordinary 64-wide window.
    Returns `False` with **no write at all** on `n <= upN` (the caller
    answers `409`, "nothing written")."""
    ref = _secrets().document(device_id)

    def _txn(transaction: Transaction) -> bool:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            raise KeyError(f"no such device secret: {device_id!r}")
        data = snap.to_dict() or {}
        up_n = data.get("upN", 0)
        up_bits = _from_signed64(data.get("upBits", 0))

        if n > up_n:
            shift = n - up_n
            new_bits = ((up_bits << shift) | (1 << (shift - 1))) & _WINDOW_MASK
            transaction.update(ref, {"upN": n, "upBits": _to_signed64(new_bits)})
            return True

        return False

    return run_transaction(_txn)


def next_down_n(device_id: str) -> int:
    """Issues the next `n` for a `/down` envelope to this device -- the
    relay's own mirror of §2.5's counter, one higher each call."""
    ref = _secrets().document(device_id)

    def _txn(transaction: Transaction) -> int:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            raise KeyError(f"no such device secret: {device_id!r}")
        data = snap.to_dict() or {}
        down_n = data.get("downN", 0) + 1
        transaction.update(ref, {"downN": down_n})
        return down_n

    return run_transaction(_txn)


def get_wifi(device_id: str) -> tuple[bool, list[WifiNet]]:
    """docs/WIFI_TASKS.md W7: `GET /api/devices/{id}/wifi`'s reader.
    `(False, [])` for a device with no `deviceSecrets/{d}` document at all
    (e.g. a `password`-mode test device, or one created before this field
    existed) -- same "no such secret is just an empty answer" style
    `app/devcfg.py`'s pending-slot readers use, rather than raising."""
    snap = _secrets().document(device_id).get()
    if not snap.exists:
        return False, []
    data = snap.to_dict() or {}
    en = bool(data.get("wifiEnabled", False))
    raw_nets = data.get("wifiNets", [])
    nets = [WifiNet.model_validate(n) for n in raw_nets] if isinstance(raw_nets, list) else []
    return en, nets[:WIFI_MAX_NETS]


def set_wifi(device_id: str, *, en: bool, nets: list[WifiNet] | None) -> None:
    """docs/WIFI_TASKS.md W7: `PUT /api/devices/{id}/wifi`'s writer. `nets is
    None` means "leave the stored networks alone, apply `en` only" (mirrors
    `cfg.wifi.nets` absent on the wire, docs/WIFI_DESIGN.md §4) -- a merge
    write that touches only `wifiEnabled` in that case; `nets=[]` clears the
    stored set (still a merge write, but with an explicit empty list so the
    old entries do not linger). Caller (`app/routers/devices.py`) has already
    applied the `tls == "pinned"` guard before calling this with a non-`None`
    `nets` -- this function does not re-check it."""
    fields: dict[str, Any] = {"wifiEnabled": en}
    if nets is not None:
        fields["wifiNets"] = [n.model_dump() for n in nets]
    _secrets().document(device_id).set(fields, merge=True)


def bump_sig_failures(device_id: str) -> int:
    """Increments and returns the failure counter §2.6 uses to raise the
    admin-facing `authAlarm` after 20 failures in 10 minutes."""
    ref = _secrets().document(device_id)

    def _txn(transaction: Transaction) -> int:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            raise KeyError(f"no such device secret: {device_id!r}")
        data = snap.to_dict() or {}
        count = data.get("sigFailures", 0) + 1
        transaction.update(ref, {"sigFailures": count})
        return count

    return run_transaction(_txn)
