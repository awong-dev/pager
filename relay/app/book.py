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
from app.store import contacts as contacts_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import users as users_store
from app.store.devices import SmsContact
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
    # externals only: whether this contact is within the pager's `cfg.sms`
    # cap (the first `devices_store.MAX_SMS_CONTACTS` by name).
    onPager: bool | None = None

    @property
    def label(self) -> str:
        return self.nick or self.displayName


def same_family_persons(a: User, b: User) -> bool:
    """docs/ADDRESS_BOOK_DESIGN.md decision 2: two persons of one (non-null)
    family count as approved for each other."""
    return a.kind == "person" and b.kind == "person" and a.familyId is not None and a.familyId == b.familyId


def edge_or_family(sender: User, recipient: User) -> bool:
    """`allow/{sender}_{recipient}.message`, with the same-family implied
    approval applied only where no edge doc exists: an explicit edge with
    `message: false` is a deny and wins over the family default."""
    edge = allow_store.get_edge(sender.uid, recipient.uid)
    if edge is not None:
        return edge.message
    return same_family_persons(sender, recipient)


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


def truncate_sms_name(name: str) -> str:
    """`app/store/devices.py`'s `SmsContact.name` caps (16 code points, 24
    UTF-8 bytes) -- a contact's name comes from an admin-typed string with
    no such cap, so this truncates rather than 500ing/422ing on a perfectly
    normal display name the pager just can't show in full (same "truncate,
    don't reject" convention `app/devcfg.py`'s book projection already uses
    for `displayName`)."""
    truncated = name[: devices_store.SMS_CONTACT_NAME_MAX_CODEPOINTS]
    while len(truncated.encode("utf-8")) > devices_store.SMS_CONTACT_NAME_MAX_UTF8_BYTES:
        truncated = truncated[:-1]
    return truncated or "?"


def sms_contacts_for(owner: User) -> list[User]:
    """The SMS contacts `owner` may text from their pager (docs/V02_DESIGN.md
    §6's `cfg.sms`), uncapped and sorted by the name the pager shows.

    `[]` unless `owner` is an enabled person with a family. Candidates are
    that family's enabled externals that have a phone. A candidate is
    included when `allow/{owner}_{x}.message` is true (explicit approval),
    or when the owner's outbound policy allows any number
    (`policy.rule(out, "external") == "any"`) and no explicit
    `allow/{owner}_{x}` carries `message: false` -- the same "family default,
    explicit deny wins" as `edge_or_family`. The one source for `cfg.sms`,
    `devices.smsContacts`, the address book's external rows and the ingest
    in-book check, so they cannot drift."""
    if owner.kind != "person" or owner.disabled or owner.familyId is None:
        return []
    explicit = {e.toUid: e.message for e in allow_store.list_edges() if e.fromUid == owner.uid}
    implied = policy_module.rule(owner.policy.out, "external") == "any"
    out = [
        u
        for u in users_store.list_users()
        if u.kind == "external"
        and u.ownerFamilyId == owner.familyId
        and u.phone
        and not u.disabled
        and (explicit.get(u.uid) is True or (implied and explicit.get(u.uid) is not False))
    ]
    out.sort(key=lambda u: (truncate_sms_name(u.displayName).casefold(), u.uid))
    return out


def rederive_sms_contacts(owner_uid: str, broker: BrokerClient) -> None:
    """`devices.smsContacts` is `sms_contacts_for(owner)` capped at
    `MAX_SMS_CONTACTS`, written to and pushed (`cfg.sms`) to each of the
    owner's devices. Derived and idempotent; a stale concurrent write is
    repaired by the next trigger."""
    from app import devcfg

    owner = users_store.get_user(owner_uid)
    if owner is None or owner.familyId is None:
        return
    contacts = sms_contacts_for(owner)[: devices_store.MAX_SMS_CONTACTS]
    sms_contacts = [
        SmsContact(name=truncate_sms_name(u.displayName), phone=u.phone or "") for u in contacts
    ]
    devices = devices_store.list_devices(owner_uid=owner_uid)
    for device in devices:
        devices_store.set_sms_contacts(device.id, sms_contacts)
        devcfg.push_sms_contacts(device.id, [c.model_dump() for c in sms_contacts], broker)
    implied = sum(
        1
        for u in contacts
        if (e := allow_store.get_edge(owner_uid, u.uid)) is None or not e.message
    )
    logger.info(
        "sms_contacts rederived owner=%s n=%d implied=%d devices=%d",
        owner_uid,
        len(contacts),
        implied,
        len(devices),
    )


def rederive_family_sms_contacts(family_id: str, broker: BrokerClient) -> None:
    """`rederive_sms_contacts` for every person of `family_id`."""
    for user in users_store.list_users():
        if user.kind == "person" and user.familyId == family_id:
            rederive_sms_contacts(user.uid, broker)


def entries_for(owner_uid: str) -> list[BookEntry]:
    """Same-family persons (not self, not disabled) U the owner's outgoing
    message-edge peers (not disabled) U the owner's groups, de-duplicated by
    uid. `sendable`/`reason` are `policy.check` with the edge flags computed
    as `routing` computes them (edge OR same-family persons)."""
    owner = users_store.get_user(owner_uid)
    if owner is None:
        return []
    nicks = _nicks(owner_uid)
    all_edges = allow_store.list_edges()
    edges = [e for e in all_edges if e.message]
    out_edges = {e.toUid for e in edges if e.fromUid == owner_uid}
    in_edges = {e.fromUid for e in edges if e.toUid == owner_uid}
    # Explicit `message: false` edges: denies that beat the family default.
    denied_out = {e.toUid for e in all_edges if e.fromUid == owner_uid and not e.message}
    denied_in = {e.fromUid for e in all_edges if e.toUid == owner_uid and not e.message}

    sms_contacts = sms_contacts_for(owner)
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
        if user is None or user.disabled or user.kind == "external":
            continue
        peers[uid] = (user, same_family_persons(owner, user))

    entries: list[BookEntry] = []
    for uid, (user, in_family) in peers.items():
        family = same_family_persons(owner, user)
        has_out = uid in out_edges or (family and uid not in denied_out)
        has_in = uid in in_edges or (family and uid not in denied_in)
        reason = policy_module.check(owner, user, has_out, has_in)
        entries.append(
            BookEntry(
                uid=uid,
                alias=user.alias,
                kind="person",
                displayName=user.displayName,
                nick=nicks.get(uid),
                phone=None,
                inFamily=in_family,
                sendable=reason is None,
                reason=reason,
            )
        )
    # Externals come only from `sms_contacts_for` (the list `cfg.sms` is cut
    # from): the relay never texts them, so they are always `sendable` as far
    # as the pager's own SMS path goes.
    for index, contact in enumerate(sms_contacts):
        entries.append(
            BookEntry(
                uid=contact.uid,
                alias=contact.alias,
                kind="external",
                displayName=contact.displayName,
                nick=nicks.get(contact.uid),
                phone=contact.phone,
                inFamily=False,
                sendable=True,
                reason=None,
                onPager=index < devices_store.MAX_SMS_CONTACTS,
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
