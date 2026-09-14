"""`python -m app.bootstrap --admin-email <email>` -- docs/SERVER_PLAN.md
§5.3: "the first deploy runs `python -m app.bootstrap --admin-email …`".
Creates the first admin: a Firebase Auth user (by email) + a `users/{uid}`
Firestore document with `role: 'admin'` + the `admin` custom claim.

Idempotent: re-running with the same email finds the existing Auth user
(rather than erroring) and only fills in whatever is still missing (the
`users/{uid}` doc, the custom claim), so it is safe to run again after a
partial failure or just to confirm the admin is still set up correctly.
"""

from __future__ import annotations

import argparse
import sys

from firebase_admin import auth as fb_auth

from app.db.firestore import get_app
from app.store import users as users_store

DEFAULT_ADMIN_ALIAS = "admin"
DEFAULT_ADMIN_DISPLAY_NAME = "Admin"


def bootstrap_admin(
    admin_email: str, *, alias: str = DEFAULT_ADMIN_ALIAS, display_name: str = DEFAULT_ADMIN_DISPLAY_NAME
) -> str:
    """Returns the admin's uid. Safe to call more than once."""
    get_app()  # ensure firebase_admin is initialized before any auth.* call
    try:
        auth_user = fb_auth.get_user_by_email(admin_email)
    except fb_auth.UserNotFoundError:
        auth_user = fb_auth.create_user(email=admin_email)

    uid = auth_user.uid
    user = users_store.get_user(uid)
    if user is None:
        user = users_store.create_user(
            uid=uid,
            alias=alias,
            display_name=display_name,
            email=admin_email,
            role="admin",
        )
    elif user.role != "admin":
        user = users_store.update_user(uid, role="admin")

    fb_auth.set_custom_user_claims(uid, {"admin": True})
    return uid


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--admin-email", required=True)
    parser.add_argument("--alias", default=DEFAULT_ADMIN_ALIAS)
    parser.add_argument("--display-name", default=DEFAULT_ADMIN_DISPLAY_NAME)
    args = parser.parse_args(argv)

    uid = bootstrap_admin(args.admin_email, alias=args.alias, display_name=args.display_name)
    print(f"admin bootstrapped: uid={uid} email={args.admin_email} alias={args.alias}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
