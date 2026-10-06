"""`/api/me/*` -- docs/SERVER_PLAN.md §5.1."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app.config import Settings
from app.main import create_app
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import externals as externals_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header


def make_settings(**overrides: object) -> Settings:
    defaults = {
        "broker_api_url": "http://unused.invalid/api/v5",
        "broker_api_key": None,
        "broker_api_secret": None,
        "webhook_key": "test-webhook-key",
        "dev_mode": True,
        "google_cloud_project": None,
        "firestore_emulator_host": None,
        "firebase_auth_emulator_host": None,
    }
    defaults.update(overrides)
    return Settings(**defaults)


@pytest.fixture
def client() -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=FakeBrokerClient())
    with TestClient(app) as c:
        yield c


def _make_user(uid: str, alias: str) -> dict[str, str]:
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name=alias, family_id="me-fam")
    fb_auth.set_custom_user_claims(uid, {"role": "member", "fam": "me-fam"})
    return auth_header(uid)


def test_get_me_returns_user_and_role(client: TestClient):
    headers = _make_user("me1", "me1")
    resp = client.get("/api/me", headers=headers)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["user"]["alias"] == "me1"
    assert data["role"] == "member"
    # docs/FAMILIES_DESIGN.md §3: familyId/kind/policy/notify live inside
    # `user` (the `User` model already carries them); a freshly-created
    # member's claims (none yet -- `create_user` in this test module never
    # calls `set_claims`) already agree with the doc's defaults, so this is
    # not stale.
    assert data["user"]["familyId"] == "me-fam"
    assert data["user"]["kind"] == "person"
    assert data["user"]["policy"] == {"out": "people", "in": "people"}
    assert data["user"]["notify"] == {"alerts": True}
    assert data["claimsStale"] is False


def test_get_me_reports_claims_stale_when_doc_role_changed_and_reissues_claims(
    client: TestClient,
):
    uid = "me-stale-1"
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=uid, display_name=uid, family_id="me-fam")
    fb_auth.set_custom_user_claims(uid, {"role": "member", "fam": "me-fam"})
    # The doc's role changes (e.g. promoted to a family admin) without the
    # token's claims being reissued yet -- `/api/me` must notice the
    # mismatch and reissue `{role, fam}` server-side.
    users_store.update_user(uid, role="admin")

    resp = client.get("/api/me", headers=auth_header(uid))
    assert resp.status_code == 200, resp.text
    assert resp.json()["claimsStale"] is True

    # `set_claims` was called server-side as a side effect of the mismatch
    # above -- the Firebase Auth user's custom claims are reissued in place,
    # so the *next* freshly-minted token (this test's `auth_header` always
    # mints one, the same "force refresh" a real client does) already
    # agrees with the doc.
    refreshed = fb_auth.get_user(uid)
    assert refreshed.custom_claims == {"role": "admin", "fam": "me-fam"}

    resp2 = client.get("/api/me", headers=auth_header(uid))
    assert resp2.json()["claimsStale"] is False


def test_get_me_requires_auth(client: TestClient):
    resp = client.get("/api/me")
    assert resp.status_code == 401


def test_patch_me_notify_alerts_round_trips(client: TestClient):
    headers = _make_user("patchme1", "patchme1")

    resp = client.patch("/api/me", json={"notify": {"alerts": False}}, headers=headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["notify"]["alerts"] is False

    resp2 = client.get("/api/me", headers=headers)
    assert resp2.json()["user"]["notify"]["alerts"] is False

    resp3 = client.patch("/api/me", json={"notify": {"alerts": True}}, headers=headers)
    assert resp3.json()["notify"]["alerts"] is True


def test_patch_me_requires_auth(client: TestClient):
    resp = client.patch("/api/me", json={"notify": {"alerts": False}})
    assert resp.status_code == 401


def test_list_backends_includes_implicit_webapp_backend(client: TestClient):
    headers = _make_user("me2", "me2")
    resp = client.get("/api/me/backends", headers=headers)
    assert resp.status_code == 200
    kinds = {b["kind"] for b in resp.json()}
    assert kinds == {"webapp"}


def test_create_backend_rejects_pager_kind(client: TestClient):
    headers = _make_user("me3", "me3")
    resp = client.post(
        "/api/me/backends", json={"kind": "pager", "config": {}}, headers=headers
    )
    assert resp.status_code == 400


def test_create_backend_kind_sms_is_422(client: TestClient):
    headers = _make_user("me4", "me4")
    resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15551234567"}},
        headers=headers,
    )
    assert resp.status_code == 422, resp.text
    assert [b["kind"] for b in client.get("/api/me/backends", headers=headers).json()] == ["webapp"]


def test_list_backends_skips_retired_sms_row(client: TestClient):
    """A `kind:"sms"` row left over from the removed relay SMS backend must
    not 500 a read, and must not take part in delivery."""
    from app.db.firestore import get_db
    from app.routing import Routing

    headers = _make_user("me5", "me5")
    get_db().collection("users").document("me5").collection("backends").document("stale").set(
        {"kind": "sms", "config": {}}
    )

    resp = client.get("/api/me/backends", headers=headers)
    assert resp.status_code == 200, resp.text
    assert [b["kind"] for b in resp.json()] == ["webapp"]
    assert backends_store.get_backend("me5", "stale") is None

    users_store.create_user(uid="sender5", alias="sender5", display_name="Sender5")
    allow_store.set_edge("sender5", "me5", message=True, locate=False)
    allow_store.set_edge("me5", "sender5", message=True, locate=False)
    result = Routing(FakeBrokerClient()).send(
        sender_uid="sender5",
        recipient_alias="me5",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert result.ok
    assert {d.kind for d in result.messages[0].deliveries.values()} == {"webapp"}


def test_verify_unknown_backend_is_404(client: TestClient):
    headers = _make_user("me9", "me9")
    resp = client.post("/api/me/backends/nope/verify", json={"code": "123456"}, headers=headers)
    assert resp.status_code == 404


def test_push_token_add_and_remove(client: TestClient):
    headers = _make_user("me5", "me5")
    add_resp = client.post(
        "/api/me/push-tokens", json={"token": "tok_abc123"}, headers=headers
    )
    assert add_resp.status_code == 200

    from app.store import push_tokens as push_tokens_store

    assert push_tokens_store.list_tokens("me5") == ["tok_abc123"]

    del_resp = client.delete("/api/me/push-tokens/tok_abc123", headers=headers)
    assert del_resp.status_code == 200
    assert push_tokens_store.list_tokens("me5") == []


# ---------------------------------------------------------------------------
# GET /api/directory -- docs/FAMILIES_TASKS.md 1.5
# ---------------------------------------------------------------------------


def test_directory_union_of_family_edge_peer_and_conversation_external(client: TestClient):
    fb_auth.create_user(uid="dir-me", email="dir-me@example.com")
    users_store.create_user(
        uid="dir-me", alias="dirme", display_name="Dir Me", family_id="dir-fam-a"
    )
    headers = auth_header("dir-me")

    # Same family as the caller.
    fb_auth.create_user(uid="dir-sib", email="dir-sib@example.com")
    users_store.create_user(
        uid="dir-sib", alias="dirsib", display_name="Dir Sib", family_id="dir-fam-a"
    )

    # An `allow` edge peer in a different family (caller is `fromUid`).
    fb_auth.create_user(uid="dir-peer", email="dir-peer@example.com")
    users_store.create_user(
        uid="dir-peer", alias="dirpeer", display_name="Dir Peer", family_id="dir-fam-b"
    )
    allow_store.set_edge("dir-me", "dir-peer", message=True, locate=False)

    # An external the caller has a conversation with.
    ext = externals_store.get_or_create("dir-fam-a", "+15551234567", "Unknown")
    messages_store.create_message(
        sender_uid="dir-me", recipient_uid=ext.uid, kind="text", ts=1000, body="hi"
    )

    # An unrelated user: different family, no edge, no conversation.
    fb_auth.create_user(uid="dir-stranger", email="dir-stranger@example.com")
    users_store.create_user(
        uid="dir-stranger",
        alias="dirstranger",
        display_name="Dir Stranger",
        family_id="dir-fam-c",
    )

    resp = client.get("/api/directory", headers=headers)
    assert resp.status_code == 200, resp.text
    entries = resp.json()["entries"]
    by_uid = {e["uid"]: e for e in entries}

    assert "dir-sib" in by_uid
    assert by_uid["dir-sib"]["familyId"] == "dir-fam-a"
    assert "phone" not in by_uid["dir-sib"]

    assert "dir-peer" in by_uid
    assert by_uid["dir-peer"]["familyId"] == "dir-fam-b"

    assert ext.uid in by_uid
    assert by_uid[ext.uid]["kind"] == "external"
    assert by_uid[ext.uid]["familyId"] is None
    assert by_uid[ext.uid]["phone"] == "+15551234567"

    assert "dir-stranger" not in by_uid

    # Sorted by alias.
    assert [e["alias"] for e in entries] == sorted(e["alias"] for e in entries)


def test_directory_requires_auth(client: TestClient):
    resp = client.get("/api/directory")
    assert resp.status_code == 401
