"""`heldChat/br_{bridgeId}_{eventId}` -- Google Chat texts from a conversation
nobody subscribed to yet (docs/BRIDGE_PHONE_DESIGN.md decision 5, gchat bullet).

Server-only. The doc id is the **same string** as the live `wire_id`, so a
backlog release (`wire_id=row.id`) and a retried live delivery dedup against
each other. Queries are single-equality on `convRowId`
(`bridge_conversations.row_id`), the rest filtered in Python.
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter
from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db

HeldChatStatus = Literal["held", "delivered", "too_long", "dismissed"]

HELD_CAP = 25
BODY_MAX_CODEPOINTS = 1600


class HeldChat(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    bridgeId: str
    conversationId: str
    convRowId: str
    familyId: str
    toUid: str
    senderName: str = ""
    body: str
    receivedAt: datetime
    status: HeldChatStatus = "held"
    decidedAt: datetime | None = None


def _col():
    return get_db().collection("heldChat")


def create(
    hid: str,
    *,
    bridge_id: str,
    conversation_id: str,
    conv_row_id: str,
    family_id: str,
    to_uid: str,
    sender_name: str,
    body: str,
) -> bool:
    """False when `hid` already exists (a retried event)."""
    try:
        _col().document(hid).create(
            {
                "bridgeId": bridge_id,
                "conversationId": conversation_id,
                "convRowId": conv_row_id,
                "familyId": family_id,
                "toUid": to_uid,
                "senderName": sender_name,
                "body": body[:BODY_MAX_CODEPOINTS],
                "receivedAt": SERVER_TIMESTAMP,
                "status": "held",
                "decidedAt": None,
            }
        )
    except AlreadyExists:
        return False
    return True


def exists(hid: str) -> bool:
    return _col().document(hid).get().exists


def list_for_conversation(
    conv_row_id: str, status: HeldChatStatus | None = "held"
) -> list[HeldChat]:
    """Oldest first; `status=None` = every status."""
    rows = [
        HeldChat.model_validate({"id": s.id, **(s.to_dict() or {})})
        for s in _col().where(filter=FieldFilter("convRowId", "==", conv_row_id)).stream()
    ]
    if status is not None:
        rows = [r for r in rows if r.status == status]
    rows.sort(key=lambda r: (r.receivedAt, r.id))
    return rows


def count_held(conv_row_id: str) -> int:
    return len(list_for_conversation(conv_row_id, "held"))


def set_status(ids: list[str], status: HeldChatStatus) -> None:
    for hid in ids:
        _col().document(hid).update({"status": status, "decidedAt": SERVER_TIMESTAMP})
