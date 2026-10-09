"""The inbound-text step table for the bridge phone --
docs/RELAY_SMS_DESIGN.md decision 4 from the *blocked* step on
(docs/BRIDGE_PHONE_DESIGN.md decision 5). The owner is known from the bridge;
there are no consent keywords (O1/O3).
"""

from __future__ import annotations

import logging
from collections.abc import Callable, Sequence

from app import alerts as alerts_module
from app import book, sms_text
from app import policy as policy_module
from app.routing import Routing
from app.store import backends as backends_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import held_sms as held_sms_store
from app.store import messages as messages_store
from app.store.users import User

logger = logging.getLogger("relay.inbound_text")


def placeholder_for(attachments: Sequence[str]) -> str:
    """Empty text with attachments: `[photo]` for exactly one image, else
    `[attachment]` (decision 5)."""
    return "[photo]" if list(attachments) == ["image"] else "[attachment]"


def record_channel(
    contact_uid: str, bid: str, member_uid: str, via: str, voice_conv: str | None
) -> None:
    """Decision 5: the channel a member last used with this external, kept
    per member on the external's `sms` backend row (the contact is
    family-wide, so one sibling's Voice thread must not switch another's)."""
    row = backends_store.get_backend(contact_uid, bid)
    if row is None:
        return
    config = dict(row.config)
    via_map = dict(config.get("via") or {})
    conv_map = dict(config.get("voiceConv") or {})
    changed = False
    if via_map.get(member_uid) != via:
        via_map[member_uid] = via
        changed = True
    # `voiceConv` holds the Voice thread id or, for `whatsapp`, the chat JID
    # (WA2): the member's last channel is the only one used, so one slot is enough.
    if via in ("gvoice", "whatsapp") and voice_conv and conv_map.get(member_uid) != voice_conv:
        conv_map[member_uid] = voice_conv
        changed = True
    if changed:
        config["via"] = via_map
        if conv_map:
            config["voiceConv"] = conv_map
        backends_store.update_backend(contact_uid, bid, config=config)


def handle_text(
    target: User,
    from_number: str,
    raw_body: str,
    sid: str,
    routing: Routing,
    *,
    reply: Callable[[str], object],
    attachments: Sequence[str] = (),
    via: str | None = None,
    voice_conv: str | None = None,
) -> str:
    """Blocked, duplicate, empty, known contact -> deliver, else held.
    Returns the outcome label. `reply(text)` sends a text back to
    `from_number` (the `too_long` hint). `via` (`sms`|`gvoice`|`whatsapp`, bridge only)
    is recorded per member on a delivered text."""
    family_id = target.familyId
    assert family_id is not None
    family = families_store.get_family(family_id)
    if family is not None and from_number in family.blockedNumbers:
        return "blocked"

    if held_sms_store.exists(sid) or messages_store.wire_id_exists(sid, target.uid):
        return "duplicate"

    # `raw_body` is kept as received (held rows show parents the original);
    # `body` is what may reach a pager (§3.1 control characters).
    raw_body = (raw_body or "").strip()
    body = sms_text.pager_body(raw_body)
    if not body:
        if not attachments:
            return "dropped_empty"
        raw_body = body = placeholder_for(attachments)

    contact = externals_store.get_family_contact(family_id, from_number)
    if (
        contact is not None
        and not contact.disabled
        and policy_module.check(contact, target, False, book.edge_or_family(target, contact))
        is None
    ):
        if sms_text.body_too_long(body):
            # Rejected with a hint, never truncated, and never stored.
            reply(sms_text.too_long_hint())
            return "too_long"
        bid = externals_store.ensure_sms_backend(contact)
        result = routing.send(
            sender_uid=contact.uid,
            recipient_alias=target.alias,
            kind="text",
            body=body,
            origin_backend_kind="sms",
            origin_backend_id=bid,
            wire_id=sid,
        )
        if result.rejected:
            return f"rejected_{result.rejected[0].reason}"
        if not result.messages:
            return "duplicate"
        if via is not None:
            record_channel(contact.uid, bid, target.uid, via, voice_conv)
        return "delivered"

    # Held: no contact, no edge, or the member's numbers rule is `none`.
    if held_sms_store.count_held(family_id, from_number, target.uid) >= held_sms_store.HELD_CAP:
        return "held_cap"
    if not held_sms_store.create(
        sid, family_id=family_id, to_uid=target.uid, from_phone=from_number, body=raw_body,
        via=via, conv=voice_conv,
    ):
        return "duplicate"
    alert_id = alerts_module.sms_held_upsert(family_id, target, from_number, raw_body)
    held_sms_store.set_alert_id([sid], alert_id)
    return "held"
