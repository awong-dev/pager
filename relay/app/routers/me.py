"""`/api/me/*` -- docs/SERVER_PLAN.md §5.1: `GET /api/me`, a user's own
backends, and FCM push-token registration. Every route requires a
registered caller (`app.auth.require_user`); there is no admin-only surface
here (see `app/routers/admin.py` for that).

`POST /api/me/backends/{id}/verify {code}` completes a backend's link flow
(§5.1's API surface table). `POST /api/me/backends` calls the new backend's
`start_link()` right after creating it (§6.1: "e.g. send a code"); only
`gchat` has a real link flow, and `pager`/`webapp` no-op. There is no `sms`
self-service backend: an `sms` row belongs to an SMS contact (an external)
and is made by the relay when a family admin approves the number
(docs/RELAY_SMS_DESIGN.md decision 3), so `kind: "sms"` here is a 422.

`POST /api/me/backends` forces `enabled=False` at creation for any kind with
a link/verify flow (`gchat`), regardless of what the request body asked for.
`app/routing.py`'s fan-out only ever checks `backend.enabled`, never
`verifiedAt` (deliberately -- `pager`/`webapp` have no verify step at all, so
gating fan-out on `verifiedAt` would break them). A gchat backend defaulting
to `enabled=True` at creation would deliver to an unverified Chat space on
the very next message, with the verify flow never actually exercised.
`verify_backend` below flips `enabled=True` on success, so this is purely
"not enabled until proven", not "gchat can never be enabled".

**`POST /api/me/backends` is rate limited per user** -- the
highest-priority rate limit in the relay, since each call can trigger a
real `start_link()` (a Chat link-code issue, docs/SERVER_PLAN.md §6.5).
`app/store/rate_limits.py`'s Firestore-backed fixed-window
counter, keyed `"backends:{uid}"`, `RATE_LIMIT_BACKEND_CREATE_LIMIT` calls
per `RATE_LIMIT_BACKEND_CREATE_WINDOW_S` seconds (defaults: 10 per hour --
generous for a real user configuring a couple of backends, tight enough to
bound abuse). Read fresh from the environment on every call, the same
per-call `os.environ.get(...)` pattern this module's siblings already use
(`app/backends/gchat.py`'s `gchat_audience()`, `app/jobs.py`'s
`_sweep_batch_size()`) -- not threaded through `app.config.Settings`.
"""

from __future__ import annotations

import logging
import os
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from google.cloud.firestore import FieldFilter
from pydantic import BaseModel

from app.auth import AuthedUser, principal_for, require_user, set_claims
from app.backends.base import Backend as BackendImpl
from app.db.firestore import get_db
from app.store import backends as backends_store
from app.store import push_tokens as push_tokens_store
from app.store import rate_limits as rate_limits_store
from app.store import users as users_store
from app.store.backends import Backend, BackendKind
from app.store.users import User

logger = logging.getLogger("relay.routers.me")

router = APIRouter()

# H2: backend kinds with a link/verify flow -- `create_backend` forces
# `enabled=False` at creation for these regardless of the request body,
# `verify_backend` is what flips it back to `True` on success. `pager`/
# `webapp` have no such flow and keep their existing default.
LINK_FLOW_KINDS: frozenset[str] = frozenset({"gchat"})

DEFAULT_BACKEND_CREATE_LIMIT = 10
DEFAULT_BACKEND_CREATE_WINDOW_S = 3600


def _backend_create_rate_limit() -> tuple[int, int]:
    limit = int(os.environ.get("RATE_LIMIT_BACKEND_CREATE_LIMIT", str(DEFAULT_BACKEND_CREATE_LIMIT)))
    window_s = int(
        os.environ.get("RATE_LIMIT_BACKEND_CREATE_WINDOW_S", str(DEFAULT_BACKEND_CREATE_WINDOW_S))
    )
    return limit, window_s


def require_backend_create_rate_limit(
    authed: Annotated[AuthedUser, Depends(require_user)],
) -> AuthedUser:
    """`POST /api/me/backends`-only dependency (a drop-in replacement for a
    plain `Depends(require_user)` on that one route, since it also returns
    the `AuthedUser` every other backend route needs) -- see this module's
    docstring."""
    limit, window_s = _backend_create_rate_limit()
    if not rate_limits_store.check_and_increment(
        f"backends:{authed.uid}", limit=limit, window_s=window_s
    ):
        raise HTTPException(
            status_code=429, detail="too many backend creation attempts; try again later"
        )
    return authed


def get_backend_registry(request: Request) -> dict[str, BackendImpl]:
    return request.app.state.backend_registry


class MeResponse(BaseModel):
    user: User
    role: str
    claims: dict
    # docs/FAMILIES_DESIGN.md §1 decision 2: true when the token's `role`/
    # `fam` claims disagree with the `users/{uid}` doc -- the web forces
    # `getIdToken(true)` and refetches when it sees this. Re-issued
    # server-side below so the *next* forced refresh already carries the
    # right claims, rather than waiting for whatever next wrote them.
    claimsStale: bool = False


@router.get("/api/me")
def get_me(authed: Annotated[AuthedUser, Depends(require_user)]) -> MeResponse:
    principal = principal_for(authed)
    stale = principal.role != authed.user.role or (
        principal.family_id or ""
    ) != (authed.user.familyId or "")
    if stale:
        set_claims(authed.uid, authed.user.role, authed.user.familyId)
    return MeResponse(
        user=authed.user, role=authed.user.role, claims=authed.claims, claimsStale=stale
    )


class NotifyPatch(BaseModel):
    alerts: bool


class PatchMeRequest(BaseModel):
    notify: NotifyPatch | None = None


@router.patch("/api/me")
def patch_me(req: PatchMeRequest, authed: Annotated[AuthedUser, Depends(require_user)]) -> User:
    """docs/FAMILIES_DESIGN.md §5.6 / task 4.2: today's only field is
    `notify.alerts` (whether this admin's family alerts are pushed,
    `app/backends/webapp.py`'s `push_alert`); a member can toggle it too,
    it just has no effect since `push_alert` only ever considers admins."""
    notify_alerts = req.notify.alerts if req.notify is not None else None
    return users_store.update_user(authed.uid, notify_alerts=notify_alerts)


# ---------------------------------------------------------------------------
# directory
# ---------------------------------------------------------------------------

# docs/FAMILIES_TASKS.md 1.5: "Cap 500 entries, deduplicated by uid, sorted
# by alias."
DIRECTORY_MAX_ENTRIES = 500


class DirectoryEntry(BaseModel):
    uid: str
    alias: str
    displayName: str
    kind: str
    familyId: str | None
    role: str
    # Only set for `kind == 'external'`: the contact's `users.phone`.
    phone: str | None = None


@router.get("/api/directory")
def get_directory(
    authed: Annotated[AuthedUser, Depends(require_user)],
    # docs/FAMILIES_TASKS.md 3.2 addition (a): only consulted for a `super`
    # caller (an `admin`'s own scope is always their own `familyId`, a
    # `member`'s never has one) -- same `?family=` shape `/api/family/*`
    # already uses, but this route stays open to every registered caller,
    # not `require_family_admin`-gated, so a bare `member` passing it is
    # simply ignored rather than 403ed.
    family: Annotated[str | None, Query()] = None,
) -> dict:
    """docs/FAMILIES_DESIGN.md §4 `GET /api/directory`: the aliases the
    caller may resolve even though `firestore.rules` only lets a member
    read `users/{uid}` docs inside their own family (1.4) -- edge peers in
    another family and conversation participants (including externals) are
    otherwise invisible to `useDirectory()` (§5.1). Union of: every user
    sharing the caller's `familyId` (skipped when the caller has none);
    both ends of every `allow` edge the caller is a party to; every uid
    appearing in the `uids` of a conversation the caller belongs to; and,
    for an `admin` (own family) or a `super` naming one via `?family=`,
    both ends of *every* `allow` edge tagged with that family -- addition
    (a) -- so a family admin sees an external a member approved even
    though the admin holds no edge of their own to it.
    """
    me = authed.uid
    db = get_db()
    uids: set[str] = set()

    family_id = authed.user.familyId
    if family_id:
        for snap in db.collection("users").where(
            filter=FieldFilter("familyId", "==", family_id)
        ).stream():
            uids.add(snap.id)

    for snap in db.collection("allow").where(filter=FieldFilter("fromUid", "==", me)).stream():
        to_uid = (snap.to_dict() or {}).get("toUid")
        if to_uid:
            uids.add(to_uid)
    for snap in db.collection("allow").where(filter=FieldFilter("toUid", "==", me)).stream():
        from_uid = (snap.to_dict() or {}).get("fromUid")
        if from_uid:
            uids.add(from_uid)

    for snap in db.collection("conversations").where(
        filter=FieldFilter("uids", "array_contains", me)
    ).stream():
        for uid in (snap.to_dict() or {}).get("uids") or []:
            uids.add(uid)

    admin_scope_family_id = (
        family
        if (authed.user.role == "super" and family)
        else (family_id if authed.user.role == "admin" else None)
    )
    if admin_scope_family_id:
        for snap in db.collection("allow").where(
            filter=FieldFilter("familyIds", "array_contains", admin_scope_family_id)
        ).stream():
            data = snap.to_dict() or {}
            if data.get("fromUid"):
                uids.add(data["fromUid"])
            if data.get("toUid"):
                uids.add(data["toUid"])

    entries: list[DirectoryEntry] = []
    for uid in uids:
        user = users_store.get_user(uid)
        if user is None:
            continue
        phone = user.phone if user.kind == "external" else None
        entries.append(
            DirectoryEntry(
                uid=user.uid,
                alias=user.alias,
                displayName=user.displayName,
                kind=user.kind,
                familyId=user.familyId,
                role=user.role,
                phone=phone,
            )
        )

    entries.sort(key=lambda e: e.alias)
    # `phone` is `str | None = None` on the model so the field always
    # exists for `model_dump()`'s benefit above, but the web's `DirectoryEntry`
    # declares it `phone?: string` (present only for externals, per this
    # task) -- drop the key rather than serialize a `null` for every
    # person, since `familyId`'s own `null` (an external's, or a person
    # with none yet) is meaningful and must stay.
    out: list[dict] = []
    for entry in entries[:DIRECTORY_MAX_ENTRIES]:
        data = entry.model_dump()
        if data["phone"] is None:
            del data["phone"]
        out.append(data)
    return {"entries": out}


# ---------------------------------------------------------------------------
# backends
# ---------------------------------------------------------------------------


class CreateBackendRequest(BaseModel):
    kind: BackendKind
    config: dict = {}
    enabled: bool = True


class PatchBackendRequest(BaseModel):
    config: dict | None = None
    enabled: bool | None = None


@router.get("/api/me/backends")
def list_backends(authed: Annotated[AuthedUser, Depends(require_user)]) -> list[Backend]:
    existing = backends_store.list_backends(authed.uid)
    if not any(b.kind == "webapp" for b in existing):
        # Every user gets an implicit `webapp` backend at creation
        # (`app/store/users.py`'s `create_user`) -- this is a defensive
        # backfill, not the primary mechanism, so a row created before that
        # existed (or by a test that constructs `users/{uid}` directly)
        # still lists one.
        backends_store.create_backend(authed.uid, kind="webapp", config={}, enabled=True)
        existing = backends_store.list_backends(authed.uid)
    return existing


@router.post("/api/me/backends")
def create_backend(
    req: CreateBackendRequest,
    authed: Annotated[AuthedUser, Depends(require_backend_create_rate_limit)],
    registry: Annotated[dict[str, BackendImpl], Depends(get_backend_registry)],
) -> Backend:
    if req.kind == "sms":
        raise HTTPException(
            status_code=422, detail="sms backends are managed by the relay, not self-service"
        )
    if req.kind == "pager":
        # Pager backends are provisioned by `POST /api/admin/devices`
        # (docs/SERVER_PLAN.md §5.5) -- a user cannot self-issue one, since
        # its `config.deviceId` has to name a real device this user owns.
        raise HTTPException(status_code=400, detail="pager backends are admin-provisioned")
    config = req.config
    # H2: never trust the request body's `enabled` for a kind with a link/
    # verify flow -- see this module's docstring.
    enabled = False if req.kind in LINK_FLOW_KINDS else req.enabled
    created = backends_store.create_backend(
        authed.uid, kind=req.kind, config=config, enabled=enabled
    )
    # §6.1: "start_link(user, backend) -- e.g. send a code". Only gchat
    # implements it for real (it generates and stores a link code the user
    # types back at the Chat app) -- pager/
    # webapp's `start_link` both return None and touch nothing. Best-effort:
    # a `start_link` failure (e.g. Chat unreachable) must not fail backend
    # *creation* -- the row already exists and can be retried (the web app's
    # "Verify" dialog has no separate "resend" affordance yet, a known
    # gap).
    impl = registry.get(req.kind)
    if impl is not None:
        try:
            impl.start_link(authed.user, created)
        except Exception:
            logger.exception("start_link failed for new %s backend %s", req.kind, created.id)
    refreshed = backends_store.get_backend(authed.uid, created.id)
    return refreshed if refreshed is not None else created


class VerifyBackendRequest(BaseModel):
    code: str


@router.post("/api/me/backends/{bid}/verify")
def verify_backend(
    bid: str,
    req: VerifyBackendRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
    registry: Annotated[dict[str, BackendImpl], Depends(get_backend_registry)],
) -> Backend:
    row = backends_store.get_backend(authed.uid, bid)
    if row is None:
        raise HTTPException(status_code=404, detail="no such backend")
    impl = registry.get(row.kind)
    if impl is None or not impl.complete_link(row, req.code):
        raise HTTPException(status_code=400, detail="invalid or expired code")
    updates: dict[str, object] = {"verified": True, "enabled": True}
    updated = backends_store.update_backend(authed.uid, bid, **updates)
    return updated


@router.patch("/api/me/backends/{bid}")
def patch_backend(
    bid: str, req: PatchBackendRequest, authed: Annotated[AuthedUser, Depends(require_user)]
) -> Backend:
    row = backends_store.get_backend(authed.uid, bid)
    if row is None:
        raise HTTPException(status_code=404, detail="no such backend")

    return backends_store.update_backend(authed.uid, bid, config=req.config, enabled=req.enabled)


@router.delete("/api/me/backends/{bid}")
def delete_backend(
    bid: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> dict[str, bool]:
    row = backends_store.get_backend(authed.uid, bid)
    if row is None:
        raise HTTPException(status_code=404, detail="no such backend")
    backends_store.delete_backend(authed.uid, bid)
    return {"ok": True}


# ---------------------------------------------------------------------------
# push tokens
# ---------------------------------------------------------------------------


class PushTokenRequest(BaseModel):
    token: str


@router.post("/api/me/push-tokens")
def add_push_token(
    req: PushTokenRequest, authed: Annotated[AuthedUser, Depends(require_user)]
) -> dict[str, bool]:
    push_tokens_store.add_token(authed.uid, req.token)
    return {"ok": True}


@router.delete("/api/me/push-tokens/{token}")
def remove_push_token(
    token: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> dict[str, bool]:
    push_tokens_store.remove_token(authed.uid, token)
    return {"ok": True}
