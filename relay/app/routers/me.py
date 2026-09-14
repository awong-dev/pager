"""`/api/me/*` -- docs/SERVER_PLAN.md §5.1: `GET /api/me`, a user's own
backends, and FCM push-token registration. Every route requires a
registered caller (`app.auth.require_user`); there is no admin-only surface
here (see `app/routers/admin.py` for that).

`POST /api/me/backends/{id}/verify {code}` completes a backend's link flow
(§5.1's API surface table). `POST /api/me/backends` calls the new backend's
`start_link()` right after creating it (§6.1: "e.g. send a code"); only
`sms` and `gchat` have a real link flow, and `pager`/`webapp` no-op.

Three constraints on that surface are load-bearing for `phoneIndex`'s "an
unverified/unlinked claim can never capture another user's inbound traffic"
invariant (`app/store/backends.py`'s module docstring):

- `POST /api/me/backends` forces `enabled=False` at creation
  for any kind with a link/verify flow (`sms`, `gchat`), regardless of what
  the request body asked for. `app/routing.py`'s fan-out only ever checks
  `backend.enabled`, never `verifiedAt` (deliberately -- `pager`/`webapp`
  have no verify step at all, so gating fan-out on `verifiedAt` would break
  them). An sms/gchat backend defaulting to `enabled=True` at creation
  would send real SMS to an unverified, possibly-not-owned phone number on
  the very next message, with the verify flow never actually exercised.
  `verify_backend` below flips `enabled=True` on success, so this is purely
  "not enabled until proven", not "sms/gchat can never be enabled".
- `PATCH /api/me/backends/{bid}` clears `phoneIndex` for the old
  number (and `verifiedAt`) whenever `config.phone` changes, and
  `DELETE /api/me/backends/{bid}` clears `phoneIndex` for the backend's
  current number before deleting the row. Without this, "verify with your
  own number, then PATCH to a victim's number" bypasses the rule above
  outright (the backend is already `enabled` and now `verifiedAt` from the
  first, legitimate verification), and a deleted backend leaves a dangling
  `phoneIndex` entry that still routes inbound SMS somewhere.
- Both routes normalise `config.phone` to E.164
  (`sms_twilio.normalize_e164`) before it is ever stored or used as a
  `phoneIndex` document id; an unnormalised/malformed number both breaks the
  lookup against Twilio's always-E.164 `From` field and, if it contains a
  `/`, would otherwise crash `.document(phone)` with an unhandled 500.

**`POST /api/me/backends` is rate limited per user** -- the
highest-priority rate limit in the relay, since each call can trigger a
real `start_link()` (a real SMS send, docs/SERVER_PLAN.md §6.4).
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

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel

from app.auth import AuthedUser, require_user
from app.backends.base import Backend as BackendImpl
from app.backends.sms_twilio import normalize_e164
from app.store import backends as backends_store
from app.store import push_tokens as push_tokens_store
from app.store import rate_limits as rate_limits_store
from app.store.backends import Backend, BackendKind
from app.store.users import User

logger = logging.getLogger("relay.routers.me")

router = APIRouter()

# H2: backend kinds with a link/verify flow -- `create_backend` forces
# `enabled=False` at creation for these regardless of the request body,
# `verify_backend` is what flips it back to `True` on success. `pager`/
# `webapp` have no such flow and keep their existing default.
LINK_FLOW_KINDS: frozenset[str] = frozenset({"sms", "gchat"})

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


@router.get("/api/me")
def get_me(authed: Annotated[AuthedUser, Depends(require_user)]) -> MeResponse:
    return MeResponse(user=authed.user, role=authed.user.role, claims=authed.claims)


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
    if req.kind == "pager":
        # Pager backends are provisioned by `POST /api/admin/devices`
        # (docs/SERVER_PLAN.md §5.5) -- a user cannot self-issue one, since
        # its `config.deviceId` has to name a real device this user owns.
        raise HTTPException(status_code=400, detail="pager backends are admin-provisioned")
    config = req.config
    if req.kind == "sms" and config.get("phone"):
        try:
            config = {**config, "phone": normalize_e164(str(config["phone"]))}
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from None
    # H2: never trust the request body's `enabled` for a kind with a link/
    # verify flow -- see this module's docstring.
    enabled = False if req.kind in LINK_FLOW_KINDS else req.enabled
    created = backends_store.create_backend(
        authed.uid, kind=req.kind, config=config, enabled=enabled
    )
    # §6.1: "start_link(user, backend) -- e.g. send a code". Only sms/gchat
    # implement it for real (sms texts a verify code; gchat generates and
    # stores a link code the user types back at the Chat app) -- pager/
    # webapp's `start_link` both return None and touch nothing. Best-effort:
    # a `start_link` failure (e.g. Twilio unreachable) must not fail backend
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
    # sms: publish the now-verified phone to `phoneIndex` (`app/store/
    # backends.py`'s module docstring) so the inbound Twilio webhook can map
    # `From` back to this user -- written here, not inside `complete_link`
    # itself, because `complete_link`'s contract (`backends/base.py`) is a
    # pure proof-check that doesn't know this backend's owning uid. The
    # one-time verify code itself (H1) was never in `config` to begin with
    # (it lives in `smsVerifyCodes`, `app/store/backends.py`) -- clear that
    # record here, on success, rather than waiting for its TTL.
    if row.kind == "sms":
        phone = row.config.get("phone")
        if phone:
            try:
                phone = normalize_e164(str(phone))
            except ValueError:
                # Already normalised at creation time (M1) -- this only
                # trips for a pre-fix row, and a verify that can't produce a
                # legal phoneIndex doc id should fail loudly, not 500.
                raise HTTPException(status_code=400, detail="backend has no valid phone") from None
            backends_store.set_phone_index(phone, authed.uid, bid)
        backends_store.clear_sms_verify_code(bid)
    updated = backends_store.update_backend(authed.uid, bid, **updates)
    return updated


@router.patch("/api/me/backends/{bid}")
def patch_backend(
    bid: str, req: PatchBackendRequest, authed: Annotated[AuthedUser, Depends(require_user)]
) -> Backend:
    row = backends_store.get_backend(authed.uid, bid)
    if row is None:
        raise HTTPException(status_code=404, detail="no such backend")

    config = req.config
    verified: bool | None = None
    enabled = req.enabled
    # H3: an sms backend's `config.phone` is the identity `phoneIndex`
    # trusts -- rewriting it after verification, without also clearing the
    # old `phoneIndex` entry, `verifiedAt`, *and* `enabled`, is a second
    # bypass of H2 ("verify with your own number, then PATCH to a victim's
    # number" -- if `enabled` stayed `True`, the backend would still pass
    # `app/routing.py`'s fan-out gate (`enabled` is the *only* thing it
    # checks, deliberately -- see this router's module docstring) and start
    # texting the new, never-verified number on the very next message,
    # exactly what H2 exists to prevent). Requires re-verifying the new
    # number through `POST .../verify` before it can receive anything again,
    # same as a freshly created backend.
    if row.kind == "sms" and config is not None and "phone" in config:
        new_phone_raw = config.get("phone")
        old_phone = row.config.get("phone")
        try:
            new_phone = normalize_e164(str(new_phone_raw)) if new_phone_raw else None
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from None
        if new_phone != old_phone:
            config = {**config, "phone": new_phone} if new_phone else {
                k: v for k, v in config.items() if k != "phone"
            }
            if old_phone:
                backends_store.clear_phone_index(old_phone)
            verified = False
            enabled = False
    return backends_store.update_backend(
        authed.uid, bid, config=config, enabled=enabled, verified=verified
    )


@router.delete("/api/me/backends/{bid}")
def delete_backend(
    bid: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> dict[str, bool]:
    row = backends_store.get_backend(authed.uid, bid)
    if row is None:
        raise HTTPException(status_code=404, detail="no such backend")
    # H3: tear down `phoneIndex` alongside its source -- otherwise a deleted
    # backend leaves a stale entry that still routes inbound SMS to a `bid`
    # that no longer exists.
    if row.kind == "sms":
        phone = row.config.get("phone")
        if phone:
            backends_store.clear_phone_index(phone)
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
