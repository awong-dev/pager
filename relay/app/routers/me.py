"""`/api/me/*` -- docs/SERVER_PLAN.md §5.1: `GET /api/me`, a user's own
backends, and FCM push-token registration. Every route requires a
registered caller (`app.auth.require_user`); there is no admin-only surface
here (see `app/routers/admin.py` for that)."""

from __future__ import annotations

from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel

from app.auth import AuthedUser, require_user
from app.store import backends as backends_store
from app.store import push_tokens as push_tokens_store
from app.store.backends import Backend, BackendKind
from app.store.users import User

router = APIRouter()


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
    req: CreateBackendRequest, authed: Annotated[AuthedUser, Depends(require_user)]
) -> Backend:
    if req.kind == "pager":
        # Pager backends are provisioned by `POST /api/admin/devices`
        # (docs/SERVER_PLAN.md §5.5) -- a user cannot self-issue one, since
        # its `config.deviceId` has to name a real device this user owns.
        raise HTTPException(status_code=400, detail="pager backends are admin-provisioned")
    return backends_store.create_backend(
        authed.uid, kind=req.kind, config=req.config, enabled=req.enabled
    )


@router.patch("/api/me/backends/{bid}")
def patch_backend(
    bid: str, req: PatchBackendRequest, authed: Annotated[AuthedUser, Depends(require_user)]
) -> Backend:
    if backends_store.get_backend(authed.uid, bid) is None:
        raise HTTPException(status_code=404, detail="no such backend")
    return backends_store.update_backend(authed.uid, bid, config=req.config, enabled=req.enabled)


@router.delete("/api/me/backends/{bid}")
def delete_backend(
    bid: str, authed: Annotated[AuthedUser, Depends(require_user)]
) -> dict[str, bool]:
    if backends_store.get_backend(authed.uid, bid) is None:
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
