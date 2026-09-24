#!/usr/bin/env python3
"""Set (or restore) a web-app user's password with the Firebase Admin SDK.

Why this exists: Google's email pipeline on the kid-pager project delivers
nothing (no sign-in links, no password resets), so a password is the only way
into the web app -- and Firebase silently deletes an account's password the
moment that account completes an email-link sign-in (docs/GOTCHAS.md, "An
email-link sign-in deletes the account's password"). This is the way back in.

Auth: Application Default Credentials (`gcloud auth application-default login`
as an owner of the project). The password is read from a prompt, never from
argv or the environment, so it does not land in shell history.

    relay/.venv/bin/python tools/set_web_password.py awong.dev@gmail.com
"""

from __future__ import annotations

import getpass
import sys

import firebase_admin
from firebase_admin import auth, credentials

PROJECT_ID = "kid-pager"


def main(argv: list[str]) -> int:
    if len(argv) != 2 or argv[1].startswith("-"):
        print(f"usage: {argv[0]} <email>", file=sys.stderr)
        return 2
    email = argv[1]

    firebase_admin.initialize_app(
        credentials.ApplicationDefault(), {"projectId": PROJECT_ID}
    )
    try:
        user = auth.get_user_by_email(email)
    except auth.UserNotFoundError:
        print(f"no Firebase Auth account for {email}", file=sys.stderr)
        return 1

    pw = getpass.getpass(f"new password for {email} (uid {user.uid}): ")
    if len(pw) < 6:
        print("Firebase requires at least 6 characters; nothing changed.", file=sys.stderr)
        return 1
    if getpass.getpass("repeat: ") != pw:
        print("passwords differ; nothing changed.", file=sys.stderr)
        return 1

    auth.update_user(user.uid, password=pw)
    print(f"password set for {email}. Existing sessions stay valid; sign in at /login with it.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
