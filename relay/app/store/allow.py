"""`allow/{fromUid}_{toUid}` -- docs/SERVER_PLAN.md §3, §5.4;
docs/FAMILIES_DESIGN.md §1 decision 5, §3.

Directed edges with two independent flags. `message` gates `routing.send()`;
`locate` gates `/locate` and (via `devices.locatableBy`, which
this module recomputes) read access to a device's `locations` subcollection
through `firestore.rules`.

`replace_all` implements §5.1's `PUT /api/admin/allowlist` (replace-all
semantics): the given edges become the entire `allow` collection (or, with
`family_id`, the subset of it touching that family -- docs/FAMILIES_TASKS.md
2.3), and every device owned by a uid whose incoming edges changed gets its
`locatableBy` array rewritten to match.

`family_ids_for`/`check_locate_family` are `docs/FAMILIES_DESIGN.md` §1
decision 5's "location and device data never cross a family": every edge
doc carries `familyIds` (the non-null `familyId`s of both ends), and a
`locate: true` edge whose ends have different (or null) `familyId` is
refused. `replace_all` enforces the refusal (the `PUT /api/admin/allowlist`
surface docs/FAMILIES_TASKS.md 2.3 names); `set_edge` writes `familyIds` on
every edge (so contact-approve and group-create/join's edges carry it too)
but does not itself refuse -- those call sites predate per-user `familyId`
being universal and have their own tests relying on same-uid-pair,
no-family-yet edges succeeding. TODO(orchestrator): once those callers and
their tests are family-aware, route them through `check_locate_family` too,
per docs/FAMILIES_TASKS.md 2.3's "every edge writer ... gets it".
"""

from __future__ import annotations

from dataclasses import dataclass

from pydantic import BaseModel, ConfigDict

from app.db.firestore import get_db
from app.store import devices as devices_store
from app.store import users as users_store


class AllowEdge(BaseModel):
    model_config = ConfigDict(extra="ignore")

    fromUid: str
    toUid: str
    message: bool = False
    locate: bool = False
    # docs/FAMILIES_DESIGN.md §3: the non-null `familyId`s of both ends,
    # deduped and sorted -- lets `firestore.rules`' family-admin read clause
    # stay lookup-free. Empty for edges written before this field existed.
    familyIds: list[str] = []


class LocateCrossFamily(ValueError):
    """Raised by `check_locate_family` for a `locate: true` edge whose ends
    have different (or null) `familyId` -- docs/FAMILIES_DESIGN.md §1
    decision 5. `str(exc) == "locate_cross_family"`, the machine reason
    callers surface in a 400 body."""


def _family_id(uid: str) -> str | None:
    user = users_store.get_user(uid)
    return user.familyId if user is not None else None


def family_ids_for(from_uid: str, to_uid: str) -> list[str]:
    """The non-null `familyId`s of both ends of an edge, deduped and
    sorted -- `allow/{from}_{to}.familyIds`."""
    ids = {fid for fid in (_family_id(from_uid), _family_id(to_uid)) if fid is not None}
    return sorted(ids)


def check_locate_family(from_uid: str, to_uid: str, locate: bool) -> None:
    """Raises `LocateCrossFamily` iff `locate` is true and the two ends
    don't share a (non-null) `familyId` -- docs/FAMILIES_DESIGN.md §1
    decision 5. A no-op for `locate=False`."""
    if not locate:
        return
    from_fam = _family_id(from_uid)
    to_fam = _family_id(to_uid)
    if from_fam is None or to_fam is None or from_fam != to_fam:
        raise LocateCrossFamily("locate_cross_family")


@dataclass(frozen=True, slots=True)
class EdgeInput:
    from_uid: str
    to_uid: str
    message: bool = True
    locate: bool = True


def edge_id(from_uid: str, to_uid: str) -> str:
    return f"{from_uid}_{to_uid}"


def _allow():
    return get_db().collection("allow")


def get_edge(from_uid: str, to_uid: str) -> AllowEdge | None:
    snap = _allow().document(edge_id(from_uid, to_uid)).get()
    if not snap.exists:
        return None
    return AllowEdge.model_validate(snap.to_dict() or {})


def list_edges() -> list[AllowEdge]:
    return [AllowEdge.model_validate(snap.to_dict() or {}) for snap in _allow().stream()]


def is_message_allowed(from_uid: str, to_uid: str) -> bool:
    """docs/SERVER_PLAN.md §5.4: `message` gates `routing.send()`."""
    edge = get_edge(from_uid, to_uid)
    return edge is not None and edge.message


def allowed_recipients(from_uid: str) -> list[str]:
    """Every `toUid` with `allow/{from_uid}_{toUid}.message == true` --
    docs/SERVER_PLAN.md §5.2 step 1's broadcast set ("every user the sender
    is allowed to message")."""
    return sorted(e.toUid for e in list_edges() if e.fromUid == from_uid and e.message)


def set_edge(from_uid: str, to_uid: str, *, message: bool, locate: bool) -> AllowEdge:
    """Upserts a single edge and, if `locate` may have changed, recomputes
    that one recipient's devices' `locatableBy`. Prefer `replace_all` for
    admin bulk edits; this is the narrower single-edge primitive. Writes
    `familyIds` (see module docstring) but does not enforce
    `check_locate_family` -- see the module docstring's TODO."""
    _allow().document(edge_id(from_uid, to_uid)).set(
        {
            "fromUid": from_uid,
            "toUid": to_uid,
            "message": message,
            "locate": locate,
            "familyIds": family_ids_for(from_uid, to_uid),
        }
    )
    _recompute_locatable_by(to_uid)
    edge = get_edge(from_uid, to_uid)
    assert edge is not None
    return edge


def delete_edge(from_uid: str, to_uid: str) -> None:
    _allow().document(edge_id(from_uid, to_uid)).delete()
    _recompute_locatable_by(to_uid)


def recompute_locatable_by_for_owner(uid: str) -> None:
    """Public wrapper around `_recompute_locatable_by`, for callers outside
    this module. `set_edge`/`delete_edge`/
    `replace_all` only ever recompute `locatableBy` on devices that already
    *exist* at the moment an edge changes (`devices_store.list_devices
    (owner_uid=to_uid)`), so a device created *after* its owner's incoming
    `locate` edges were set would otherwise start, and silently stay, at
    `locatableBy: []` -- undetectable by a `get` on that single document
    (which nobody was denied), but exactly what breaks a `list`
    (collection query) that depends on it (`tools/pager_client.py`'s
    `ServerClient.locations()`, docs/PROTOCOL.md §13 / SERVER_PLAN.md §5.6).
    `app/routers/admin.py`'s `POST /api/admin/devices` calls this
    immediately after creating a device, closing that gap without changing
    `set_edge`/`delete_edge`/`replace_all` at all."""
    _recompute_locatable_by(uid)


def _recompute_locatable_by(to_uid: str) -> None:
    """`locatableBy` on every device owned by `to_uid` = every `fromUid`
    with `allow/{fromUid}_{to_uid}.locate == true` -- denormalised onto the
    device so `firestore.rules` needs no join (docs/SERVER_PLAN.md §3)."""
    locators = sorted(
        e.fromUid
        for e in list_edges()
        if e.toUid == to_uid and e.locate
    )
    for device in devices_store.list_devices(owner_uid=to_uid):
        devices_store.set_locatable_by(device.id, locators)


def replace_all(edges: list[EdgeInput], *, family_id: str | None = None) -> list[AllowEdge]:
    """Replace-all, per `PUT /api/admin/allowlist` (docs/FAMILIES_TASKS.md
    2.3): with `family_id=None`, `edges` becomes the *entire* `allow`
    collection (super only, per the route). With `family_id` given, only
    the subset of the existing collection where either end's `familyId` is
    `family_id` is replaced -- edges entirely outside that family are left
    untouched, whether or not they appear in `edges`.

    Every edge in `edges` is checked with `check_locate_family` *before*
    any write happens (raises `LocateCrossFamily`, so a bad entry in a
    large replace-all never partially applies). Deletes any in-scope
    existing edge not present in the new set, upserts every given edge with
    its `familyIds`, and recomputes `locatableBy` for every uid that
    appears as a `toUid` in either the old (in-scope) or the new set."""
    for e in edges:
        check_locate_family(e.from_uid, e.to_uid, e.locate)

    db = get_db()
    all_existing = list_edges()
    if family_id is None:
        existing = {(e.fromUid, e.toUid) for e in all_existing}
    else:
        existing = {
            (e.fromUid, e.toUid)
            for e in all_existing
            if family_id in (_family_id(e.fromUid), _family_id(e.toUid))
        }
    incoming = {(e.from_uid, e.to_uid) for e in edges}

    batch = db.batch()
    for from_uid, to_uid in existing - incoming:
        batch.delete(_allow().document(edge_id(from_uid, to_uid)))
    for e in edges:
        batch.set(
            _allow().document(edge_id(e.from_uid, e.to_uid)),
            {
                "fromUid": e.from_uid,
                "toUid": e.to_uid,
                "message": e.message,
                "locate": e.locate,
                "familyIds": family_ids_for(e.from_uid, e.to_uid),
            },
        )
    batch.commit()

    affected_to_uids = {to for _, to in existing | incoming}
    for to_uid in affected_to_uids:
        _recompute_locatable_by(to_uid)

    return list_edges()
