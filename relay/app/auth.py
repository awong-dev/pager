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

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Annotated, Any

import google.auth.jwt as google_jwt
import google.auth.transport.requests as google_requests
import google.oauth2.id_token as google_id_token
from fastapi import Depends, Header, HTTPException, Query
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


@dataclass(frozen=True, slots=True)
class Principal:
    """The token's `role`/`fam` custom claims only -- never the
    `users/{uid}` document's own `role`/`familyId` fields --
    docs/FAMILIES_DESIGN.md §1 decision 2: "Firebase custom claims `role`
    and `fam` are the *only* source of truth for the relay dependency and
    the rules." A token minted before these claims existed (or a bare
    legacy `{"admin": True}` claim, which is never consulted) is `member`
    with no family -- there is no OR-with-the-user-doc fallback."""

    uid: str
    role: str
    family_id: str | None


def principal_for(authed: AuthedUser) -> Principal:
    role = authed.claims.get("role") or "member"
    fam = authed.claims.get("fam") or None
    return Principal(uid=authed.uid, role=role, family_id=fam)


def require_super(
    authed: Annotated[AuthedUser, Depends(require_user)],
) -> AuthedUser:
    """Role `super` by claim only. Returns `AuthedUser` (not `Principal`) so
    the existing `/api/admin/*` call sites of `require_admin` below, which
    only ever read `authed.uid`, keep working unchanged until task 5.1
    re-homes them."""
    if principal_for(authed).role != "super":
        raise HTTPException(status_code=403, detail="super admin only")
    return authed


def require_family_admin(
    authed: Annotated[AuthedUser, Depends(require_user)],
    family: Annotated[str | None, Query()] = None,
    x_family: Annotated[str | None, Header(alias="X-Family")] = None,
) -> tuple[Principal, str]:
    """docs/FAMILIES_DESIGN.md §4: role `admin` acting on their own family,
    or role `super` naming any family via `?family=`/`X-Family`. An `admin`
    naming a *different* family in either place is refused outright -- it
    is never silently redirected back to their own family."""
    principal = principal_for(authed)
    requested = family or x_family
    if principal.role == "admin":
        if principal.family_id is None:
            raise HTTPException(status_code=403, detail="family admin only")
        if requested is not None and requested != principal.family_id:
            raise HTTPException(status_code=403, detail="cannot act on another family")
        return principal, principal.family_id
    if principal.role == "super":
        if not requested:
            raise HTTPException(status_code=400, detail="?family= or X-Family is required")
        return principal, requested
    raise HTTPException(status_code=403, detail="family admin only")


# docs/FAMILIES_TASKS.md 1.2: `require_admin` stays a plain alias of
# `require_super` until task 5.1 re-homes every current `/api/admin/*` route
# under `require_family_admin`/`require_super` -- keeps `app/routers/
# admin.py` and `app/routers/conversations.py`'s existing
# `Depends(require_admin)` call sites working unchanged in the meantime.
require_admin = require_super


def set_claims(uid: str, role: str, family_id: str | None) -> None:
    """docs/FAMILIES_DESIGN.md §1 decision 2: the *only* custom claims the
    relay writes from here on are `role` and `fam` -- consumed by
    `Principal` (task 1.2) and by `firestore.rules`'s `role()`/`fam()`
    helpers (task 1.4). Replaces the old `{"admin": True}` claim (written by
    `app/routers/admin.py`'s now-removed `_set_admin_claim`), which is never
    written again anywhere. `family_id=None` writes `fam: ""`, matching
    `fam()`'s own `request.auth.token.get('fam', '')` default for a claim
    that was never set."""
    fb_auth.set_custom_user_claims(uid, {"role": role, "fam": family_id or ""})


# ---------------------------------------------------------------------------
# `/internal/*` OIDC verification -- docs/SERVER_PLAN.md §5.1, §9.2
# ("/internal/* additionally requires a Google OIDC token from the
# Scheduler/Tasks service account").
#
# Reuses the exact `google-auth` verification primitives
# `app/backends/gchat.py`'s `verify_chat_bearer_token` already established
# for Google Chat's own bearer token -- the shapes differ only in what's checked
# beyond signature+audience: Chat's token asserts a fixed issuer identity
# (`chat@system.gserviceaccount.com`); a Scheduler/Cloud Tasks OIDC token
# instead asserts an arbitrary caller *service account email*, checked here
# against an allow-list rather than a single constant, since a real
# deployment has (at least) one caller identity for Cloud Scheduler
# (`infra/modules/schedule`'s `google_service_account.scheduler`) and,
# later, a second for Cloud Tasks retries (`TASKS_SERVICE_ACCOUNT_EMAIL`,
# `app/tasks.py`).
# ---------------------------------------------------------------------------


def verify_internal_oidc_token(
    token: str,
    *,
    audience: str,
    allowed_emails: frozenset[str],
    certs: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Raises `ValueError` on any invalid signature/audience/issuer/expiry/
    caller -- no "falsy but not exception" failure mode, matching
    `verify_chat_bearer_token`'s own contract.

    Checks, in order: (1) the token is a validly-signed Google ID token
    (against Google's own published OAuth2 certs -- the real/prod path --
    or `certs`, a small `{key_id: public_key_pem}` map, when injected for a
    test that signs its own JWT rather than hitting the network, the same
    substitution `verify_chat_bearer_token` accepts); (2) `aud` equals
    `audience` (`OIDC_AUDIENCE` -- matching
    `infra/modules/schedule/main.tf`'s `oidc_token { audience =
    var.relay_service_url }`, this is the Cloud Run service's own URL, not
    the public-facing Hosting domain `PUBLIC_BASE_URL` holds -- see
    `app/routers/internal.py`'s module docstring for why those two are not
    the same value); (3) on the real/prod path only, `iss` is one of
    Google's own OAuth2 issuers (`google.oauth2.id_token.verify_oauth2_token`
    enforces this itself; the certs-injected test path below skips it, the
    same trade-off `verify_chat_bearer_token` makes -- a self-signed test
    JWT has no real Google issuer to assert); (4) `email` is a member of
    `allowed_emails` (`OIDC_ALLOWED_EMAILS`) -- the one check with no
    equivalent in `verify_chat_bearer_token`, since Google Chat's bearer
    token always asserts the same fixed Chat-system identity, while an
    OIDC-authenticated caller's identity is exactly what distinguishes "the
    deployment's own Cloud Scheduler" from "anyone else who got hold of a
    validly-signed-but-unrelated Google ID token"."""
    if not audience:
        raise ValueError("no OIDC_AUDIENCE configured for this deployment")
    if not allowed_emails:
        raise ValueError("no OIDC_ALLOWED_EMAILS configured for this deployment")
    if certs is not None:
        claims = google_jwt.decode(
            token, certs=dict(certs), audience=audience, clock_skew_in_seconds=30
        )
    else:
        request = google_requests.Request()
        claims = google_id_token.verify_oauth2_token(
            token, request, audience=audience, clock_skew_in_seconds=30
        )
    email = claims.get("email")
    if email not in allowed_emails:
        raise ValueError(f"unexpected caller email: {email!r}")
    return claims
