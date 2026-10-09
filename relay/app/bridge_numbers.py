"""Applying a bridge's accepted numbers to its owner -- docs/BRIDGE_PHONE_DESIGN.md
decision 3 and O1 (revised 9 Oct 2026).

The member's `smsNumber` is the bridge's `simNumber` if present, else its
`voiceNumber`. Shared by pair, reassign, accept-sim and the number PATCH so
the `SmsNumberTaken` handling is written once.
"""

from __future__ import annotations

import logging

from app import book
from app.broker import BrokerClient
from app.store import bridges as bridges_store
from app.store import users as users_store

logger = logging.getLogger("relay.bridge")


def desired_sms_number(bridge: bridges_store.Bridge) -> str | None:
    return bridge.simNumber or bridge.voiceNumber


def caps_for(bridge_sms_cap: bool, gchat: bool, sim: str | None, voice: str | None):
    """`caps.sms` = a SIM is present and the phone says it can send;
    `caps.gvoice` = a Voice number is present (O1 revised)."""
    return bridges_store.BridgeCaps(sms=bool(bridge_sms_cap and sim), gchat=gchat, gvoice=bool(voice))


def apply_numbers(bridge: bridges_store.Bridge, broker: BrokerClient) -> bool:
    """Sets the owner's `smsNumber` to the bridge's number when it differs,
    then re-derives the owner's SMS contacts and book. `SmsNumberTaken` is
    recorded in `status.error` and changes nothing else. Returns True when
    the owner holds the number afterwards (or the bridge has none)."""
    owner = users_store.get_user(bridge.ownerUid)
    if owner is None:
        return False
    number = desired_sms_number(bridge)
    if number is None:
        return True
    if owner.smsNumber != number:
        try:
            users_store.set_sms_number(owner.uid, number)
        except users_store.SmsNumberTaken as exc:
            holder = users_store.get_user(exc.holder_uid)
            label = f"@{holder.alias}" if holder else exc.holder_uid
            bridges_store.set_error(bridge.id, f"sim number belongs to {label}")
            logger.info("bridge sim taken bridge=%s holder=%s", bridge.id, exc.holder_uid)
            return False
    book.rederive_sms_contacts(owner.uid, broker)
    return True


def release_numbers(bridge: bridges_store.Bridge, broker: BrokerClient) -> None:
    """Clears the owner's `smsNumber` if it is one of this bridge's numbers
    (decision 2 unpair / reassign), then re-derives."""
    owner = users_store.get_user(bridge.ownerUid)
    if owner is None:
        return
    if owner.smsNumber and owner.smsNumber in {bridge.simNumber, bridge.voiceNumber}:
        users_store.set_sms_number(owner.uid, None)
        book.bump_and_push({owner.uid}, broker, reason="sms_number")
    book.rederive_sms_contacts(owner.uid, broker)
