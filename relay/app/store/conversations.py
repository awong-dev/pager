"""Group `conversations/{convKey}` CRUD -- docs/GROUP_CHAT_DESIGN.md §2, §3.

A group conversation is a `conversations/{convKey}` document like any DM's,
except `kind: 'group'`, a minted `g_`-prefixed id (`convKey` is *not* the
derived pair key `app/store/messages.py:conv_key` produces -- a pair key
cannot express 3+ members), and its `uids` field is the *member list*
rather than a fixed two-uid pair. Both client queries and
`firestore.rules` already key membership off `uids array-contains me`, so
this is the whole trick: membership read-scoping comes for free from a field
that already exists.

The group's alias lives in the exact same flat `aliases/{alias}` namespace a
user alias does (`app/store/users.py`) -- a group alias doc is
`{convKey: ...}` where a user's is `{uid: ...}`, so `transaction.create()`
against that one collection is what makes a group alias and a user alias
mutually exclusive, and it's also why `users_store.get_uid_for_alias()`
already returns `None` for a group alias with no group-aware check at all
(it reads a `uid` field a group's alias doc never has).

`app/routing.py`'s group-fan-out branch and `app/routers/conversations.py`'s
group admin endpoints (docs/GROUP_CHAT_DESIGN.md §3) are the only intended
callers besides tests.
"""

from __future__ import annotations

from google.api_core.exceptions import AlreadyExists
from google.cloud.firestore import FieldFilter, Transaction

from app.db.firestore import get_db, run_transaction
from app.ids import new_id
from app.store.messages import (
    Conversation,
    build_participants_and_family_ids,
    get_conversation,
)
from app.store.users import ALIAS_RE, RESERVED_ALIASES, InvalidAlias


def _conversations():
    return get_db().collection("conversations")


def _aliases():
    return get_db().collection("aliases")


def _validate_group_alias(alias: str) -> None:
    # Same regex/reserved-word rule as a user alias (docs/SERVER_PLAN.md
    # §3.1) -- it's the same flat namespace, see this module's docstring.
    if alias in RESERVED_ALIASES or not ALIAS_RE.match(alias):
        raise InvalidAlias(f"invalid or reserved group alias: {alias!r}")


def create_group(
    *,
    name: str,
    alias: str,
    member_uids: list[str],
    created_by: str,
) -> Conversation:
    """Mints a `g_`-prefixed convKey and creates `conversations/{convKey}`
    plus `aliases/{alias} = {convKey}` in one transaction. The alias write
    is staged with `transaction.create()`, exactly like
    `users_store.create_user`'s `aliases/{alias} = {uid}` -- it raises
    `google.api_core.exceptions.AlreadyExists` (left uncaught here; the G2
    API layer turns it into a 409) if `alias` is already taken, by a user or
    another group.

    `member_uids` is de-duplicated and sorted before it's written (G1's
    verify: "members read back sorted"). Whether `created_by` itself must be
    a member is left to the caller (the API layer) -- a store primitive
    shouldn't assume who's calling it; this function only refuses a
    membership list that can't be a group at all (fewer than two distinct
    members)."""
    _validate_group_alias(alias)
    members = sorted(set(member_uids))
    if len(members) < 2:
        raise ValueError("a group needs at least two distinct members")

    key = new_id("g_")
    conv_ref = _conversations().document(key)
    alias_ref = _aliases().document(alias)

    def _txn(transaction: Transaction) -> None:
        # docs/FAMILIES_DESIGN.md §1 decisions 3-4: `participants`/
        # `familyIds` over the full member list, written once here on
        # creation (see `build_participants_and_family_ids`'s docstring).
        participants, family_ids = build_participants_and_family_ids(
            members, transaction=transaction
        )
        transaction.create(alias_ref, {"convKey": key})
        transaction.set(
            conv_ref,
            {
                "uids": members,
                "kind": "group",
                "name": name,
                "alias": alias,
                "createdBy": created_by,
                "lastMessageAt": None,
                "lastPreview": "",
                "unread": {},
                "participants": participants,
                "familyIds": family_ids,
            },
        )

    run_transaction(_txn)

    fetched = get_conversation(key)
    assert fetched is not None
    return fetched


def _bridge_ids(bridge_id: str, conversation_id: str) -> tuple[str, str]:
    """`(convKey, alias)` of a bridged group, derived from the same hash as
    `externals.chat_ids` so a retry finds what a crashed attempt wrote."""
    import hashlib

    h = hashlib.sha256(f"{bridge_id}|{conversation_id}".encode()).hexdigest()
    return f"g_c{h[:14]}", f"bc{h[:8]}"


def create_bridge_group(
    *,
    name: str,
    owner_uid: str,
    external_uid: str,
    bridge_id: str,
    conversation_id: str,
    source: str,
    link: str | None,
    roster: dict[str, str],
) -> Conversation:
    """docs/BRIDGE_PHONE_DESIGN.md decision 7: the group doc of a subscribed
    Google Chat group -- `uids: [owner, external]`, `bridge`, `roster`, the
    relay-minted alias. Doc and alias are written with `create()` in one
    transaction; `AlreadyExists` returns the existing group."""
    key, alias = _bridge_ids(bridge_id, conversation_id)
    conv_ref = _conversations().document(key)
    alias_ref = _aliases().document(alias)
    members = sorted({owner_uid, external_uid})

    def _txn(transaction: Transaction) -> None:
        participants, family_ids = build_participants_and_family_ids(
            members, transaction=transaction
        )
        transaction.create(alias_ref, {"convKey": key})
        transaction.create(
            conv_ref,
            {
                "uids": members,
                "kind": "group",
                "name": name,
                "alias": alias,
                "createdBy": owner_uid,
                "lastMessageAt": None,
                "lastPreview": "",
                "unread": {},
                "participants": participants,
                "familyIds": family_ids,
                "bridge": {
                    "bridgeId": bridge_id,
                    "conversationId": conversation_id,
                    "source": source,
                    "link": link,
                },
                "roster": roster,
            },
        )

    try:
        run_transaction(_txn)
    except AlreadyExists:
        pass
    fetched = get_conversation(key)
    assert fetched is not None
    return fetched


def get_bridge_group(conv_key: str) -> Conversation | None:
    conv = get_conversation(conv_key)
    return conv if conv is not None and conv.bridge is not None else None


def update_bridge_group(
    conv_key: str, *, name: str | None = None, roster: dict[str, str] | None = None
) -> None:
    updates: dict[str, object] = {}
    if name is not None:
        updates["name"] = name
    if roster is not None:
        updates["roster"] = roster
    if updates:
        _conversations().document(conv_key).update(updates)


def delete_bridge_group(conv_key: str) -> None:
    """The group doc and its alias; messages keep their history."""
    conv = get_conversation(conv_key)
    if conv is None:
        return
    if conv.alias:
        _aliases().document(conv.alias).delete()
    _conversations().document(conv_key).delete()


def get_by_alias(alias: str) -> Conversation | None:
    """Resolves a group alias to its `conversations/{convKey}` document.
    `None` if `alias` doesn't exist at all, *or* if it names a user alias
    instead of a group one -- a user's `aliases/{alias}` doc has a `uid`
    field, never `convKey` (see this module's docstring)."""
    snap = _aliases().document(alias).get()
    if not snap.exists:
        return None
    key = (snap.to_dict() or {}).get("convKey")
    if key is None:
        return None
    return get_conversation(key)


def list_groups_for_member(uid: str) -> list[Conversation]:
    """Every group conversation `uid` currently belongs to --
    docs/GROUP_CHAT_DESIGN.md §4's book contacts (`app/devcfg.py`). Queries
    `uids array-contains uid` (the same field/operator the group thread
    query and `firestore.rules` already rely on, §2) and filters
    `kind == 'group'` in Python rather than adding a composite index: this
    collection is one document per conversation at household scale, so a
    second filter pass over an already-indexed array-contains scan needs no
    index of its own."""
    query = _conversations().where(filter=FieldFilter("uids", "array_contains", uid))
    return [
        conv
        for conv in (
            Conversation.model_validate({"convKey": snap.id, **(snap.to_dict() or {})})
            for snap in query.stream()
        )
        if conv.kind == "group"
    ]


def is_member(conv_key: str, uid: str) -> bool:
    conv = get_conversation(conv_key)
    return conv is not None and uid in conv.uids


def add_member(conv_key: str, uid: str) -> Conversation:
    """Adds `uid` to `conversations/{conv_key}.uids` (a no-op, not an
    error, if already a member). Does not touch `allow` edges -- per
    docs/GROUP_CHAT_DESIGN.md decision 2, creating the missing allow edges
    both ways is the caller's (the join endpoint's) job, since it also has
    to know which uid is joining *as whom* is not this module's concern."""
    ref = _conversations().document(conv_key)

    def _txn(transaction: Transaction) -> None:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            raise KeyError(f"no such conversation: {conv_key!r}")
        members = sorted(set(snap.get("uids") or []) | {uid})
        # docs/FAMILIES_DESIGN.md §3's trigger list: a join rewrites both
        # `participants` (the new member's entry appears) and `familyIds`
        # (their family may not have been represented yet).
        participants, family_ids = build_participants_and_family_ids(
            members, transaction=transaction
        )
        transaction.update(
            ref, {"uids": members, "participants": participants, "familyIds": family_ids}
        )

    run_transaction(_txn)
    fetched = get_conversation(conv_key)
    assert fetched is not None
    return fetched


def remove_member(conv_key: str, uid: str) -> Conversation:
    """Removes `uid` from `conversations/{conv_key}.uids` (a no-op if not a
    member). Per §8.6, leaves `allow` edges untouched -- decision 2 says
    what happens to those on removal is explicitly still open."""
    ref = _conversations().document(conv_key)

    def _txn(transaction: Transaction) -> None:
        snap = ref.get(transaction=transaction)
        if not snap.exists:
            raise KeyError(f"no such conversation: {conv_key!r}")
        members = sorted(set(snap.get("uids") or []) - {uid})
        # See `add_member`'s docstring note -- a leave rewrites both the
        # same way (the leaver's `participants` entry and, if they were the
        # only member of their family, that family drops out of
        # `familyIds`).
        participants, family_ids = build_participants_and_family_ids(
            members, transaction=transaction
        )
        transaction.update(
            ref, {"uids": members, "participants": participants, "familyIds": family_ids}
        )

    run_transaction(_txn)
    fetched = get_conversation(conv_key)
    assert fetched is not None
    return fetched
