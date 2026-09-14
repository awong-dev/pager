"""Firebase Auth verification, the registry gate, and the admin claim check
-- docs/SERVER_PLAN.md §5.3.

`require_user` is the one dependency every non-webhook, non-dev API route
depends on: it verifies the bearer token is a real Firebase ID token
(401 if not) and then requires a matching `users/{uid}` document to exist
(403 if not) -- "Firebase Auth will happily create an account for any email
that clicks a link. The relay's dependency rejects any UID with no
`users/{uid}` document, and the admin creates users *by email/phone* ahead
of time." `firestore.rules`'s `registered()` enforces the same rule
independently on the client-read side.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Annotated

from fastapi import Depends, HTTPException
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from firebase_admin import auth as fb_auth

from app.store import users as users_store
from app.store.users import User

bearer_scheme = HTTPBearer(auto_error=False)


@dataclass(frozen=True, slots=True)
class AuthedUser:
    """The verified token plus the matching registry row -- everything a
    route needs, in one dependency result."""

    uid: str
    claims: dict
    user: User


def verify_id_token(token: str) -> dict:
    """Wraps `firebase_admin.auth.verify_id_token`. Raises
    `firebase_admin.auth.InvalidIdTokenError` (or a subclass) on a bad/
    expired/malformed token -- callers translate that to 401, not this
    function's job to know about HTTP."""
    return fb_auth.verify_id_token(token, check_revoked=False)


def require_user(
    creds: Annotated[HTTPAuthorizationCredentials | None, Depends(bearer_scheme)] = None,
) -> AuthedUser:
    if creds is None:
        raise HTTPException(status_code=401, detail="missing bearer token")
    try:
        claims = verify_id_token(creds.credentials)
    except Exception as exc:
        raise HTTPException(status_code=401, detail="invalid or expired ID token") from exc

    uid = claims["uid"]
    user = users_store.get_user(uid)
    if user is None:
        # Registry gate (docs/SERVER_PLAN.md §5.3): a stranger who signs in
        # gets a 403 and no data, even with a perfectly valid Firebase token.
        raise HTTPException(status_code=403, detail="not registered")
    if user.disabled:
        raise HTTPException(status_code=403, detail="account disabled")
    return AuthedUser(uid=uid, claims=claims, user=user)


def require_admin(
    authed: Annotated[AuthedUser, Depends(require_user)],
) -> AuthedUser:
    if not authed.claims.get("admin") and authed.user.role != "admin":
        raise HTTPException(status_code=403, detail="admin only")
    return authed
