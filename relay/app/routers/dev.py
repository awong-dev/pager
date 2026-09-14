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
from pydantic import BaseModel

router = APIRouter()


class DevTokenRequest(BaseModel):
    uid: str


class DevTokenResponse(BaseModel):
    token: str


@router.post("/api/dev/token")
def mint_dev_token(req: DevTokenRequest, request: Request) -> DevTokenResponse:
    settings = request.app.state.settings
    if not settings.dev_mode:
        raise HTTPException(status_code=404, detail="not found")
    token = fb_auth.create_custom_token(req.uid)
    return DevTokenResponse(token=token.decode("utf-8"))
