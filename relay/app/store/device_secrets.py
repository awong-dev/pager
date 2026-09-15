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
from datetime import datetime

from google.cloud.firestore import SERVER_TIMESTAMP, Transaction
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db, run_transaction

_WINDOW = 64
_WINDOW_MASK = (1 << _WINDOW) - 1


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
