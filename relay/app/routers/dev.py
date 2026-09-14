"""`POST /api/dev/token` -- docs/SERVER_PLAN.md §5.1, §5.3, §8: `DEV_MODE=1`
only, mints a Firebase custom token for a given `uid` so the test client
(`tools/pager_client.py`, later phase) can sign in without a real email
link / SMS code flow. The client exchanges the custom token for an ID token
against the Auth emulator's Identity Toolkit REST endpoint itself -- this
endpoint only mints, it never talks to that REST endpoint.

Gated on `Settings.dev_mode`, not on any auth dependency (a real deployment
never sets `DEV_MODE=1`, so there's nothing to authenticate here that
matters); returns 404 when disabled, so its very existence is invisible in
prod, not just 403 information-leaked."""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Request
from firebase_admin import auth as fb_auth
from pydantic import BaseModel, model_validator

from app.store import users as users_store

router = APIRouter()


class DevTokenRequest(BaseModel):
    uid: str | None = None
    # `tools/pager_client.py`'s `login <alias>` has no uid to give (a human
    # signs in by alias, never a uid) -- this endpoint resolves it
    # server-side via the same `aliases/{alias}` lookup `routing.py` uses,
    # rather than inventing a second "resolve alias" endpoint just for the
    # test client.
    alias: str | None = None

    @model_validator(mode="after")
    def _require_one(self) -> DevTokenRequest:
        if not self.uid and not self.alias:
            raise ValueError("either uid or alias is required")
        return self


class DevTokenResponse(BaseModel):
    token: str
    uid: str


@router.post("/api/dev/token")
def mint_dev_token(req: DevTokenRequest, request: Request) -> DevTokenResponse:
    settings = request.app.state.settings
    if not settings.dev_mode:
        raise HTTPException(status_code=404, detail="not found")
    uid = req.uid
    if uid is None:
        assert req.alias is not None
        uid = users_store.get_uid_for_alias(req.alias)
        if uid is None:
            raise HTTPException(status_code=404, detail=f"unknown alias: {req.alias!r}")
    token = fb_auth.create_custom_token(uid)
    return DevTokenResponse(token=token.decode("utf-8"), uid=uid)
