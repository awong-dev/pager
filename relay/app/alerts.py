"""Alert writers -- docs/FAMILIES_DESIGN.md §6 "Alert creation";
docs/FAMILIES_TASKS.md 4.1.

Every alert this deployment ever writes goes through this module's `create`
(never `app/store/alerts.py`'s own `create` directly, outside this module
and tests) so the push (`app/backends/webapp.py`'s `push_alert`) can never be
forgotten -- one call site, one guarantee. The three kind-specific helpers
below (`new_conversation`, `sms_unknown`, `contact_request`) are the only
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
from app.store import users as users_store
from app.store.contacts import ContactRequest
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


def contact_request(request: ContactRequest) -> str | None:
    """docs/FAMILIES_DESIGN.md §6 -- called from `app/store/contacts.py`'s
    `create_request` right after a new (not deduped/no-op) `contactRequests/
    {key}` doc is written. `familyId` comes from the device owner's own
    user doc (docs/FAMILIES_TASKS.md 4.1); `None`, a no-op, if that user has
    no family (stale data, shouldn't happen in normal operation)."""
    owner = users_store.get_user(request.ownerUid)
    if owner is None or owner.familyId is None:
        return None
    alert = _base_alert("contact_request")
    alert.update(
        {
            "status": "open",
            "subjectUid": owner.uid,
            "subjectAlias": owner.alias,
            "peerPhone": request.phone,
            "preview": request.name[:PREVIEW_MAX_CHARS],
            "contactRequestKey": request.key,
        }
    )
    # docs/CONTACT_REQ_DESIGN.md decision 2: say who the request resolves to.
    # A phone is only ever an SMS contact (`peerPhone`); an alias resolves to
    # an in-system user.
    peer: User | None = None
    if not request.phone and request.alias:
        peer = users_store.get_user_by_alias(request.alias)
    if peer is not None:
        alert.update({"peerUid": peer.uid, "peerAlias": peer.alias, "peerName": peer.displayName})
    return create(owner.familyId, alert)
