"""Shared inbound-reply recipient resolution for adapters whose inbound
webhook must pick a recipient out of free text -- docs/SERVER_PLAN.md §6.4/
§6.5, both worded identically: *"if the text starts with `@alias ` use it,
else if the user has exactly one allowed peer use that, else reply with a
usage hint"*. `app/backends/sms_twilio.py` and `app/backends/gchat.py` both
call `resolve_reply()` so this one rule lives in one place, per this
phase's brief ("factor that resolution logic into a shared helper both
adapters call, don't duplicate it").
"""

from __future__ import annotations

from dataclasses import dataclass

from app.store import allow as allow_store
from app.store import users as users_store
from app.wire import is_valid_alias

USAGE_HINT = "reply @name your message, or ask your admin"


@dataclass(frozen=True, slots=True)
class ResolvedReply:
    recipient_alias: str
    body: str


def resolve_reply(sender_uid: str, text: str) -> ResolvedReply | None:
    """`None` means no rule resolved a recipient -- the caller must send
    `USAGE_HINT` back over its own channel (never a wire/pager down
    message; there is no device involved on this path)."""
    text = text.strip()
    if not text:
        return None

    if text.startswith("@"):
        # "starts with `@alias `" -- the first whitespace-separated token
        # after `@` is the alias, case-folded to match `ALIAS_RE`'s
        # lowercase-only shape (a human typing on a phone keyboard should
        # not have to get letter case exactly right).
        parts = text[1:].split(None, 1)
        if len(parts) != 2:
            return None
        alias, body = parts[0].lower(), parts[1].strip()
        if not is_valid_alias(alias) or not body:
            return None
        return ResolvedReply(recipient_alias=alias, body=body)

    peers = allow_store.allowed_recipients(sender_uid)
    if len(peers) == 1:
        peer = users_store.get_user(peers[0])
        if peer is not None:
            return ResolvedReply(recipient_alias=peer.alias, body=text)
    return None
