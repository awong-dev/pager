"""Alert writers -- docs/FAMILIES_DESIGN.md §6 "Alert creation";
docs/FAMILIES_TASKS.md 4.1.

Every alert this deployment ever writes goes through this module's `create`
(never `app/store/alerts.py`'s own `create` directly, outside this module
and tests) so the push (`app/backends/webapp.py`'s `push_alert`) can never be
forgotten -- one call site, one guarantee. The three kind-specific helpers
below (`new_conversation`, `sms_unknown`, `approval_upsert`) are the only
callers `app/routing.py`, `app/ingest.py` and `app/store/
contacts.py` need: each builds the §3 field shape for its own kind and hands
it to `create`.

**FCM client seam.** `push_alert` (task 4.2) takes an `FCMClient` explicitly
rather than reading one off some shared app state, because this module has
no per-request object to carry one on (unlike `app/backends/webapp.py`'s
`WebappBackend`, constructed once per app with its own `_fcm` instance
attribute). `app/main.py` already builds the real `FirebaseFCMClient` once
at startup when `PUSH_BACKEND=fcm` (for `build_registry`'s `webapp` backend);
`set_fcm_client` below is the same client, handed to this module too, via
one `app.alerts.set_fcm_client(fcm_client)` call added to that same startup
block -- so every alert push in the whole app (routing's new_conversation,
a device's sms_log `sms_unknown`, a device's contact_req) goes through the
one real client, or the default `NullFCMClient` in dev/test."""

from __future__ import annotations

from app.backends.webapp import FCMClient, NullFCMClient
from app.backends.webapp import push_alert as _push_alert
from app.store import alerts as alerts_store
from app.store import held_chat as held_chat_store
from app.store import held_sms as held_sms_store
from app.store import users as users_store
from app.store.users import User

# docs/FAMILIES_DESIGN.md §3: "preview (<=120)".
PREVIEW_MAX_CHARS = 120

_fcm_client: FCMClient = NullFCMClient()


def set_fcm_client(client: FCMClient) -> None:
    """Called once from `app/main.py`'s startup (see module docstring) --
    every `create()` call below pushes through whatever client was last set
    here. Tests that need to assert on a push pass a fake and call this
    before exercising the code under test; every other test gets the
    default `NullFCMClient` (no real FCM credentials needed)."""
    global _fcm_client
    _fcm_client = client


def get_fcm_client() -> FCMClient:
    return _fcm_client


def create(family_id: str, alert: dict) -> str:
    """`app/store/alerts.py`'s `create`, plus the push every alert must get
    (docs/FAMILIES_TASKS.md 4.1: "`alerts.py` must call it after every
    create"). The one place in this codebase that does both."""
    alert_id = alerts_store.create(family_id, alert)
    _push_alert(family_id, {**alert, "id": alert_id}, fcm_client=_fcm_client)
    return alert_id


def _base_alert(kind: str) -> dict:
    return {
        "kind": kind,
        "subjectUid": None,
        "subjectAlias": None,
        "peerUid": None,
        "peerAlias": None,
        "peerName": None,
        "peerPhone": None,
        "preview": "",
        "heldBody": None,
        "convKey": None,
        "contactRequestKey": None,
        "decidedAt": None,
        "decidedBy": None,
    }


def new_conversation(sender: User, recipient: User, conv_key: str) -> str | None:
    """docs/FAMILIES_DESIGN.md §6: fired by `app/routing.py`'s `send()` when
    it creates a DM `conversations` doc, the sender's `policy.out == 'open'`,
    and no `allow/{sender}_{recipient}.message` edge exists (routing.py's
    own docstring/caller decides the *when*; this just writes the alert).
    `None`, a no-op, if `sender` has no family (nothing to attribute the
    alert to -- shouldn't happen for a real `kind == 'person'` sender, but
    defensive against stale data rather than raising).

    `peerUid` is always `recipient.uid` (an external has one too -- decision
    6's minted `x_...` uid), unlike `sms_unknown`'s peer (which may not
    exist yet at alert time): by the time `routing.send` reaches this call
    the recipient is already a resolved, registered uid either way. An
    external recipient's phone number is looked up onto `peerPhone` as well,
    so `/family/alerts`' `new_conversation` card can show it the same way
    `sms_unknown`'s does -- `POST .../approve` (`app/routers/family.py`)
    only needs `peerUid` to write the approving edge, `peerPhone` is display
    only."""
    if sender.familyId is None:
        return None
    peer_phone = recipient.phone if recipient.kind == "external" else None
    alert = _base_alert("new_conversation")
    alert.update(
        {
            "status": "open",
            "subjectUid": sender.uid,
            "subjectAlias": sender.alias,
            "peerUid": recipient.uid,
            "peerAlias": recipient.alias,
            "peerPhone": peer_phone,
            "preview": f"@{sender.alias} started a chat with @{recipient.alias}"[
                :PREVIEW_MAX_CHARS
            ],
            "convKey": conv_key,
        }
    )
    return create(sender.familyId, alert)


def sms_unknown(family_id: str, phone: str, target_uid: str | None, body: str) -> str:
    """docs/FAMILIES_DESIGN.md §4: an open alert for a text the pager
    received (device-direct `sms_log`) from a number the family has no
    contact for. `target_uid` is the family member whose pager got it, `None`
    when none could be determined. Always `open` with no peer and nothing
    held: approving it creates the contact, nothing is re-sent (the relay
    sends no SMS)."""
    subject = users_store.get_user(target_uid) if target_uid is not None else None
    alert = _base_alert("sms_unknown")
    alert.update(
        {
            "status": "open",
            "subjectUid": subject.uid if subject is not None else None,
            "subjectAlias": subject.alias if subject is not None else None,
            "peerUid": None,
            "peerAlias": None,
            "peerName": None,
            "peerPhone": phone,
            "preview": body[:PREVIEW_MAX_CHARS],
            "heldBody": None,
        }
    )
    return create(family_id, alert)


def sms_held_upsert(family_id: str, target: User, phone: str, body: str) -> str:
    """docs/RELAY_SMS_DESIGN.md decision 5: one **open** `sms_unknown` alert
    per `(family, target person, number)`. The first held text creates it
    (`preview` = the text, `heldCount`), later ones update `preview` to the
    newest text, `heldCount` and `updatedAt`; every text pushes FCM to the
    family admins with body `"<phone> -> @alias: <text>"`. `heldCount` is
    counted from the `heldSms` rows (status `held`) -- the caller has already
    written this text's row -- so a crash between row and alert is repaired
    by the next text. Returns the alert id."""
    count = held_sms_store.count_held(family_id, phone, target.uid)
    preview = body[:PREVIEW_MAX_CHARS]
    push_body = f"{phone} \u2192 @{target.alias}: {body}"
    existing = alerts_store.find_open(family_id, "sms_unknown", target.uid, phone)
    if existing is not None:
        alerts_store.update_fields(
            family_id, existing.id, {"preview": preview, "heldCount": count}
        )
        alert_id = existing.id
        pushed = {**existing.model_dump(), "preview": preview}
        _push_alert(
            family_id, {**pushed, "id": alert_id, "pushBody": push_body}, fcm_client=_fcm_client
        )
        return alert_id
    alert = _base_alert("sms_unknown")
    alert.update(
        {
            "status": "open",
            "subjectUid": target.uid,
            "subjectAlias": target.alias,
            "peerPhone": phone,
            "preview": preview,
            "heldBody": None,
            "heldCount": count,
            "updatedAt": None,
            "pushBody": push_body,
        }
    )
    # `pushBody` is push-only: stored alerts do not carry it.
    stored = {k: v for k, v in alert.items() if k != "pushBody"}
    alert_id = alerts_store.create(family_id, stored)
    _push_alert(family_id, {**alert, "id": alert_id}, fcm_client=_fcm_client)
    return alert_id


def approval_upsert(owner: User, peer: User) -> alerts_store.ApprovalOutcome:
    """docs/BOOK_ADD_ANYONE_DESIGN.md D8: the one `contact_request` alert per
    `(owner, peer)`, id `cr_{owner}_{peer}`. The create / no-op-if-open /
    reopen-after-24h decision is `alerts_store.upsert_approval`'s single
    transaction; the FCM push goes out only on `created` and `reopened`, so a
    kid who keeps sending does not keep buzzing the parents. `"declined"`
    (a parent dismissed it less than 24 h ago) writes and pushes nothing.
    A no-op (`"open"`) for an owner without a family."""
    if owner.familyId is None:
        return "open"
    alert = _base_alert("contact_request")
    alert.update(
        {
            "status": "open",
            "subjectUid": owner.uid,
            "subjectAlias": owner.alias,
            "peerUid": peer.uid,
            "peerAlias": peer.alias,
            "peerName": peer.displayName,
            "peerPhone": peer.phone if peer.kind == "external" else None,
            "preview": f"wants to text {peer.displayName}"[:PREVIEW_MAX_CHARS],
        }
    )
    outcome = alerts_store.upsert_approval(owner.familyId, owner.uid, peer.uid, alert)
    if outcome in ("created", "reopened"):
        _push_alert(
            owner.familyId,
            {**alert, "id": alerts_store.approval_alert_id(owner.uid, peer.uid)},
            fcm_client=_fcm_client,
        )
    return outcome


_SOURCE_LABELS = {"whatsapp": "WhatsApp", "gchat": "Google Chat", "gvoice": "Google Voice"}


def chat_held_upsert(family_id: str, target: User, row, body: str, sender_name: str) -> str:
    """docs/BRIDGE_PHONE_DESIGN.md decision 8: one **open** `chat_unknown`
    alert per `(bridge, conversation)`. The first held text creates it
    (`preview` = the text, `heldCount`, the people seen so far); later ones
    update `preview`, `heldCount` and `people` and push again. `heldCount`
    is counted from the `heldChat` rows (status `held`), so a crash between
    row and alert is repaired by the next text. `pushBody` is
    `"<title> (<sender>) -> @alias: <text>"` for a group, `"<Source>: <title> ->
    @alias: <text>"` for a DM. Returns the alert id."""
    count = held_chat_store.count_held(row.id)
    preview = body[:PREVIEW_MAX_CHARS]
    title = row.title or "a conversation"
    if row.isGroup:
        push_body = f"{title} ({sender_name}) \u2192 @{target.alias}: {body}"
    else:
        # BRIDGE_WHATSAPP_LID_DESIGN L7: a DM push names the app, not the sender twice.
        label = _SOURCE_LABELS.get(row.source, "Chat")
        push_body = f"{label}: {row.title or sender_name} \u2192 @{target.alias}: {body}"
    fields = {
        "preview": preview,
        "heldCount": count,
        "people": list(row.people),
        "convTitle": row.title,
        "isGroup": row.isGroup,
    }
    existing = alerts_store.find_open(
        family_id, "chat_unknown", target.uid, None, bridge_conv=(row.bridgeId, row.conversationId)
    )
    if existing is not None:
        alerts_store.update_fields(family_id, existing.id, fields)
        pushed = {**existing.model_dump(), **fields}
        _push_alert(
            family_id,
            {**pushed, "id": existing.id, "pushBody": push_body},
            fcm_client=_fcm_client,
        )
        return existing.id
    alert = _base_alert("chat_unknown")
    alert.update(
        {
            "status": "open",
            "subjectUid": target.uid,
            "subjectAlias": target.alias,
            **fields,
            "bridgeId": row.bridgeId,
            "conversationId": row.conversationId,
            "convRef": row.ref,
            "source": row.source,
            "updatedAt": None,
        }
    )
    alert_id = alerts_store.create(family_id, alert)
    _push_alert(
        family_id, {**alert, "id": alert_id, "pushBody": push_body}, fcm_client=_fcm_client
    )
    return alert_id
