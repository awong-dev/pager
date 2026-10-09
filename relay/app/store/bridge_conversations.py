"""`bridgeConversations/{bridgeId}_{ref}` -- docs/BRIDGE_PHONE_DESIGN.md decision 6.

The Google Chat / Voice conversations a bridge phone has seen, whether or not
a parent subscribed to them. Server-only; the web reads it through
`GET /api/family/members/{uid}/chat`. `ref = sha256(conversationId)[:16]`
because Android's `shortcutId` / `sbn.key` may contain `/` and `|`, which
break a document id and a URL segment.
"""

from __future__ import annotations

import hashlib
from datetime import datetime
from typing import Any, Literal

from google.cloud.firestore import SERVER_TIMESTAMP, FieldFilter, Transaction
from pydantic import BaseModel, ConfigDict, Field

from app.db.firestore import get_db, run_transaction

ConvStatus = Literal["seen", "subscribed", "ignored", "paused"]

PEOPLE_MAX = 64
PREVIEW_MAX = 160


class BridgeConversation(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    ref: str
    bridgeId: str
    familyId: str
    ownerUid: str
    source: str
    conversationId: str
    title: str | None = None
    isGroup: bool = False
    link: str | None = None
    people: list[str] = Field(default_factory=list)
    lastPreview: str = ""
    lastAt: datetime | None = None
    firstSeenAt: datetime | None = None
    inspectedAt: datetime | None = None
    heldCount: int = 0
    status: ConvStatus = "seen"
    uid: str | None = None
    convKey: str | None = None
    pagerName: str | None = None
    customName: bool = False
    alertId: str | None = None


def conv_ref(conversation_id: str) -> str:
    return hashlib.sha256(conversation_id.encode()).hexdigest()[:16]


def row_id(bridge_id: str, conversation_id: str) -> str:
    return f"{bridge_id}_{conv_ref(conversation_id)}"


def _col():
    return get_db().collection("bridgeConversations")


def _from_snap(snap) -> BridgeConversation:
    data = snap.to_dict() or {}
    return BridgeConversation.model_validate(
        {"id": snap.id, "ref": conv_ref(data.get("conversationId", "")), **data}
    )


def get(bridge_id: str, conversation_id: str) -> BridgeConversation | None:
    snap = _col().document(row_id(bridge_id, conversation_id)).get()
    return _from_snap(snap) if snap.exists else None


def get_by_ref(bridge_id: str, ref: str) -> BridgeConversation | None:
    snap = _col().document(f"{bridge_id}_{ref}").get()
    return _from_snap(snap) if snap.exists else None


def count_created_since(bridge_id: str, since: datetime) -> int:
    """New rows of `bridge_id` first seen after `since` (the 50-per-day cap).
    One single-field range query, filtered by bridge in Python, so no
    composite index."""
    query = _col().where(filter=FieldFilter("firstSeenAt", ">", since))
    return sum(1 for s in query.stream() if (s.to_dict() or {}).get("bridgeId") == bridge_id)


def upsert_seen(
    bridge: Any,
    *,
    source: str,
    conversation_id: str,
    title: str | None,
    is_group: bool,
    link: str | None,
    speaker: str | None,
    people: list[str],
    preview: str | None,
    inspected: bool = False,
) -> BridgeConversation:
    """Create or update the row in one transaction: `title`/`isGroup`/`link`
    when given, `people` (the speaker and any reported names appended, <= 64,
    insertion order), `lastPreview`/`lastAt` for a message or `inspectedAt`
    for an inspect. Never changes `status`."""
    rid = row_id(bridge.id, conversation_id)
    ref = _col().document(rid)

    def _txn(transaction: Transaction) -> None:
        snap = ref.get(transaction=transaction)
        data = (snap.to_dict() or {}) if snap.exists else {}
        merged = list(data.get("people") or [])
        for name in [speaker, *people]:
            if name and name not in merged and len(merged) < PEOPLE_MAX:
                merged.append(name)
        updates: dict[str, Any] = {"people": merged}
        if title:
            updates["title"] = title
        if link:
            updates["link"] = link
        if inspected:
            updates["inspectedAt"] = SERVER_TIMESTAMP
        if preview is not None:
            updates["lastPreview"] = preview[:PREVIEW_MAX]
            updates["lastAt"] = SERVER_TIMESTAMP
        updates["isGroup"] = is_group
        if snap.exists:
            transaction.update(ref, updates)
        else:
            transaction.create(
                ref,
                {
                    "bridgeId": bridge.id,
                    "familyId": bridge.familyId,
                    "ownerUid": bridge.ownerUid,
                    "source": source,
                    "conversationId": conversation_id,
                    "title": title,
                    "link": link,
                    "lastPreview": "",
                    "lastAt": None,
                    "firstSeenAt": SERVER_TIMESTAMP,
                    "inspectedAt": None,
                    "heldCount": 0,
                    "status": "seen",
                    "uid": None,
                    "convKey": None,
                    "pagerName": None,
                    "customName": False,
                    "alertId": None,
                    **updates,
                },
            )

    run_transaction(_txn)
    fetched = get(bridge.id, conversation_id)
    assert fetched is not None
    return fetched


def list_for_owner(owner_uid: str, statuses: tuple[str, ...] | None = None) -> list[BridgeConversation]:
    query = _col().where(filter=FieldFilter("ownerUid", "==", owner_uid))
    rows = [_from_snap(s) for s in query.stream()]
    if statuses is not None:
        rows = [r for r in rows if r.status in statuses]
    rows.sort(key=lambda r: (r.lastAt or r.firstSeenAt or datetime.min.replace(tzinfo=r.firstSeenAt.tzinfo if r.firstSeenAt else None)), reverse=True)
    return rows


def list_for_bridge(bridge_id: str) -> list[BridgeConversation]:
    query = _col().where(filter=FieldFilter("bridgeId", "==", bridge_id))
    return [_from_snap(s) for s in query.stream()]


def set_status(row_id_: str, status: ConvStatus) -> None:
    _col().document(row_id_).update({"status": status})


def set_fields(row_id_: str, **fields: Any) -> None:
    _col().document(row_id_).update(fields)
