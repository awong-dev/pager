"""Bearer auth for `/bridge/*` -- docs/BRIDGE_PHONE_DESIGN.md decision 2.

The token is `<bridgeId>.<secret>`; only `sha256(token)` is stored
(`bridges.tokenHash`). Every failure is the same 401 body.
"""

from __future__ import annotations

import hashlib
import hmac
import secrets

from fastapi import HTTPException, Request

from app.store import bridges as bridges_store


def _hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def mint_token(bridge_id: str) -> tuple[str, str]:
    """`(token, tokenHash)`; the token is shown to the phone once."""
    token = f"{bridge_id}.{secrets.token_urlsafe(32)}"
    return token, _hash(token)


def verify(token: str) -> bridges_store.Bridge | None:
    bridge_id, sep, _secret = token.partition(".")
    if not sep or not bridge_id:
        return None
    bridge = bridges_store.get(bridge_id)
    if bridge is None or bridge.tokenHash is None:
        return None
    if not hmac.compare_digest(_hash(token).encode(), bridge.tokenHash.encode()):
        return None
    return bridge


def _unauthorized() -> HTTPException:
    return HTTPException(status_code=401, detail="unauthorized")


def require_bridge(request: Request) -> bridges_store.Bridge:
    header = request.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    if scheme.lower() != "bearer" or not token.strip():
        raise _unauthorized()
    bridge = verify(token.strip())
    if bridge is None:
        raise _unauthorized()
    return bridge
