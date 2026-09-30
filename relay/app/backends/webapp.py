"""`webapp` backend -- docs/SERVER_PLAN.md §6.3.

`deliver()` is nearly free: the message document `routing.py` already wrote
*is* the delivery (the browser's Firestore listener sees it immediately),
so all this does is mark the delivery `sent` and best-effort push an FCM
data message to the recipient's registered tokens. FCM itself is injected
as a small `FCMClient` protocol so tests (and dev, which has no real FCM
credentials) can substitute a no-op/fake rather than this module reaching
for real `firebase_admin.messaging` (docs/SERVER_PLAN.md §5.9: "FCM sends
go through a stub `messaging` client").

Delivery goes `read` separately, via `POST /api/conversations/{alias}/
messages/{id}/read` (`app/routers/conversations.py`), when the browser
reports the thread viewed -- not from this module.
"""

from __future__ import annotations

import logging
from typing import Protocol

from pydantic import BaseModel, ConfigDict

from app.backends.base import DeliverResult, LinkStep
from app.store import conversations as conversations_store
from app.store import messages as messages_store
from app.store import push_tokens as push_tokens_store
from app.store import users as users_store
from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, Message
from app.store.users import User

logger = logging.getLogger("relay.backends.webapp")

PREVIEW_MAX_CHARS = 120


class WebappConfig(BaseModel):
    """Every user gets an implicit `webapp` backend at creation
    (`app/store/users.py`'s `create_user`); there is nothing for a user to
    configure."""

    model_config = ConfigDict(extra="ignore")


class FCMClient(Protocol):
    """Stubbable stand-in for `firebase_admin.messaging`."""

    def send_data(self, tokens: list[str], data: dict[str, str]) -> None: ...


class NullFCMClient:
    """Default FCM client: does nothing. Never touches real Firebase Cloud
    Messaging -- no credentials for it are configured in dev/test."""

    def send_data(self, tokens: list[str], data: dict[str, str]) -> None:
        return None


class WebappBackend:
    kind = "webapp"
    config_schema = WebappConfig

    def __init__(self, fcm_client: FCMClient | None = None) -> None:
        self._fcm = fcm_client or NullFCMClient()

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult:
        messages_store.mark_delivery_sent_if_queued(msg.id, backend.id)
        try:
            tokens = push_tokens_store.list_tokens(msg.recipientUid)
            if tokens:
                # docs/SERVER_PLAN.md §7.6 / docs/GROUP_CHAT_DESIGN.md §6:
                # the payload contract both the relay and the web service
                # worker (`onBackgroundMessage`) honour. FCM data maps are
                # string-only, so every value here is already a `str`.
                is_group = msg.groupMsgId is not None
                # docs/GROUP_CHAT_DESIGN.md §2: `senderAlias` is denormalised
                # onto a group copy specifically so this never needs a
                # `users/{uid}` read per delivery; a DM copy never sets it
                # (it's `None`), so the lookup below is still the only
                # source for a DM push, same as before this field existed.
                # `sender_alias` falls back to the raw uid on a lookup miss
                # (e.g. a deleted sender) rather than failing the push
                # outright -- this send is best-effort (see the `except
                # Exception` below).
                if is_group and msg.senderAlias is not None:
                    sender_alias = msg.senderAlias
                else:
                    sender = users_store.get_user(msg.senderUid)
                    sender_alias = sender.alias if sender is not None else msg.senderUid

                # §6: for a group copy, `title` = the group name and `url`
                # names the group's own alias, not the sender's -- the
                # thread this push should open is the group, not a DM with
                # whoever happened to send this particular message. A DM
                # copy is unaffected: both still key off `sender_alias`,
                # byte-identical to before groups existed.
                if is_group:
                    conv = conversations_store.get_conversation(msg.convKey)
                    title = conv.name if conv is not None and conv.name else sender_alias
                    chat_alias = conv.alias if conv is not None and conv.alias else sender_alias
                else:
                    title = sender_alias
                    chat_alias = sender_alias

                data = {
                    "kind": "message",
                    "convKey": msg.convKey,
                    "id": msg.id,
                    "senderUid": msg.senderUid,
                    "senderAlias": sender_alias,
                    "title": title,
                    "body": (msg.body or "")[:PREVIEW_MAX_CHARS],
                    "url": f"/chat/{chat_alias}",
                }
                if msg.groupMsgId is not None:
                    # §6: "One push per member copy, collapse key =
                    # groupMsgId." `FCMClient.send_data`'s `data` map is the
                    # only channel this seam has to the client -- there is no
                    # separate FCM-protocol collapse-key parameter wired up
                    # here (`app/backends/fcm.py`'s `send_data` takes a plain
                    # string-value data map, nothing else) -- so the
                    # collapse key travels as a data field the service
                    # worker can key its own notification tag on. Absent
                    # entirely on a DM push, same as `groupMsgId` is absent
                    # on a DM message.
                    data["groupMsgId"] = msg.groupMsgId
                self._fcm.send_data(tokens, data)
        except Exception:
            # FCM is best-effort: the message is already visible to the
            # browser's Firestore listener regardless of whether the push
            # notification itself succeeds, so a push failure must not fail
            # the delivery.
            logger.exception("FCM push failed for message %s", msg.id)
        return DeliverResult(ok=True, state="sent")

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None:
        return None

    def complete_link(self, backend: BackendRow, proof: str) -> bool:
        return True

    def render_state(self, delivery: Delivery) -> str:
        return delivery.state


# ---------------------------------------------------------------------------
# alert push -- docs/FAMILIES_DESIGN.md §6 "Push"
# ---------------------------------------------------------------------------

ALERT_URL = "/family/alerts"

_ALERT_TITLES = {
    "new_conversation": lambda a: f"New chat: @{a['subjectAlias']} ↔ @{a.get('peerAlias')}",
    "sms_unknown": lambda a: f"Text from an unknown number for @{a['subjectAlias']}",
    "contact_request": lambda a: f"Contact request from @{a['subjectAlias']}'s pager",
}


def push_alert(family_id: str, alert: dict, fcm_client: FCMClient | None = None) -> None:
    """docs/FAMILIES_DESIGN.md §6 "Push": `alerts.py` (task 4.1, not yet
    built) calls this once after it creates a `families/{family_id}/
    alerts/{id}` doc. Every `users/{uid}` with `familyId == family_id` and
    `role == 'admin'` (never `super`, never `member` -- exactly the family's
    admins) whose `notify.alerts` is not `False` gets one FCM data message
    per registered push token, same client/dead-token handling as
    `WebappBackend.deliver`'s message push (`app/backends/fcm.py`'s
    `FirebaseFCMClient` deletes a token the first time FCM reports it dead;
    this module never touches that logic directly, only through
    `FCMClient.send_data`).

    `fcm_client` mirrors `WebappBackend.__init__`'s own injection seam
    (default `NullFCMClient`, a real `FirebaseFCMClient` in prod when
    `PUSH_BACKEND=fcm`) -- this function has no instance to carry one on,
    so callers (`alerts.py`, tests) pass it explicitly instead.

    Best-effort per admin: one admin's push failure (or a `send_data` that
    raises) must not stop the rest from being notified, mirroring
    `WebappBackend.deliver`'s own `except Exception` around the FCM call.
    """
    fcm = fcm_client or NullFCMClient()
    title_fn = _ALERT_TITLES.get(alert["kind"])
    if title_fn is None:
        raise ValueError(f"unknown alert kind: {alert['kind']!r}")
    data = {
        "kind": "alert",
        "alertKind": str(alert["kind"]),
        "id": str(alert["id"]),
        "title": title_fn(alert),
        "body": (alert.get("preview") or "")[:PREVIEW_MAX_CHARS],
        "url": ALERT_URL,
    }
    for user in users_store.list_users():
        if user.familyId != family_id or user.role != "admin":
            continue
        if user.notify.alerts is False:
            continue
        try:
            tokens = push_tokens_store.list_tokens(user.uid)
            if tokens:
                fcm.send_data(tokens, data)
        except Exception:
            # Same rationale as `WebappBackend.deliver`: the alert doc
            # already exists regardless of whether this admin's push
            # succeeds, so a push failure for one admin must not raise out
            # of `push_alert` and skip the rest.
            logger.exception("FCM alert push failed for user %s", user.uid)
