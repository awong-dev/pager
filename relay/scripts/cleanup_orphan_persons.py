"""Delete family-less `person` users (docs/CONTACT_REQ_DESIGN.md decision 3).

Before the contact-request rework, approving a pager "Add contact" request
with `mode:"create"` made a Firebase Auth user plus a `kind:"person"` user
with no `familyId`. Nobody can sign in as them and the intent (an SMS
contact) is one new request, so they are deleted, not attached to a family.

Dry run by default: prints every orphan and what deleting it would remove.
`--apply --uid U [--uid V ...]` deletes exactly the named orphans (a uid that
is not an orphan is refused for the whole run, exit 2, before anything is
touched). Run from `relay/`:

    .venv/bin/python -m scripts.cleanup_orphan_persons
    .venv/bin/python -m scripts.cleanup_orphan_persons --apply --uid UID

Per orphan, `--apply` deletes: `allow` edges in both directions
(recomputing `locatableBy` for the former peers), `users/{u}/backends/*` and
`users/{u}/book/*`, the `aliases` doc and
`users` doc, the Firebase Auth user, the approved `contactRequests` rows that
produced it and their `contact_request` alerts. Messages stay as history.

It does NOT publish to a broker (that would need live broker credentials):
it bumps `bookVersion` on every former edge holder's devices, so the next
`/status` from each pager republishes its book (PROTOCOL.md §5.3). The
output says "nudge pending; the next /status republishes".

Targets whatever Firestore/Auth `app.db.firestore` is configured for
(`FIRESTORE_EMULATOR_HOST` etc., else the ambient Google credentials): check
the environment before `--apply`.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field

from firebase_admin import auth as fb_auth

from app.db.firestore import get_app, get_db
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import devices as devices_store
from app.store import families as families_store
from app.store import users as users_store


@dataclass
class Plan:
    user: users_store.User
    edges: list[allow_store.AllowEdge] = field(default_factory=list)
    backends: list[backends_store.Backend] = field(default_factory=list)
    phones: list[str] = field(default_factory=list)
    requests: list[contacts_store.ContactRequest] = field(default_factory=list)
    alerts: list[tuple[str, str]] = field(default_factory=list)  # (familyId, alertId)

    @property
    def former_holders(self) -> set[str]:
        """Everyone with an edge to the orphan: their books listed it."""
        return {e.fromUid for e in self.edges if e.toUid == self.user.uid and e.fromUid != self.user.uid}

    @property
    def former_peers(self) -> set[str]:
        return {
            e.toUid if e.fromUid == self.user.uid else e.fromUid
            for e in self.edges
            if e.fromUid != e.toUid
        }


def find_orphans() -> list[users_store.User]:
    return [u for u in users_store.list_users() if u.kind == "person" and u.familyId is None]


def build_plan(user: users_store.User) -> Plan:
    uid = user.uid
    plan = Plan(user=user)
    plan.edges = [e for e in allow_store.list_edges() if uid in (e.fromUid, e.toUid)]
    plan.backends = backends_store.list_backends(uid)
    # An orphan made the old way (a phone `contact_req` linked to a person)
    # carries that number as its sign-in `phone`; its approved requests match
    # on it. (The retired sms backend rows are no longer readable.)
    plan.phones = [user.phone] if user.phone else []

    alerts_by_family: dict[str, list[alerts_store.Alert]] = {
        f.id: alerts_store.list_alerts(f.id, "all") for f in families_store.list_families()
    }
    keyed = {
        a.contactRequestKey
        for alerts in alerts_by_family.values()
        for a in alerts
        if a.kind == "contact_request" and a.peerUid == uid and a.contactRequestKey
    }
    for req in contacts_store.list_requests(status="approved"):
        if req.alias == user.alias or (req.phone and req.phone in plan.phones) or req.key in keyed:
            plan.requests.append(req)
    keys = {r.key for r in plan.requests}
    for fid, alerts in alerts_by_family.items():
        for a in alerts:
            if a.kind == "contact_request" and (a.contactRequestKey in keys or a.peerUid == uid):
                plan.alerts.append((fid, a.id))
    return plan


def describe(plan: Plan) -> str:
    u = plan.user
    n_in = sum(1 for e in plan.edges if e.toUid == u.uid)
    n_out = sum(1 for e in plan.edges if e.fromUid == u.uid)
    lines = [
        f"{u.uid}  @{u.alias}  {u.displayName!r}  phone={u.phone}",
        f"    edges in/out: {n_in}/{n_out}",
        f"    backends: {[b.kind for b in plan.backends]}  phones: {plan.phones}",
        f"    contactRequests to delete: {[r.key for r in plan.requests]}",
        f"    alerts to delete: {[f'{f}/{a}' for f, a in plan.alerts]}",
    ]
    return "\n".join(lines)


def _delete_subcollection(uid: str, name: str) -> int:
    n = 0
    for snap in get_db().collection("users").document(uid).collection(name).stream():
        snap.reference.delete()
        n += 1
    return n


def apply_plan(plan: Plan) -> set[str]:
    """Deletes one orphan; returns the uids whose books need a nudge."""
    uid = plan.user.uid
    db = get_db()
    for e in plan.edges:
        allow_store.delete_edge(e.fromUid, e.toUid)
    for peer in plan.former_peers:
        allow_store.recompute_locatable_by_for_owner(peer)

    _delete_subcollection(uid, "backends")
    _delete_subcollection(uid, "book")

    users_store.delete_user(uid)
    try:
        fb_auth.delete_user(uid)
    except fb_auth.UserNotFoundError:
        pass

    for fid, alert_id in plan.alerts:
        db.collection("families").document(fid).collection("alerts").document(alert_id).delete()
    for req in plan.requests:
        db.collection("contactRequests").document(req.key).delete()
    return plan.former_holders


def nudge(holders: set[str]) -> int:
    """`bookVersion` bump only -- no broker publish; see module docstring."""
    n = 0
    for owner in sorted(holders):
        for device in devices_store.list_devices(owner_uid=owner):
            contacts_store.bump_book_version(device.id)
            n += 1
    return n


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--apply", action="store_true", help="delete (default: dry run)")
    parser.add_argument("--uid", action="append", default=[], help="orphan uid to delete (repeatable)")
    args = parser.parse_args(argv)
    if args.apply and not args.uid:
        parser.error("--apply requires at least one --uid")

    get_app()
    orphans = {u.uid: u for u in find_orphans()}

    if not args.apply:
        print(f"{len(orphans)} family-less person user(s) (dry run, nothing changed)")
        for user in sorted(orphans.values(), key=lambda u: u.uid):
            print(describe(build_plan(user)))
        return 0

    refused = [uid for uid in args.uid if uid not in orphans]
    if refused:
        for uid in refused:
            print(f"refusing {uid}: not a family-less person user; nothing was deleted")
        return 2

    holders: set[str] = set()
    for uid in args.uid:
        plan = build_plan(orphans[uid])
        print("deleting " + describe(plan))
        holders |= apply_plan(plan)
    holders -= set(args.uid)
    devices = nudge(holders)
    print(f"bumped bookVersion on {devices} device(s); nudge pending; the next /status republishes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
