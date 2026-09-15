"""`setupCodes/{bid}` -- docs/DEVICE_PLAN.md §3.2 steps 3, 5.

Server-only, same idiom as `app/store/device_secrets.py`: this collection has
no `firestore.rules` `match` block, so it is default-deny for every client
read and write. Holds only what §3.2 says it may hold -- `deviceId` and
`expiresAt` -- **never the token, `bpw` or `bkey`** (`app/devsetup.py`'s
`issue()` derives those and discards them once it has used them; nothing in
this module can reconstruct them from `bid` alone, which is exactly the
"the relay database sees `bid`, `expiresAt`... nothing" row of §3.2's "what
each party can see" table).

`bid` (the document id) is the HKDF-derived first 12 hex characters
`app/devsetup.derive()` computes, *not* the eventual `device_id` -- a single
device can be issued a fresh setup code more than once (§3.5 rotation), each
time under a different `bid`, so this collection is keyed by the bootstrap
credential's own identity rather than the device's.
"""

from __future__ import annotations

from datetime import datetime

from google.cloud.firestore import FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db


class SetupCode(BaseModel):
    model_config = ConfigDict(extra="ignore")

    bid: str
    deviceId: str
    expiresAt: datetime


def _codes():
    return get_db().collection("setupCodes")


def create(bid: str, device_id: str, expires_at: datetime) -> SetupCode:
    _codes().document(bid).set({"deviceId": device_id, "expiresAt": expires_at})
    fetched = get(bid)
    assert fetched is not None
    return fetched


def get(bid: str) -> SetupCode | None:
    snap = _codes().document(bid).get()
    if not snap.exists:
        return None
    return SetupCode.model_validate({"bid": bid, **(snap.to_dict() or {})})


def delete(bid: str) -> None:
    _codes().document(bid).delete()


def list_expired(now: datetime) -> list[SetupCode]:
    """Every `setupCodes/{bid}` whose `expiresAt` is already in the past --
    `app/devsetup.expire()`'s input, mirroring §3.2's "`jobs.tick` ... expires
    stale codes the same way [as `complete`]"."""
    query = _codes().where(filter=FieldFilter("expiresAt", "<", now))
    return [
        SetupCode.model_validate({"bid": snap.id, **(snap.to_dict() or {})})
        for snap in query.stream()
    ]
