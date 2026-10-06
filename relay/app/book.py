"""The derived address book -- docs/ADDRESS_BOOK_DESIGN.md decisions 1-3, 6-7.

The book is *derived*, never stored: one function, `entries_for`, feeds the
pager (`app/devcfg.py`), `GET /api/book` and the web's New chat. The only
stored piece is a per-owner nickname, `users/{owner}/book/{peer}`.

No FastAPI imports. `app.devcfg` is imported lazily where it is needed for
pushing, because `devcfg._approved_contacts` itself calls `entries_for`.
"""

from __future__ import annotations

import logging
import unicodedata
from collections.abc import Iterable
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

from google.cloud import firestore
from google.cloud.firestore import Transaction

from app import policy as policy_module
from app.db.firestore import get_db, run_transaction
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import users as users_store
from app.store.users import User

if TYPE_CHECKING:
    from app.broker import BrokerClient

logger = logging.getLogger("relay.book")

# docs/ADDRESS_BOOK_DESIGN.md decision 4: the wire's `c[].n` bounds.
NICK_MAX_CODEPOINTS = 16
NICK_MAX_UTF8_BYTES = 48


@dataclass
class BookEntry:
    uid: str | None  # None for a group
    alias: str
    kind: Literal["person", "external", "group"]
    displayName: str
    nick: str | None
    phone: str | None
    inFamily: bool
    sendable: bool
    reason: str | None

    @property
    def label(self) -> str:
        return self.nick or self.displayName


def same_family_persons(a: User, b: User) -> bool:
    """docs/ADDRESS_BOOK_DESIGN.md decision 2: two persons of one (non-null)
    family count as approved for each other."""
    return a.kind == "person" and b.kind == "person" and a.familyId is not None and a.familyId == b.familyId


def validate_nick(raw: str) -> str:
    """Trim and check a nickname against the wire's `n` bounds; `ValueError`
    with a human message on violation (rejected, never truncated)."""
    # Control characters are checked on the raw text, before trimming: a
    # trailing "\n" from a paste is rejected, not silently eaten.
    if any(unicodedata.category(ch).startswith("C") for ch in raw):
        raise ValueError("nickname must not contain control characters")
    nick = raw.strip()
    if not nick:
        raise ValueError("nickname must not be empty")
    if len(nick) > NICK_MAX_CODEPOINTS:
        raise ValueError(f"nickname must be at most {NICK_MAX_CODEPOINTS} characters")
    if len(nick.encode("utf-8")) > NICK_MAX_UTF8_BYTES:
        raise ValueError(f"nickname must be at most {NICK_MAX_UTF8_BYTES} bytes in UTF-8")
    return nick


def _book_col(owner_uid: str):
    return get_db().collection("users").document(owner_uid).collection("book")


def _nicks(owner_uid: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for snap in _book_col(owner_uid).stream():
        nick = (snap.to_dict() or {}).get("nick")
        if isinstance(nick, str) and nick:
            out[snap.id] = nick
    return out


def _sms_phone(uid: str) -> str | None:
    for backend in backends_store.list_backends(uid):
        if backend.kind == "sms":
            return backend.config.get("phone")
    return None


def entries_for(owner_uid: str) -> list[BookEntry]:
    """Same-family persons (not self, not disabled) U the owner's outgoing
    message-edge peers (not disabled) U the owner's groups, de-duplicated by
    uid. `sendable`/`reason` are `policy.check` with the edge flags computed
    as `routing` computes them (edge OR same-family persons)."""
    owner = users_store.get_user(owner_uid)
    if owner is None:
        return []
    nicks = _nicks(owner_uid)
    edges = [e for e in allow_store.list_edges() if e.message]
    out_edges = {e.toUid for e in edges if e.fromUid == owner_uid}
    in_edges = {e.fromUid for e in edges if e.toUid == owner_uid}

    peers: dict[str, tuple[User, bool]] = {}
    if owner.familyId is not None:
        for user in users_store.list_users():
            if (
                user.uid != owner_uid
                and user.kind == "person"
                and user.familyId == owner.familyId
                and not user.disabled
            ):
                peers[user.uid] = (user, True)
    for uid in sorted(out_edges):
        if uid == owner_uid or uid in peers:
            continue
        user = users_store.get_user(uid)
        if user is None or user.disabled:
            continue
        peers[uid] = (user, same_family_persons(owner, user))

    entries: list[BookEntry] = []
    for uid, (user, in_family) in peers.items():
        has_out = uid in out_edges or same_family_persons(owner, user)
        has_in = uid in in_edges or same_family_persons(owner, user)
        reason = policy_module.check(owner, user, has_out, has_in)
        entries.append(
            BookEntry(
                uid=uid,
                alias=user.alias,
                kind="external" if user.kind == "external" else "person",
                displayName=user.displayName,
                nick=nicks.get(uid),
                phone=_sms_phone(uid) if user.kind == "external" else None,
                inFamily=in_family,
                sendable=reason is None,
                reason=reason,
            )
        )
    for conv in conversations_store.list_groups_for_member(owner_uid):
        if conv.alias is None:
            continue
        entries.append(
            BookEntry(
                uid=None,
                alias=conv.alias,
                kind="group",
                displayName=conv.name or conv.alias,
                nick=None,
                phone=None,
                inFamily=False,
                sendable=True,
                reason=None,
            )
        )
    return entries


def set_nick(owner_uid: str, peer_uid: str, nick: str | None, by_uid: str) -> None:
    """ONE transaction (decision 7): write/delete `users/{owner}/book/{peer}`
    and bump `bookVersion` on every device the owner has. The push follows
    outside (`push_only`); a lost push is repaired by the next `/status`,
    a lost bump would never be."""
    owner = users_store.get_user(owner_uid)
    family_id = owner.familyId if owner is not None else None
    db = get_db()
    doc = _book_col(owner_uid).document(peer_uid)
    devices_query = db.collection("devices").where(
        filter=firestore.FieldFilter("ownerUid", "==", owner_uid)
    )

    def _txn(transaction: Transaction) -> None:
        # All reads before any write.
        snaps = list(devices_query.stream(transaction=transaction))
        bumps = [(s.reference, int((s.to_dict() or {}).get("bookVersion", 0) or 0) + 1) for s in snaps]
        if nick is None:
            transaction.delete(doc)
        else:
            transaction.set(
                doc,
                {
                    "nick": nick,
                    "familyId": family_id,
                    "updatedAt": firestore.SERVER_TIMESTAMP,
                    "updatedBy": by_uid,
                },
            )
        for ref, bv in bumps:
            transaction.update(ref, {"bookVersion": bv})

    run_transaction(_txn)
    logger.info("book bump reason=nick owner=%s", owner_uid)


def _device_ids(owner_uid: str) -> list[str]:
    return [d.id for d in devices_store.list_devices(owner_uid=owner_uid)]


def bump_and_push(owner_uids: Iterable[str], broker: BrokerClient, reason: str) -> None:
    """Per distinct device: `bump_book_version` then `devcfg.push_book`.
    Best-effort and idempotent (extra bump on retry is harmless)."""
    from app import devcfg

    seen: set[str] = set()
    for owner_uid in sorted(set(owner_uids)):
        ids = [d for d in _device_ids(owner_uid) if d not in seen]
        seen.update(ids)
        for device_id in ids:
            contacts_store.bump_book_version(device_id)
            devcfg.push_book(device_id, broker)
        logger.info("book bump reason=%s owner=%s devices=%d", reason, owner_uid, len(ids))


def push_only(owner_uid: str, broker: BrokerClient) -> None:
    """Push (no bump) for each of the owner's devices -- after `set_nick`."""
    from app import devcfg

    for device_id in _device_ids(owner_uid):
        devcfg.push_book(device_id, broker)


def family_uids(family_id: str | None) -> set[str]:
    if family_id is None:
        return set()
    return {
        u.uid for u in users_store.list_users() if u.kind == "person" and u.familyId == family_id
    }


def edge_holders(uid: str) -> set[str]:
    """Every uid with an outgoing message edge to `uid`."""
    return {e.fromUid for e in allow_store.list_edges() if e.toUid == uid and e.message}
