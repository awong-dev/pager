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


def test_create_update_delete_sms_backend(client: TestClient):
    headers = _make_user("me4", "me4")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15551234567"}, "enabled": False},
        headers=headers,
    )
    assert create_resp.status_code == 200, create_resp.text
    bid = create_resp.json()["id"]

    patch_resp = client.patch(
        f"/api/me/backends/{bid}", json={"enabled": True}, headers=headers
    )
    assert patch_resp.status_code == 200
    assert patch_resp.json()["enabled"] is True

    delete_resp = client.delete(f"/api/me/backends/{bid}", headers=headers)
    assert delete_resp.status_code == 200

    patch_after_delete = client.patch(
        f"/api/me/backends/{bid}", json={"enabled": True}, headers=headers
    )
    assert patch_after_delete.status_code == 404


def _capture_sms(monkeypatch: pytest.MonkeyPatch) -> list[tuple[str, str]]:
    """Whitebox capture of every `send_sms()` call -- the only way tests can
    still learn a verify code after H1 (it is no longer readable off the
    backend's `config`, by design)."""
    from app.backends import sms_twilio as sms_twilio_module
    from app.notify.sms import TwilioSendResult

    sent: list[tuple[str, str]] = []

    def fake_send_sms(to: str, body: str) -> TwilioSendResult:
        sent.append((to, body))
        return TwilioSendResult(ok=True, sid="SMtest")

    monkeypatch.setattr(sms_twilio_module.sms_client, "send_sms", fake_send_sms)
    return sent


def _code_from_sms(sent: list[tuple[str, str]]) -> str:
    import re

    match = re.search(r"\d{6}", sent[-1][1])
    assert match is not None, f"no 6-digit code found in {sent[-1][1]!r}"
    return match.group(0)


def test_create_sms_backend_auto_start_link_sends_code_and_stays_disabled(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
):
    """`POST /api/me/backends` calls the
    new backend's `start_link()` -- for `sms` that texts a verify code.

    **H1**: the code is never stored in `config` (readable by the backend's
    own owner via `firestore.rules` -- exactly the person a phone claim
    needs to be verified against). **H2**: a backend with a link/verify flow
    is never `enabled` at creation, regardless of what the request asked
    for, until it is actually verified."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    sent = _capture_sms(monkeypatch)
    headers = _make_user("me6", "me6")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15559998888"}, "enabled": True},
        headers=headers,
    )
    assert create_resp.status_code == 200, create_resp.text
    body = create_resp.json()
    assert body["config"]["phone"] == "+15559998888"
    # H1: no verify code (or its expiry) anywhere in the client-readable config.
    assert "verifyCode" not in body["config"]
    assert "verifyCodeExpiresAt" not in body["config"]
    assert body["verifiedAt"] is None
    # H2: forced to disabled despite the request asking for `enabled: true`.
    assert body["enabled"] is False
    # The code really was sent -- just not stashed anywhere the claimant can read.
    assert len(sent) == 1
    assert sent[0][0] == "+15559998888"


def test_verify_sms_backend_wrong_code_is_400(client: TestClient, monkeypatch: pytest.MonkeyPatch):
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    headers = _make_user("me7", "me7")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15559998889"}, "enabled": True},
        headers=headers,
    )
    bid = create_resp.json()["id"]

    resp = client.post(f"/api/me/backends/{bid}/verify", json={"code": "000000"}, headers=headers)
    assert resp.status_code == 400


def test_verify_sms_backend_right_code_marks_verified_and_indexes_phone(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
):
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    sent = _capture_sms(monkeypatch)
    headers = _make_user("me8", "me8")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15559998890"}, "enabled": True},
        headers=headers,
    )
    bid = create_resp.json()["id"]
    assert create_resp.json()["enabled"] is False  # H2
    code = _code_from_sms(sent)

    resp = client.post(f"/api/me/backends/{bid}/verify", json={"code": code}, headers=headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["verifiedAt"] is not None
    assert body["enabled"] is True
    assert "verifyCode" not in body["config"]

    from app.store import backends as backends_store

    assert backends_store.get_by_phone("+15559998890") == ("me8", bid)
    # The one-time verify code record is cleaned up on success, not left
    # sitting around until its TTL.
    assert backends_store.get_sms_verify_code(bid) is None


def test_verify_unknown_backend_is_404(client: TestClient):
    headers = _make_user("me9", "me9")
    resp = client.post("/api/me/backends/nope/verify", json={"code": "123456"}, headers=headers)
    assert resp.status_code == 404


def test_h2_unverified_sms_backend_never_receives_a_delivery(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
):
    """H2: `POST /api/me/backends` creating an sms backend must not fan out
    real SMS to it before it is verified -- exercised through the real
    `Routing.send()` path (not just the enabled flag in isolation), the
    same delivery engine `app/routing.py`'s fan-out uses in production."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    headers = _make_user("me10", "me10")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15559998891"}, "enabled": True},
        headers=headers,
    )
    assert create_resp.status_code == 200, create_resp.text
    assert create_resp.json()["enabled"] is False

    from app.routing import Routing
    from app.store import allow as allow_store
    from app.store import users as users_store
    from tests.fake_transport import FakeBrokerClient

    users_store.create_user(uid="sender10", alias="sender10", display_name="Sender10")
    allow_store.set_edge("sender10", "me10", message=True, locate=True)
    allow_store.set_edge("me10", "sender10", message=True, locate=True)

    routing = Routing(FakeBrokerClient())
    result = routing.send(
        sender_uid="sender10",
        recipient_alias="me10",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert result.ok
    msg = result.messages[0]
    kinds = {d.kind for d in msg.deliveries.values()}
    assert "sms" not in kinds


def test_h3_patching_phone_clears_old_phone_index_and_verified_state(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
):
    """H3: `PATCH .../backends/{bid}` rewriting `config.phone` on an
    already-verified backend must not silently keep routing/attributing to
    the old number, and must not leave the backend `verified`/`enabled` for
    a number that was never actually proven."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    sent = _capture_sms(monkeypatch)
    headers = _make_user("me11", "me11")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15559998892"}, "enabled": True},
        headers=headers,
    )
    bid = create_resp.json()["id"]
    code = _code_from_sms(sent)
    verify_resp = client.post(
        f"/api/me/backends/{bid}/verify", json={"code": code}, headers=headers
    )
    assert verify_resp.status_code == 200, verify_resp.text

    from app.store import backends as backends_store

    assert backends_store.get_by_phone("+15559998892") == ("me11", bid)

    patch_resp = client.patch(
        f"/api/me/backends/{bid}",
        json={"config": {"phone": "+15559998893"}},
        headers=headers,
    )
    assert patch_resp.status_code == 200, patch_resp.text
    body = patch_resp.json()
    assert body["verifiedAt"] is None
    assert body["enabled"] is False
    assert backends_store.get_by_phone("+15559998892") is None
    # The new number is not indexed either -- only a real verify does that.
    assert backends_store.get_by_phone("+15559998893") is None


def test_h3_deleting_sms_backend_clears_phone_index(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
):
    """H3: deleting a verified sms backend must not leave a dangling
    `phoneIndex` entry that keeps routing inbound SMS to a `bid` that no
    longer exists."""
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)
    sent = _capture_sms(monkeypatch)
    headers = _make_user("me12", "me12")
    create_resp = client.post(
        "/api/me/backends",
        json={"kind": "sms", "config": {"phone": "+15559998894"}, "enabled": True},
        headers=headers,
    )
    bid = create_resp.json()["id"]
    code = _code_from_sms(sent)
    verify_resp = client.post(
        f"/api/me/backends/{bid}/verify", json={"code": code}, headers=headers
    )
    assert verify_resp.status_code == 200, verify_resp.text

    from app.store import backends as backends_store

    assert backends_store.get_by_phone("+15559998894") == ("me12", bid)

    delete_resp = client.delete(f"/api/me/backends/{bid}", headers=headers)
    assert delete_resp.status_code == 200
    assert backends_store.get_by_phone("+15559998894") is None


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
    fb_auth.create_user(uid="dir-ext", email="dir-ext@example.com")
    users_store.create_user(
        uid="dir-ext", alias="15551234567", display_name="Unknown", kind="external"
    )
    backends_store.create_backend(
        "dir-ext", kind="sms", config={"phone": "+15551234567"}, enabled=True
    )
    messages_store.create_message(
        sender_uid="dir-me", recipient_uid="dir-ext", kind="text", ts=1000, body="hi"
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

    assert "dir-ext" in by_uid
    assert by_uid["dir-ext"]["kind"] == "external"
    assert by_uid["dir-ext"]["familyId"] is None
    assert by_uid["dir-ext"]["phone"] == "+15551234567"

    assert "dir-stranger" not in by_uid

    # Sorted by alias.
    assert [e["alias"] for e in entries] == sorted(e["alias"] for e in entries)


def test_directory_requires_auth(client: TestClient):
    resp = client.get("/api/directory")
    assert resp.status_code == 401
