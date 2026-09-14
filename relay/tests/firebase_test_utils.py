"""Test-only helpers for getting a *real* Firebase ID token out of the Auth
emulator, the same two-step dance `tools/pager_client.py` will do in a later
phase (docs/SERVER_PLAN.md §8): mint a custom token server-side
(`firebase_admin.auth.create_custom_token`), then exchange it for an ID
token against the emulator's Identity Toolkit REST endpoint. Using a real
token (not a hand-built JWT) means these tests exercise the exact same
`firebase_admin.auth.verify_id_token` path production traffic does, and
(for tests/test_rules.py) the exact same tokens `firestore.rules` sees.
"""

from __future__ import annotations

import os

import httpx
from firebase_admin import auth as fb_auth

AUTH_HOST = os.environ.get("FIREBASE_AUTH_EMULATOR_HOST", "localhost:9099")
# The Auth emulator does not validate this key -- any non-empty string works.
FAKE_API_KEY = "fake-api-key"


def mint_id_token(uid: str) -> str:
    custom_token = fb_auth.create_custom_token(uid).decode("utf-8")
    resp = httpx.post(
        f"http://{AUTH_HOST}/identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken",
        params={"key": FAKE_API_KEY},
        json={"token": custom_token, "returnSecureToken": True},
        timeout=10.0,
    )
    resp.raise_for_status()
    return resp.json()["idToken"]


def auth_header(uid: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {mint_id_token(uid)}"}
