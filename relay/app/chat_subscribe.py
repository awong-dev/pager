"""Subscribing a bridge phone's Google Chat conversations to a member's pager --
docs/BRIDGE_PHONE_DESIGN.md decisions 7, 9, 10.

A subscribed conversation is an *external* user (`externals.get_or_create_chat`)
with a `bridge` backend row; a Chat **group** also gets a group `conversations`
doc (`conversations.create_bridge_group`) so the pager sees a group page with
`sndr` = the roster nick. Every step is idempotent on retry: the ids derive
from `(bridgeId, conversationId)`, so a retry finds what a crashed attempt
wrote (design "Transaction boundaries").
"""

from __future__ import annotations

import hashlib
import logging
import re
import unicodedata
from collections.abc import Iterable
from dataclasses import dataclass
from typing import Any

from app import book
from app import policy as policy_module
from app.backends import sms_twilio
from app.broker import BrokerClient
from app.routing import Routing
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import bridge_conversations
from app.store import bridges as bridges_store
from app.store import conversations as conversations_store
from app.store import externals as externals_store
from app.store import held_chat as held_chat_store
from app.store import users as users_store
from app.store.bridge_conversations import BridgeConversation
from app.store.users import User
from app.wire import is_valid_alias

logger = logging.getLogger("relay.chat_subscribe")

NICK_MAX = 16


class ChatError(Exception):
    """An HTTP-shaped failure (`status_code`, `detail`) the router re-raises."""

    def __init__(self, status_code: int, detail: str) -> None:
        super().__init__(detail)
        self.status_code = status_code
        self.detail = detail


@dataclass
class SubscribeResult:
    uid: str
    convKey: str | None
    alias: str
    delivered: int
    undelivered: int


def slug_nick(name: str, taken: Iterable[str]) -> str:
    """A roster nick for `name`: NFKD, ASCII only, lowercase, runs of
    `[^a-z0-9]` -> `-`, stripped, cut to 16; empty -> `p` + sha256(name)[:6];
    a collision with `taken` cuts to 13 and appends `-2`, `-3`, ... The
    result always passes `wire.is_valid_alias`."""
    taken_set = set(taken) | {"system"}
    ascii_name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode("ascii")
    nick = re.sub(r"[^a-z0-9]+", "-", ascii_name.lower()).strip("-")[:NICK_MAX].strip("-")
    if not nick:
        nick = "p" + hashlib.sha256(name.encode()).hexdigest()[:6]
    if nick not in taken_set:
        return nick
    for i in range(2, 10_000):
        suffix = f"-{i}"
        candidate = nick[: min(13, NICK_MAX - len(suffix))] + suffix
        if candidate not in taken_set:
            return candidate
    raise ChatError(500, "no free roster nick")  # unreachable at human scale


def validate_roster(roster: list[tuple[str, str]]) -> dict[str, str]:
    """`[(name, nick)]` -> `{nick: name}`; each nick must be alias-shaped
    (it travels as the wire's `sndr`) and unique, else a 422."""
    out: dict[str, str] = {}
    for name, nick in roster:
        if not name.strip():
            raise ChatError(422, "roster names must not be empty")
        if nick == "system" or not is_valid_alias(nick):
            raise ChatError(422, f"roster nick {nick!r} must be 1-16 lowercase letters, digits, - or _")
        if nick in out:
            raise ChatError(422, f"roster nick {nick!r} is used twice")
        out[nick] = name.strip()
    return out


def _require_owner(bridge: bridges_store.Bridge, family_id: str) -> User:
    if bridge.familyId != family_id:
        raise ChatError(404, "no such bridge")
    owner = users_store.get_user(bridge.ownerUid)
    if owner is None or owner.kind != "person" or owner.disabled or owner.familyId != family_id:
        raise ChatError(409, "the bridge owner is not an active member of this family")
    return owner


def _require_row(bridge: bridges_store.Bridge, conversation_id: str) -> BridgeConversation:
    row = bridge_conversations.get(bridge.id, conversation_id)
    if row is None:
        raise ChatError(404, "no such conversation")
    return row


def _validate_name(owner: User, name: str, ext_uid: str, group_alias: str | None) -> str:
    try:
        nick = book.validate_nick(name)
    except ValueError as exc:
        raise ChatError(422, str(exc)) from exc
    taken = {
        e.label.casefold()
        for e in book.entries_for(owner.uid)
        if e.uid != ext_uid and e.alias != group_alias
    }
    if nick.casefold() in taken:
        raise ChatError(409, f'"{nick}" is already in the address book')
    return nick


def _recipient_alias(row: BridgeConversation, owner: User) -> str:
    if row.isGroup and row.convKey:
        group = conversations_store.get_bridge_group(row.convKey)
        if group is not None and group.alias:
            return group.alias
    return owner.alias


def _nick_for(conv_key: str, sender_name: str) -> str:
    """The roster nick of `sender_name`; an unseen speaker gets a slug nick
    that is added to the roster (so they can be renamed later)."""
    group = conversations_store.get_bridge_group(conv_key)
    roster = dict(group.roster) if group is not None else {}
    for nick, name in roster.items():
        if name == sender_name:
            return nick
    nick = slug_nick(sender_name or "someone", roster)
    roster[nick] = sender_name or "someone"
    conversations_store.update_bridge_group(conv_key, roster=roster)
    return nick


def _send_text(
    routing: Routing,
    row: BridgeConversation,
    owner: User,
    *,
    body: str,
    sender_name: str,
    wire_id: str,
    ts: int | None,
) -> str:
    """One text from the conversation to the owner's pager; returns the
    inbound outcome label (`delivered` / `duplicate` / `rejected_<reason>`)."""
    assert row.uid is not None
    sender_alias = None
    if row.isGroup and row.convKey:
        sender_alias = _nick_for(row.convKey, sender_name)
    result = routing.send(
        sender_uid=row.uid,
        recipient_alias=_recipient_alias(row, owner),
        kind="text",
        body=body,
        origin_backend_kind="bridge",
        origin_backend_id="bridge",
        wire_id=wire_id,
        ts=ts,
        sender_alias=sender_alias,
    )
    if result.rejected:
        return f"rejected_{result.rejected[0].reason}"
    return "delivered" if result.messages else "duplicate"


def release_backlog(row: BridgeConversation, owner: User, routing: Routing) -> tuple[int, int]:
    """Decision 7: every `heldChat` row `held` for the conversation, oldest
    first, to the pager with `wire_id=row.id`. A text over 160 code points
    (or empty after control-character cleanup) becomes `too_long`, kept for
    the parents. Returns `(delivered, undelivered)`."""
    delivered = undelivered = 0
    for held in held_chat_store.list_for_conversation(row.id, "held"):
        text = sms_twilio.pager_body(held.body)
        if not text or sms_twilio.body_too_long(text):
            held_chat_store.set_status([held.id], "too_long")
            undelivered += 1
            continue
        outcome = _send_text(
            routing,
            row,
            owner,
            body=text,
            sender_name=held.senderName,
            wire_id=held.id,
            ts=int(held.receivedAt.timestamp()),
        )
        if outcome in ("delivered", "duplicate"):
            held_chat_store.set_status([held.id], "delivered")
            delivered += 1
        else:
            undelivered += 1
    return delivered, undelivered


def subscribe(
    family_id: str,
    bridge: bridges_store.Bridge,
    conversation_id: str,
    pager_name: str,
    can_reply: bool,
    roster: list[tuple[str, str]],
    by_uid: str,
    broker: BrokerClient,
    routing: Routing,
) -> SubscribeResult:
    owner = _require_owner(bridge, family_id)
    if not bridge.paired:
        raise ChatError(409, "the bridge phone is not paired")
    row = _require_row(bridge, conversation_id)
    ext_uid, _ext_alias, _h = externals_store.chat_ids(bridge.id, conversation_id)
    group_alias = conversations_store._bridge_ids(bridge.id, conversation_id)[1] if row.isGroup else None
    name = _validate_name(owner, pager_name, ext_uid, group_alias)
    roster_map = validate_roster(roster) if row.isGroup else {}
    if policy_module.rule(owner.policy.in_, "person") == "none":
        raise ChatError(409, f"@{owner.alias}'s inbound policy allows nobody")
    # O5: an outbound policy that allows no people forces read-only, never 409.
    if policy_module.rule(owner.policy.out, "person") == "none":
        can_reply = False

    try:
        ext = externals_store.get_or_create_chat(
            family_id,
            bridge.id,
            conversation_id,
            name,
            source=row.source,
            link=row.link,
            is_group=row.isGroup,
            title=row.title,
            can_reply=can_reply,
        )
    except externals_store.ContactNameTaken as exc:
        raise ChatError(409, str(exc)) from exc
    if ext.displayName != name:
        # A retry with a different name: the external keeps the name the
        # crashed attempt gave it, so rename it (the book bump follows below).
        try:
            externals_store.rename(family_id, ext.uid, name)
        except externals_store.ContactNameTaken as exc:
            raise ChatError(409, str(exc)) from exc

    conv_key: str | None = None
    if row.isGroup:
        group = conversations_store.create_bridge_group(
            name=name,
            owner_uid=owner.uid,
            external_uid=ext.uid,
            bridge_id=bridge.id,
            conversation_id=conversation_id,
            source=row.source,
            link=row.link,
            roster=roster_map,
        )
        conv_key = group.convKey
        conversations_store.update_bridge_group(conv_key, name=name, roster=roster_map)
    allow_store.set_edge(owner.uid, ext.uid, message=True, locate=False)
    allow_store.recompute_locatable_by_for_owner(ext.uid)
    externals_store.update_chat(ext.uid, canReply=can_reply)

    # Subscribed *before* the backlog, so an event landing mid-subscribe is
    # delivered live instead of held behind an alert that is about to be decided.
    title16 = (row.title or "")[:16]
    bridge_conversations.set_fields(
        row.id,
        status="subscribed" if row.status in ("seen", "ignored") else row.status,
        uid=ext.uid,
        convKey=conv_key,
        pagerName=name,
        customName=name != title16,
    )
    book.bump_and_push({owner.uid}, broker, reason="chat_subscribe")
    fresh = bridge_conversations.get(bridge.id, conversation_id)
    assert fresh is not None
    delivered, undelivered = release_backlog(fresh, owner, routing)
    if fresh.alertId:
        alert = alerts_store.get(family_id, fresh.alertId)
        if alert is not None and alert.status == "open":
            alerts_store.decide(family_id, fresh.alertId, "handled", by_uid)
    bridge_conversations.set_fields(row.id, heldCount=held_chat_store.count_held(row.id))
    logger.info(
        "chat subscribed bridge=%s owner=%s group=%s delivered=%d undelivered=%d",
        bridge.id, owner.uid, row.isGroup, delivered, undelivered,
    )
    alias = conversations_store._bridge_ids(bridge.id, conversation_id)[1] if row.isGroup else ext.alias
    return SubscribeResult(ext.uid, conv_key, alias, delivered, undelivered)


def deliver_subscribed(
    bridge: bridges_store.Bridge,
    row: BridgeConversation,
    event: Any,
    routing: Routing,
    broker: BrokerClient,
    previous_title: str | None,
) -> str | None:
    """A text from a subscribed conversation, live (`POST /bridge/events`).
    Returns the outcome label, or `None` when the subscription is broken (the
    external is gone) and the text should be held like an unknown one."""
    from app.inbound_text import placeholder_for

    owner = users_store.get_user(bridge.ownerUid)
    if owner is None or row.uid is None or users_store.get_user(row.uid) is None:
        return None
    _follow_title(bridge, row, owner, previous_title, broker)
    raw = event.text.strip()
    body = sms_twilio.pager_body(raw) or (
        placeholder_for([a.kind for a in event.attachments]) if event.attachments else ""
    )
    if not body:
        return "dropped_empty"
    wire_id = f"br_{bridge.id}_{event.id}"
    if sms_twilio.body_too_long(body):
        # Kept for the parents, never on the pager (decision 7).
        if held_chat_store.create(
            wire_id,
            bridge_id=bridge.id,
            conversation_id=row.conversationId,
            conv_row_id=row.id,
            family_id=bridge.familyId,
            to_uid=owner.uid,
            sender_name=event.sender.name,
            body=body,
        ):
            held_chat_store.set_status([wire_id], "too_long")
        return "too_long"
    return _send_text(
        routing,
        row,
        owner,
        body=body,
        sender_name=event.sender.name,
        wire_id=wire_id,
        ts=int(event.ts) if event.ts else None,
    )


def _follow_title(
    bridge: bridges_store.Bridge,
    row: BridgeConversation,
    owner: User,
    previous_title: str | None,
    broker: BrokerClient,
) -> None:
    """Decision 6: a title change from the phone updates the group's name /
    the external's displayName unless a parent renamed it (`customName`)."""
    if row.customName or not row.title or row.title == previous_title or not row.uid:
        return
    new_name = row.title[:NICK_MAX]
    try:
        new_name = book.validate_nick(new_name)
    except ValueError:
        return
    if new_name == row.pagerName:
        return
    try:
        externals_store.rename(bridge.familyId, row.uid, new_name)
    except externals_store.ContactNameTaken:
        return
    if row.convKey:
        conversations_store.update_bridge_group(row.convKey, name=new_name)
    externals_store.update_chat(row.uid, title=row.title)
    bridge_conversations.set_fields(row.id, pagerName=new_name)
    book.bump_and_push({owner.uid}, broker, reason="chat_title")


def drop_held(family_id: str, row: BridgeConversation, by_uid: str) -> None:
    """Dismiss everything waiting on a not-subscribed conversation: the held
    rows, the open alert, and the row's `heldCount` / `alertId` (so a later
    text raises a fresh alert). Shared by Ignore, the alert's Dismiss and
    reassign."""
    held_chat_store.set_status(
        [r.id for r in held_chat_store.list_for_conversation(row.id, "held")], "dismissed"
    )
    if row.alertId:
        alert = alerts_store.get(family_id, row.alertId)
        if alert is not None and alert.status == "open":
            alerts_store.decide(family_id, row.alertId, "dismissed", by_uid)
    bridge_conversations.set_fields(row.id, heldCount=0, alertId=None)


def dismiss_alert_conversation(family_id: str, bridge_id: str, conversation_id: str, by_uid: str) -> None:
    """`POST /alerts/{id}/dismiss` on a `chat_unknown` alert: the row stays
    `seen`. The caller decides the alert itself."""
    row = bridge_conversations.get(bridge_id, conversation_id)
    if row is not None:
        drop_held(family_id, row, by_uid)


def ignore(
    family_id: str, bridge: bridges_store.Bridge, conversation_id: str, by_uid: str
) -> BridgeConversation:
    if bridge.familyId != family_id:
        raise ChatError(404, "no such bridge")
    row = _require_row(bridge, conversation_id)
    if row.status in ("subscribed", "paused"):
        raise ChatError(409, "unsubscribe this conversation first")
    bridge_conversations.set_status(row.id, "ignored")
    drop_held(family_id, row, by_uid)
    fresh = bridge_conversations.get(bridge.id, conversation_id)
    assert fresh is not None
    return fresh


def patch(
    family_id: str,
    bridge: bridges_store.Bridge,
    conversation_id: str,
    *,
    pager_name: str | None,
    can_reply: bool | None,
    paused: bool | None,
    roster: list[tuple[str, str]] | None,
    broker: BrokerClient,
) -> BridgeConversation:
    owner = _require_owner(bridge, family_id)
    row = _require_row(bridge, conversation_id)
    if row.status not in ("subscribed", "paused") or not row.uid:
        raise ChatError(409, "this conversation is not subscribed")
    changed = False
    if pager_name is not None:
        name = _validate_name(
            owner,
            pager_name,
            row.uid,
            conversations_store._bridge_ids(bridge.id, conversation_id)[1] if row.isGroup else None,
        )
        try:
            externals_store.rename(family_id, row.uid, name)
        except externals_store.ContactNameTaken as exc:
            raise ChatError(409, str(exc)) from exc
        if row.convKey:
            conversations_store.update_bridge_group(row.convKey, name=name)
        bridge_conversations.set_fields(row.id, pagerName=name, customName=True)
        changed = True
    if can_reply is not None:
        if can_reply and policy_module.rule(owner.policy.out, "person") == "none":
            raise ChatError(409, f"@{owner.alias}'s policy does not allow outbound messages")
        externals_store.update_chat(row.uid, canReply=can_reply)
        changed = True
    if roster is not None:
        if not row.isGroup or not row.convKey:
            raise ChatError(422, "only a group has a roster")
        conversations_store.update_bridge_group(row.convKey, roster=validate_roster(roster))
    if paused is not None:
        bridge_conversations.set_status(row.id, "paused" if paused else "subscribed")
        changed = True
    if changed:
        book.bump_and_push({owner.uid}, broker, reason="chat_patch")
    fresh = bridge_conversations.get(bridge.id, conversation_id)
    assert fresh is not None
    return fresh


def unsubscribe(
    family_id: str, bridge: bridges_store.Bridge, conversation_id: str, broker: BrokerClient
) -> BridgeConversation:
    """Edges, group doc and external deleted; the row goes back to `seen`.
    Messages keep their history (per-copy visibility)."""
    if bridge.familyId != family_id:
        raise ChatError(404, "no such bridge")
    owner = users_store.get_user(bridge.ownerUid)  # may be disabled: reassign away from them
    row = _require_row(bridge, conversation_id)
    if row.uid:
        if row.convKey:
            conversations_store.delete_bridge_group(row.convKey)
        externals_store.delete(family_id, row.uid)  # its edges, name, backend, user
    bridge_conversations.set_fields(
        row.id, status="seen", uid=None, convKey=None, pagerName=None, customName=False
    )
    if owner is not None:
        book.bump_and_push({owner.uid}, broker, reason="chat_unsubscribe")
    fresh = bridge_conversations.get(bridge.id, conversation_id)
    assert fresh is not None
    return fresh


def reassign_conversations(
    family_id: str, bridge: bridges_store.Bridge, new_owner_uid: str, by_uid: str, broker: BrokerClient
) -> int:
    """Run **before** `bridge.ownerUid` changes: every subscribed or paused
    conversation is unsubscribed (same path as `DELETE .../{ref}`, history
    kept) and the remaining rows move to the new owner, with whatever was
    held for the old one dismissed. Returns how many were unsubscribed."""
    unsubscribed = 0
    for row in bridge_conversations.list_for_bridge(bridge.id):
        if row.status in ("subscribed", "paused"):
            unsubscribe(family_id, bridge, row.conversationId, broker)
            unsubscribed += 1
    for row in bridge_conversations.list_for_bridge(bridge.id):
        drop_held(family_id, row, by_uid)
        bridge_conversations.set_fields(row.id, ownerUid=new_owner_uid)
    return unsubscribed
