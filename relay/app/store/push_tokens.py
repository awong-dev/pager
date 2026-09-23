"""`users/{uid}/pushTokens/{token}` -- FCM registration tokens.

Backs `POST/DELETE /api/me/push-tokens` (docs/SERVER_PLAN.md §5.1) and the
retention table's "push tokens: removed after 3 consecutive FCM
'unregistered' errors" row (§5.7). §3's collection diagram lists the API
route but does not draw this collection explicitly -- this is the minimal,
obvious shape it implies: one doc per token (doc id = the token itself, so
re-registering the same token from a second tab/device is naturally
idempotent), storing just enough to drive the retention rule later
(`errorCount`, reserved for FCM send-failure handling; nothing writes it
yet -- this module only reads/writes `createdAt` and adds/removes tokens).
"""

from __future__ import annotations

from datetime import datetime

from google.cloud.firestore import SERVER_TIMESTAMP
from google.cloud.firestore_v1.base_query import FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db


class PushToken(BaseModel):
    model_config = ConfigDict(extra="ignore")

    token: str
    createdAt: datetime | None = None
    errorCount: int = 0


def _tokens(uid: str):
    return get_db().collection("users").document(uid).collection("pushTokens")


def add_token(uid: str, token: str) -> None:
    """Idempotent: re-registering the same token resets `errorCount` to 0
    but keeps the original `createdAt` (merge, not overwrite). The token
    value is mirrored into the `token` field (in addition to being the doc
    id) purely so `remove_token_anywhere()` below can find a doc by token
    value alone via a `collection_group` query."""
    _tokens(uid).document(token).set(
        {"token": token, "createdAt": SERVER_TIMESTAMP, "errorCount": 0}, merge=True
    )


def remove_token(uid: str, token: str) -> None:
    _tokens(uid).document(token).delete()


def remove_token_anywhere(token: str) -> bool:
    """Delete a push token doc without already knowing which user owns it.

    `app/backends/fcm.py`'s `FirebaseFCMClient.send_data(tokens, data)` only
    ever sees the bare token strings FCM handed back (docs/V03_TASKS.md
    3a.1's `send_data(tokens, data)` seam carries no uid), so it cannot call
    `remove_token(uid, token)` directly. A `collection_group` query on the
    `token` field (mirrored onto every doc by `add_token()` above) finds the
    doc regardless of which user's `pushTokens` subcollection it lives in.
    Returns whether a doc was actually found and deleted, for callers/tests
    that want to know.
    """
    db = get_db()
    matches = list(
        db.collection_group("pushTokens").where(filter=FieldFilter("token", "==", token)).stream()
    )
    for snap in matches:
        snap.reference.delete()
    return bool(matches)


def list_tokens(uid: str) -> list[str]:
    """Just the token strings -- what `backends/webapp.py`'s FCM call
    needs."""
    return [snap.id for snap in _tokens(uid).stream()]


def list_token_rows(uid: str) -> list[PushToken]:
    return [
        PushToken.model_validate({"token": snap.id, **(snap.to_dict() or {})})
        for snap in _tokens(uid).stream()
    ]
