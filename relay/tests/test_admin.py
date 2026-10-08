"""`/api/admin/*` -- docs/SERVER_PLAN.md §5.1, §5.5: user/device CRUD, the
allow-list's replace-all semantics (including the `locatableBy` rewrite on
every affected device), and settings."""

from __future__ import annotations

import json
from collections.abc import Iterator
from dataclasses import dataclass, field

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import book as book_module
from app import devcfg
from app.config import Settings
from app.main import create_app
from app.store import allow as allow_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import families as families_store
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


@dataclass
class FakeEmqxAdmin:
    """Duck-typed stand-in for `app.emqx_admin.EmqxAdmin` -- the real class
    is real-network-only (its `__init__` needs a reachable EMQX to be
    useful), and `app/routers/admin.py`'s `get_emqx` dependency falls back
    to it only when `app.state.emqx_admin` is absent, so this fixture sets
    that attribute instead (docs/DEVICE_TASKS.md S2.2), the same spirit as
    `tests/fake_transport.FakeBrokerClient`. Covers all three methods this
    router and `app.devsetup.issue()` call: `ensure_device` (the router's
    own step 2 push), `ensure_boot_user` (`issue()`'s bootstrap credential),
    and `delete_user` (revoke/rotate)."""

    ensured_devices: list[tuple[str, str, str]] = field(default_factory=list)
    ensured_boot: list[tuple[str, str]] = field(default_factory=list)
    deleted: list[str] = field(default_factory=list)

    def ensure_device(self, username: str, password: str, device_id: str) -> str:
        self.ensured_devices.append((username, password, device_id))
        return "ok"

    def ensure_boot_user(self, bid: str, password: str) -> str:
        self.ensured_boot.append((bid, password))
        return "ok"

    def delete_user(self, username: str) -> str:
        self.deleted.append(username)
        return "ok"


@pytest.fixture
def fake_emqx() -> FakeEmqxAdmin:
    return FakeEmqxAdmin()


@pytest.fixture
def broker() -> FakeBrokerClient:
    """Exposed separately (not just constructed inline inside `client`) so
    this task's book-push tests (`put_allowlist`/`patch_user`'s new
    triggers) can inspect `broker.published` -- same split test_devcfg.py's
    own `broker`/`client` fixtures already use."""
    return FakeBrokerClient()


@pytest.fixture
def client(fake_emqx: FakeEmqxAdmin, broker: FakeBrokerClient) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    app.state.emqx_admin = fake_emqx
    with TestClient(app) as c:
        yield c


@pytest.fixture
def admin_headers() -> dict[str, str]:
    auth_user = fb_auth.create_user(email="root-admin@example.com")
    # The super sits in a family so `POST /api/admin/users` (which defaults
    # `familyId` to the caller's) can create persons (CONTACT_REQ decision 3).
    users_store.create_user(
        uid=auth_user.uid,
        alias="rootadmin",
        display_name="Root Admin",
        role="super",
        family_id="default",
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"role": "super", "fam": "default"})
    return auth_header(auth_user.uid)


# ---- users ----


def test_create_user_creates_auth_and_firestore_and_alias(
    client: TestClient, admin_headers: dict[str, str]
):
    resp = client.post(
        "/api/admin/users",
        json={"alias": "mom", "displayName": "Mom", "email": "mom@example.com"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["alias"] == "mom"
    assert data["role"] == "member"

    # Firebase Auth account really exists.
    auth_user = fb_auth.get_user_by_email("mom@example.com")
    assert auth_user.uid == data["uid"]
    # And the alias resolves.
    assert users_store.get_uid_for_alias("mom") == data["uid"]

    # docs/FAMILIES_DESIGN.md §1 decision 2: every created user (not just
    # admins) gets `{role, fam}` claims in sync with the doc.
    refreshed = fb_auth.get_user(data["uid"])
    assert refreshed.custom_claims == {"role": "member", "fam": "default"}


def test_create_user_requires_email_or_phone(client: TestClient, admin_headers: dict[str, str]):
    resp = client.post(
        "/api/admin/users",
        json={"alias": "nobody", "displayName": "Nobody"},
        headers=admin_headers,
    )
    assert resp.status_code == 400


def test_create_user_duplicate_alias_rolls_back_auth_user(
    client: TestClient, admin_headers: dict[str, str]
):
    client.post(
        "/api/admin/users",
        json={"alias": "dup", "displayName": "First", "email": "first@example.com"},
        headers=admin_headers,
    )
    resp = client.post(
        "/api/admin/users",
        json={"alias": "dup", "displayName": "Second", "email": "second@example.com"},
        headers=admin_headers,
    )
    assert resp.status_code == 400
    # The rolled-back Auth account must not linger.
    with pytest.raises(fb_auth.UserNotFoundError):
        fb_auth.get_user_by_email("second@example.com")


def test_patch_user_updates_fields_and_admin_claim(
    client: TestClient, admin_headers: dict[str, str]
):
    created = client.post(
        "/api/admin/users",
        json={"alias": "kid", "displayName": "Kid", "email": "kid@example.com"},
        headers=admin_headers,
    ).json()

    resp = client.patch(
        f"/api/admin/users/{created['uid']}",
        json={"displayName": "Kiddo", "role": "admin"},
        headers=admin_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["displayName"] == "Kiddo"
    assert data["role"] == "admin"

    # docs/FAMILIES_DESIGN.md §1 decision 2: claims are `{role, fam}`
    # exactly -- no `admin` key -- `fam` empty since this legacy
    # `/api/admin/users` route (task 1.3 re-homes it) never sets a family.
    refreshed = fb_auth.get_user(created["uid"])
    assert refreshed.custom_claims == {"role": "admin", "fam": "default"}


def test_patch_user_404_for_unknown_uid(client: TestClient, admin_headers: dict[str, str]):
    resp = client.patch(
        "/api/admin/users/does-not-exist", json={"displayName": "x"}, headers=admin_headers
    )
    assert resp.status_code == 404


def test_delete_user_removes_auth_and_firestore(client: TestClient, admin_headers: dict[str, str]):
    created = client.post(
        "/api/admin/users",
        json={"alias": "gone", "displayName": "Gone", "email": "gone@example.com"},
        headers=admin_headers,
    ).json()

    resp = client.delete(f"/api/admin/users/{created['uid']}", headers=admin_headers)
    assert resp.status_code == 200
    assert users_store.get_user(created["uid"]) is None
    with pytest.raises(fb_auth.UserNotFoundError):
        fb_auth.get_user(created["uid"])


# ---- devices ----


def test_create_device_returns_a_setup_code_and_stores_only_a_hash(
    client: TestClient, admin_headers: dict[str, str], fake_emqx: FakeEmqxAdmin
):
    owner = client.post(
        "/api/admin/users",
        json={"alias": "owner1", "displayName": "Owner", "email": "owner1@example.com"},
        headers=admin_headers,
    ).json()

    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-1001", "ownerAlias": "owner1", "label": "test pager"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["device"]["mqttUsername"] == "pgr-1001"
    assert data["device"]["provisionState"] == "issued"
    assert data["setupCode"]
    assert data["expiresAt"]
    assert data["brokerPush"] == "pushed"  # FakeEmqxAdmin.ensure_device always "ok"
    assert data["manualAcl"] is None

    # docs/DEVICE_PLAN.md §3.2 step 2: the router pushed the device's real
    # broker credential/ACL itself.
    assert fake_emqx.ensured_devices == [("pgr-1001", fake_emqx.ensured_devices[0][1], "pgr-1001")]
    password = fake_emqx.ensured_devices[0][1]
    assert password

    device = devices_store.get_device("pgr-1001")
    assert device is not None
    assert device.ownerUid == owner["uid"]

    # The plaintext password is never stored anywhere -- not on `devices/{d}`
    # (S1.1's migration, mounted by this task) and not in the clear in
    # `deviceSecrets/{d}` either, only its hash.
    import hashlib

    secret = device_secrets_store.get("pgr-1001")
    assert secret is not None
    assert secret.mqttPasswordHash != password
    assert secret.mqttPasswordHash == hashlib.sha256(password.encode("utf-8")).hexdigest()
    assert secret.hmacKey and len(secret.hmacKey) == 32


def test_setup_bundle_sets_req_sig_flag_for_an_hmac_device(
    client: TestClient, admin_headers: dict[str, str], monkeypatch
):
    """docs/DEVICE_PLAN.md section 10 H2: an `authMode: "hmac"` device's bundle
    must carry flags bit 0 (req_sig). With flags=0 the device publishes
    unsigned and ingest drops every envelope as bad-sig (found live)."""
    from app import devsetup

    seen: list[int] = []
    real_issue = devsetup.issue

    def spy(*args, **kwargs):
        seen.append(kwargs.get("flags", 0))
        return real_issue(*args, **kwargs)

    monkeypatch.setattr(devsetup, "issue", spy)
    client.post(
        "/api/admin/users",
        json={"alias": "owner9", "displayName": "Owner", "email": "owner9@example.com"},
        headers=admin_headers,
    )
    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-1009", "ownerAlias": "owner9", "label": "flag test"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert devices_store.get_device("pgr-1009").authMode == "hmac"

    resp = client.post("/api/admin/devices/pgr-1009/rotate-credentials", headers=admin_headers)
    assert resp.status_code == 200, resp.text

    assert seen == [1, 1]  # create, then rotate


def test_create_device_picks_up_locatable_by_from_a_pre_existing_allow_edge(
    client: TestClient, admin_headers: dict[str, str]
):
    """An allow edge with `locate=True` set
    *before* the device it names as `toAlias` exists must not be lost --
    `devices_store.create_device` always starts a fresh device at
    `locatableBy: []`, and `set_edge`/`replace_all` only ever recompute
    `locatableBy` on devices that already exist at the moment an edge
    changes, so without `POST /api/admin/devices` recomputing it once more
    right after creation, this device would stay `locatableBy: []` forever
    (see `app.store.allow.recompute_locatable_by_for_owner`'s docstring).

    docs/FAMILIES_TASKS.md 2.3: a `locate: true` edge is refused unless both
    ends share a `familyId`, so mom3/kid3 are created in the same family."""
    family = families_store.create_family(name="F-locate3", created_by="rootadmin")
    for alias in ("mom3", "kid3"):
        client.post(
            "/api/admin/users",
            json={
                "alias": alias,
                "displayName": alias,
                "email": f"{alias}@example.com",
                "familyId": family.id,
            },
            headers=admin_headers,
        )
    mom_uid = users_store.get_uid_for_alias("mom3")

    # Allow edge set up *before* the device exists.
    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "mom3", "toAlias": "kid3", "message": True, "locate": True},
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    resp2 = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-5005", "ownerAlias": "kid3", "label": "kid device"},
        headers=admin_headers,
    )
    assert resp2.status_code == 200, resp2.text

    device = devices_store.get_device("pgr-5005")
    assert device.locatableBy == [mom_uid]


def test_create_device_default_recipient_external_is_400(
    client: TestClient, admin_headers: dict[str, str]
):
    family = families_store.create_family(name="DefExt", created_by="root")
    users_store.create_user(
        uid="def-owner", alias="defowner", display_name="Owner", family_id=family.id
    )
    ext = externals_store.get_or_create(family.id, "+12065550100", "Grandma")

    resp = client.post(
        "/api/admin/devices",
        json={
            "deviceId": "pgr-defext",
            "ownerAlias": "defowner",
            "label": "p",
            "defaultToAlias": ext.alias,
        },
        headers=admin_headers,
    )
    assert resp.status_code == 400, resp.text
    assert resp.json()["detail"] == "the default recipient cannot be an SMS contact"
    assert devices_store.get_device("pgr-defext") is None


def test_create_device_seeds_cfg_sms_for_open_owner(
    client: TestClient, admin_headers: dict[str, str]
):
    from app.db.firestore import get_db

    family = families_store.create_family(name="SeedSms", created_by="root")
    users_store.create_user(
        uid="seed-open", alias="seedopen", display_name="Open", family_id=family.id
    )
    users_store.create_user(
        uid="seed-people", alias="seedpeople", display_name="People", family_id=family.id
    )
    get_db().collection("users").document("seed-open").update(
        {"policy": {"out": "open", "in": "any"}}
    )
    externals_store.get_or_create(family.id, "+12065550100", "Grandma")

    for device_id, owner in (("pgr-seed-1", "seedopen"), ("pgr-seed-2", "seedpeople")):
        resp = client.post(
            "/api/admin/devices",
            json={"deviceId": device_id, "ownerAlias": owner, "label": "p"},
            headers=admin_headers,
        )
        assert resp.status_code == 200, resp.text

    assert [(c.name, c.phone) for c in devices_store.get_device("pgr-seed-1").smsContacts] == [
        ("Grandma", "+12065550100")
    ]
    assert devices_store.get_device("pgr-seed-2").smsContacts == []


def test_patch_user_family_change_rederives_cfg_sms(
    client: TestClient, admin_headers: dict[str, str]
):
    from app.db.firestore import get_db

    fam_a = families_store.create_family(name="MoveA", created_by="root")
    fam_b = families_store.create_family(name="MoveB", created_by="root")
    created = client.post(
        "/api/admin/users",
        json={
            "alias": "mover",
            "displayName": "Mover",
            "email": "mover@example.com",
            "familyId": fam_a.id,
        },
        headers=admin_headers,
    ).json()
    get_db().collection("users").document(created["uid"]).update(
        {"policy": {"out": "open", "in": "any"}}
    )
    devices_store.create_device(
        device_id="pgr-mover",
        owner_uid=created["uid"],
        label="p",
        mqtt_username="pgr-mover",
        mqtt_password_hash="x",
        auth_mode="password",
        family_id=fam_a.id,
    )
    externals_store.get_or_create(fam_a.id, "+12065550100", "FromA")
    externals_store.get_or_create(fam_b.id, "+12065550101", "FromB")
    book_module.rederive_sms_contacts(created["uid"], FakeBrokerClient())
    assert [c.name for c in devices_store.get_device("pgr-mover").smsContacts] == ["FromA"]

    resp = client.patch(
        f"/api/admin/users/{created['uid']}",
        json={"familyId": fam_b.id},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert [c.name for c in devices_store.get_device("pgr-mover").smsContacts] == ["FromB"]


def test_put_allowlist_rederives_touched_owners(
    client: TestClient, admin_headers: dict[str, str]
):
    family = families_store.create_family(name="AllowSms", created_by="root")
    users_store.create_user(
        uid="al-kid", alias="alkid", display_name="Kid", family_id=family.id
    )
    devices_store.create_device(
        device_id="pgr-al",
        owner_uid="al-kid",
        label="p",
        mqtt_username="pgr-al",
        mqtt_password_hash="x",
        auth_mode="password",
        family_id=family.id,
    )
    ext = externals_store.get_or_create(family.id, "+12065550100", "Grandma")
    assert devices_store.get_device("pgr-al").smsContacts == []

    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "alkid", "toAlias": ext.alias, "message": True, "locate": False}
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert [c.phone for c in devices_store.get_device("pgr-al").smsContacts] == ["+12065550100"]

    # Replacing the list with nothing removes it again (the old edge's owner
    # is touched too).
    resp = client.put("/api/admin/allowlist", json={"entries": []}, headers=admin_headers)
    assert resp.status_code == 200, resp.text
    assert devices_store.get_device("pgr-al").smsContacts == []


def test_admin_create_user_backend_route_is_gone(
    client: TestClient, admin_headers: dict[str, str]
):
    user = users_store.create_user(uid="nobackend", alias="nobackend", display_name="N")
    resp = client.post(
        f"/api/admin/users/{user.uid}/backends",
        json={"kind": "sms", "phone": "+15559990000"},
        headers=admin_headers,
    )
    assert resp.status_code in (404, 405), resp.text


def test_create_device_copies_owner_family_id(
    client: TestClient, admin_headers: dict[str, str]
):
    """docs/FAMILIES_DESIGN.md §1 decision 1/§3: a device's `familyId` is
    copied from its owner at creation time."""
    family = families_store.create_family(name="Device Family", created_by="root-uid")
    users_store.create_user(
        uid="device-owner-uid",
        alias="deviceowner",
        display_name="Device Owner",
        family_id=family.id,
    )

    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-fam-1", "ownerAlias": "deviceowner", "label": "family pager"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    device = devices_store.get_device("pgr-fam-1")
    assert device is not None
    assert device.familyId == family.id


def test_create_device_unknown_owner_alias_is_400(client: TestClient, admin_headers: dict[str, str]):
    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-2002", "ownerAlias": "ghost", "label": "x"},
        headers=admin_headers,
    )
    assert resp.status_code == 400


def test_delete_device(client: TestClient, admin_headers: dict[str, str]):
    client.post(
        "/api/admin/users",
        json={"alias": "owner2", "displayName": "Owner2", "email": "owner2@example.com"},
        headers=admin_headers,
    )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-3003", "ownerAlias": "owner2", "label": "x"},
        headers=admin_headers,
    )
    resp = client.delete("/api/admin/devices/pgr-3003", headers=admin_headers)
    assert resp.status_code == 200
    assert devices_store.get_device("pgr-3003") is None


def test_rotate_credentials_returns_new_setup_code_and_invalidates_old_secret(
    client: TestClient, admin_headers: dict[str, str], fake_emqx: FakeEmqxAdmin
):
    client.post(
        "/api/admin/users",
        json={"alias": "owner6", "displayName": "Owner6", "email": "owner6@example.com"},
        headers=admin_headers,
    )
    created = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-6006", "ownerAlias": "owner6", "label": "x"},
        headers=admin_headers,
    ).json()
    old_secret = device_secrets_store.get("pgr-6006")
    assert old_secret is not None

    # docs/DEVICE_PLAN.md §2.6: an in-process authAlarm the rotation should
    # clear. `devices_store.record_sig_failure` is the same helper
    # `app.ingest.Ingest` calls on a real signature failure.
    for _ in range(devices_store.AUTH_ALARM_THRESHOLD):
        devices_store.record_sig_failure("pgr-6006")
    devices_store.set_auth_alarm("pgr-6006", True)

    # Populate `Ingest`'s in-process secret cache with the pre-rotation key,
    # the same way a real signed envelope would (S1.3's `_get_secret`).
    ingest = client.app.state.ingest
    ingest._get_secret("pgr-6006")
    assert "pgr-6006" in ingest._secret_cache

    resp = client.post(
        "/api/admin/devices/pgr-6006/rotate-credentials", headers=admin_headers
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["device"]["provisionState"] == "issued"
    assert data["setupCode"] != created["setupCode"]
    assert data["brokerPush"] == "pushed"

    # Old broker user deleted, new one pushed under the same username.
    assert "pgr-6006" in fake_emqx.deleted
    assert fake_emqx.ensured_devices[-1][0] == "pgr-6006"

    new_secret = device_secrets_store.get("pgr-6006")
    assert new_secret is not None
    assert new_secret.hmacKey != old_secret.hmacKey
    assert new_secret.mqttPasswordHash != old_secret.mqttPasswordHash

    device = devices_store.get_device("pgr-6006")
    assert device is not None
    assert device.status.authAlarm is False

    # The stale cache entry for the rotated device's old key is gone.
    assert "pgr-6006" not in ingest._secret_cache


def test_revoke_device_sets_revoked_at_and_deletes_broker_credential(
    client: TestClient, admin_headers: dict[str, str], fake_emqx: FakeEmqxAdmin
):
    client.post(
        "/api/admin/users",
        json={"alias": "owner7", "displayName": "Owner7", "email": "owner7@example.com"},
        headers=admin_headers,
    )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-7007", "ownerAlias": "owner7", "label": "x"},
        headers=admin_headers,
    )

    resp = client.post("/api/admin/devices/pgr-7007/revoke", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["revokedAt"] is not None

    device = devices_store.get_device("pgr-7007")
    assert device is not None
    assert device.revokedAt is not None
    assert "pgr-7007" in fake_emqx.deleted


def test_revoke_device_unknown_id_is_404(client: TestClient, admin_headers: dict[str, str]):
    resp = client.post("/api/admin/devices/pgr-ghost/revoke", headers=admin_headers)
    assert resp.status_code == 404


# ---- allowlist replace-all + locatableBy rewrite ----


def test_allowlist_replace_all_rewrites_locatable_by(
    client: TestClient, admin_headers: dict[str, str]
):
    # docs/FAMILIES_TASKS.md 2.3: `locate: true` edges are refused across
    # families, so all three share one.
    family = families_store.create_family(name="F-locate2", created_by="rootadmin")
    for alias in ("mom2", "dad2", "kid2"):
        client.post(
            "/api/admin/users",
            json={
                "alias": alias,
                "displayName": alias,
                "email": f"{alias}@example.com",
                "familyId": family.id,
            },
            headers=admin_headers,
        )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-4004", "ownerAlias": "kid2", "label": "kid device"},
        headers=admin_headers,
    )

    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "mom2", "toAlias": "kid2", "message": True, "locate": True},
                {"fromAlias": "dad2", "toAlias": "kid2", "message": True, "locate": False},
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    entries = resp.json()
    assert len(entries) == 2

    mom_uid = users_store.get_uid_for_alias("mom2")
    dad_uid = users_store.get_uid_for_alias("dad2")

    device = devices_store.get_device("pgr-4004")
    # Only mom2 has locate=True -> only she appears in locatableBy.
    assert device.locatableBy == [mom_uid]

    # Now replace-all again, dropping mom2's edge and adding dad2's locate.
    resp2 = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "dad2", "toAlias": "kid2", "message": True, "locate": True},
            ]
        },
        headers=admin_headers,
    )
    assert resp2.status_code == 200
    device2 = devices_store.get_device("pgr-4004")
    assert device2.locatableBy == [dad_uid]


def test_allowlist_unknown_alias_is_400(client: TestClient, admin_headers: dict[str, str]):
    resp = client.put(
        "/api/admin/allowlist",
        json={"entries": [{"fromAlias": "ghost1", "toAlias": "ghost2"}]},
        headers=admin_headers,
    )
    assert resp.status_code == 400


# ---- docs/FAMILIES_TASKS.md 2.3: locate-cross-family refusal, familyIds,
# and `?family=` scoped replace-all ----


def test_allowlist_locate_true_cross_family_is_400(
    client: TestClient, admin_headers: dict[str, str]
):
    fam_a = families_store.create_family(name="F-cross-a", created_by="rootadmin")
    fam_b = families_store.create_family(name="F-cross-b", created_by="rootadmin")
    client.post(
        "/api/admin/users",
        json={
            "alias": "crossmom",
            "displayName": "m",
            "email": "crossmom@example.com",
            "familyId": fam_a.id,
        },
        headers=admin_headers,
    )
    client.post(
        "/api/admin/users",
        json={
            "alias": "crosskid",
            "displayName": "k",
            "email": "crosskid@example.com",
            "familyId": fam_b.id,
        },
        headers=admin_headers,
    )

    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "crossmom", "toAlias": "crosskid", "message": True, "locate": True}
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 400, resp.text
    assert resp.json()["detail"] == "locate_cross_family"
    assert (
        allow_store.get_edge(
            users_store.get_uid_for_alias("crossmom"), users_store.get_uid_for_alias("crosskid")
        )
        is None
    )


def test_allowlist_locate_true_null_family_is_400(
    client: TestClient, admin_headers: dict[str, str]
):
    """Same refusal when one end has no family at all (an external: persons
    always have one, CONTACT_REQ decision 3)."""
    client.post(
        "/api/admin/users",
        json={"alias": "nullmom", "displayName": "m", "email": "nullmom@example.com"},
        headers=admin_headers,
    )
    external = externals_store.get_or_create("default", "+12065550177", "ext")

    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "nullmom", "toAlias": external.alias, "message": True, "locate": True}
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 400, resp.text
    assert resp.json()["detail"] == "locate_cross_family"


def test_allowlist_writes_family_ids_on_every_edge(
    client: TestClient, admin_headers: dict[str, str]
):
    family = families_store.create_family(name="F-fids", created_by="rootadmin")
    client.post(
        "/api/admin/users",
        json={
            "alias": "fidsmom",
            "displayName": "m",
            "email": "fidsmom@example.com",
            "familyId": family.id,
        },
        headers=admin_headers,
    )
    client.post(
        "/api/admin/users",
        json={
            "alias": "fidskid",
            "displayName": "k",
            "email": "fidskid@example.com",
            "familyId": family.id,
        },
        headers=admin_headers,
    )

    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "fidsmom", "toAlias": "fidskid", "message": True, "locate": True}
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()[0]["familyIds"] == [family.id]


def test_allowlist_family_scoped_put_leaves_other_family_edges_untouched(
    client: TestClient, admin_headers: dict[str, str]
):
    """docs/FAMILIES_TASKS.md 2.3: `?family=` replaces only edges where
    either end is in that family; an edge entirely outside it survives even
    though it's absent from the submitted entries."""
    fam_a = families_store.create_family(name="F-scope-a", created_by="rootadmin")
    fam_b = families_store.create_family(name="F-scope-b", created_by="rootadmin")
    for alias, fam in (
        ("scopea1", fam_a),
        ("scopea2", fam_a),
        ("scopeb1", fam_b),
        ("scopeb2", fam_b),
    ):
        client.post(
            "/api/admin/users",
            json={
                "alias": alias,
                "displayName": alias,
                "email": f"{alias}@example.com",
                "familyId": fam.id,
            },
            headers=admin_headers,
        )
    # Seed one edge per family with a plain (unscoped) replace-all.
    resp0 = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "scopea1", "toAlias": "scopea2", "message": True, "locate": True},
                {"fromAlias": "scopeb1", "toAlias": "scopeb2", "message": True, "locate": True},
            ]
        },
        headers=admin_headers,
    )
    assert resp0.status_code == 200, resp0.text

    # A family-A-scoped replace-all that submits no entries at all must not
    # touch family B's edge.
    resp = client.put(
        f"/api/admin/allowlist?family={fam_a.id}",
        json={"entries": []},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    assert (
        allow_store.get_edge(
            users_store.get_uid_for_alias("scopea1"), users_store.get_uid_for_alias("scopea2")
        )
        is None
    )
    assert (
        allow_store.get_edge(
            users_store.get_uid_for_alias("scopeb1"), users_store.get_uid_for_alias("scopeb2")
        )
        is not None
    )


# ---- docs/PROTOCOL.md §3.7 (v0.4) / docs/CHAT_UI_DESIGN.md §1: book-bump
# triggers -- allow-list PUT and displayName PATCH. ----


def test_allowlist_put_nudges_changed_owners_only(
    client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    for alias in ("ownera", "ownerb", "contacta", "contactb"):
        client.post(
            "/api/admin/users",
            json={"alias": alias, "displayName": alias, "email": f"{alias}@example.com"},
            headers=admin_headers,
        )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-nudge-a", "ownerAlias": "ownera", "label": "a"},
        headers=admin_headers,
    )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-nudge-b", "ownerAlias": "ownerb", "label": "b"},
        headers=admin_headers,
    )
    # Establish ownerb's edge first -- unchanged by the PUT under test below.
    resp0 = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "ownerb", "toAlias": "contactb", "message": True, "locate": False},
            ]
        },
        headers=admin_headers,
    )
    assert resp0.status_code == 200, resp0.text
    bv_a_before = devcfg.get_book_version("pgr-nudge-a")
    bv_b_before = devcfg.get_book_version("pgr-nudge-b")
    broker.clear()

    # ownera gains a message edge; ownerb's own message-edge set is
    # resubmitted unchanged.
    resp = client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "ownerb", "toAlias": "contactb", "message": True, "locate": False},
                {"fromAlias": "ownera", "toAlias": "contacta", "message": True, "locate": False},
            ]
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    assert devcfg.get_book_version("pgr-nudge-a") == bv_a_before + 1
    assert devcfg.get_book_version("pgr-nudge-b") == bv_b_before

    books = [
        json.loads(p.payload) for p in broker.published if json.loads(p.payload).get("kind") == "book"
    ]
    assert len(books) == 1


def test_display_name_change_bumps_listing_owners(
    client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    for alias in ("owner-dn", "contact-dn"):
        client.post(
            "/api/admin/users",
            json={"alias": alias, "displayName": alias, "email": f"{alias}@example.com"},
            headers=admin_headers,
        )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-dn-1", "ownerAlias": "owner-dn", "label": "d"},
        headers=admin_headers,
    )
    contact_uid = users_store.get_uid_for_alias("contact-dn")
    client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "owner-dn", "toAlias": "contact-dn", "message": True, "locate": False},
                # reverse edge: default `people` policy needs both ends to
                # approve before the entry is sendable (and so listed).
                {"fromAlias": "contact-dn", "toAlias": "owner-dn", "message": True, "locate": False},
            ]
        },
        headers=admin_headers,
    )
    bv_before = devcfg.get_book_version("pgr-dn-1")
    broker.clear()

    resp = client.patch(
        f"/api/admin/users/{contact_uid}",
        json={"displayName": "New Name"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text

    assert devcfg.get_book_version("pgr-dn-1") == bv_before + 1
    books = [
        json.loads(p.payload) for p in broker.published if json.loads(p.payload).get("kind") == "book"
    ]
    assert len(books) == 1
    assert any(c["a"] == "contact-dn" and c["n"] == "New Name" for c in books[0]["c"])


def test_display_name_unchanged_does_not_bump_book(
    client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    for alias in ("owner-dn2", "contact-dn2"):
        client.post(
            "/api/admin/users",
            json={"alias": alias, "displayName": alias, "email": f"{alias}@example.com"},
            headers=admin_headers,
        )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-dn-2", "ownerAlias": "owner-dn2", "label": "d"},
        headers=admin_headers,
    )
    contact_uid = users_store.get_uid_for_alias("contact-dn2")
    client.put(
        "/api/admin/allowlist",
        json={
            "entries": [
                {"fromAlias": "owner-dn2", "toAlias": "contact-dn2", "message": True, "locate": False}
            ]
        },
        headers=admin_headers,
    )
    bv_before = devcfg.get_book_version("pgr-dn-2")
    broker.clear()

    # Same displayName as before -- no book bump, no push.
    resp = client.patch(
        f"/api/admin/users/{contact_uid}",
        json={"displayName": "contact-dn2"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert devcfg.get_book_version("pgr-dn-2") == bv_before
    assert broker.published == []


# ---- settings ----


def test_put_and_get_settings(client: TestClient, admin_headers: dict[str, str]):
    resp = client.put(
        "/api/admin/settings",
        json={"messages": {"n": 2, "unit": "weeks"}, "locations": {"n": 3, "unit": "days"}},
        headers=admin_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["messages"] == {"n": 2, "unit": "weeks"}
    assert data["locations"] == {"n": 3, "unit": "days"}

    resp2 = client.get("/api/admin/settings", headers=admin_headers)
    assert resp2.json() == data


# ---- non-admin rejected ----


def test_non_admin_cannot_reach_admin_routes(client: TestClient):
    auth_user = fb_auth.create_user(email="plain@example.com")
    users_store.create_user(uid=auth_user.uid, alias="plain1", display_name="Plain")
    resp = client.get("/api/admin/users", headers=auth_header(auth_user.uid))
    assert resp.status_code == 403


# ---------------------------------------------------------------------------
# Carrier APN (app/apn_presets.py): stored on the device, carried by the typed
# setup code and the bundle, changeable by an admin.
# ---------------------------------------------------------------------------


def test_apn_presets_lists_us_mobile_dark_star(client: TestClient, admin_headers: dict[str, str]):
    resp = client.get("/api/admin/apn-presets", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    presets = {p["id"]: p for p in resp.json()}
    assert presets["us-mobile-dark-star"]["apn"] == "ereseller"
    assert presets["soracom"]["apn"] == "soracom.io"


def test_create_device_with_apn_puts_it_in_the_setup_code_and_keeps_it_for_rotation(
    client: TestClient, admin_headers: dict[str, str], monkeypatch
):
    from app import devsetup

    seen: list[str | None] = []
    real_issue = devsetup.issue

    def spy(*args, **kwargs):
        seen.append(kwargs.get("apn"))
        return real_issue(*args, **kwargs)

    monkeypatch.setattr(devsetup, "issue", spy)
    client.post(
        "/api/admin/users",
        json={"alias": "owner-apn", "displayName": "Owner", "email": "owner-apn@example.com"},
        headers=admin_headers,
    )
    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-apn1", "ownerAlias": "owner-apn", "label": "apn", "apn": " ereseller "},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["setupCode"].endswith(";apn=ereseller")
    assert devices_store.get_device("pgr-apn1").apn == "ereseller"

    resp = client.post("/api/admin/devices/pgr-apn1/rotate-credentials", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["setupCode"].endswith(";apn=ereseller")
    assert seen == ["ereseller", "ereseller"]


def test_set_device_apn_validates_and_clears(client: TestClient, admin_headers: dict[str, str]):
    client.post(
        "/api/admin/users",
        json={"alias": "owner-apn2", "displayName": "Owner", "email": "owner-apn2@example.com"},
        headers=admin_headers,
    )
    resp = client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-apn2", "ownerAlias": "owner-apn2", "label": "apn"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert ";apn=" not in resp.json()["setupCode"]
    assert devices_store.get_device("pgr-apn2").apn is None

    ok = client.put("/api/admin/devices/pgr-apn2/apn", json={"apn": "ereseller"}, headers=admin_headers)
    assert ok.status_code == 200 and ok.json() == {"apn": "ereseller"}
    assert devices_store.get_device("pgr-apn2").apn == "ereseller"

    for bad in ["has space", "semi;colon", "x" * 40, "-lead", "a..b"]:
        r = client.put("/api/admin/devices/pgr-apn2/apn", json={"apn": bad}, headers=admin_headers)
        assert r.status_code == 422, (bad, r.text)
    assert devices_store.get_device("pgr-apn2").apn == "ereseller"

    cleared = client.put("/api/admin/devices/pgr-apn2/apn", json={"apn": ""}, headers=admin_headers)
    assert cleared.status_code == 200 and cleared.json() == {"apn": None}
    assert devices_store.get_device("pgr-apn2").apn is None

    missing = client.put("/api/admin/devices/nope/apn", json={"apn": "ereseller"}, headers=admin_headers)
    assert missing.status_code == 404


# ---------------------------------------------------------------------------
# families -- docs/FAMILIES_DESIGN.md §4, docs/FAMILIES_TASKS.md 1.3.
# ---------------------------------------------------------------------------


def test_create_and_list_families(client: TestClient, admin_headers: dict[str, str]):
    resp = client.post(
        "/api/admin/families", json={"name": "The Ngs"}, headers=admin_headers
    )
    assert resp.status_code == 200, resp.text
    created = resp.json()
    assert created["name"] == "The Ngs"

    resp2 = client.get("/api/admin/families", headers=admin_headers)
    assert resp2.status_code == 200, resp2.text
    names = {f["name"] for f in resp2.json()}
    assert "The Ngs" in names


def test_patch_family_renames(client: TestClient, admin_headers: dict[str, str]):
    created = client.post(
        "/api/admin/families", json={"name": "Old"}, headers=admin_headers
    ).json()

    resp = client.patch(
        f"/api/admin/families/{created['id']}",
        json={"name": "New", "smsNumber": "+15551234567"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["name"] == "New"
    # The relay has no SMS number any more: a stale `smsNumber` is ignored.
    assert "smsNumber" not in data


def test_patch_family_missing_is_404(client: TestClient, admin_headers: dict[str, str]):
    resp = client.patch(
        "/api/admin/families/no-such-family", json={"name": "x"}, headers=admin_headers
    )
    assert resp.status_code == 404


def test_super_creates_a_family_and_moves_a_user(client: TestClient, admin_headers: dict[str, str]):
    """docs/FAMILIES_TASKS.md 1.3 Verify: "super creates a family and moves
    a user"."""
    family = client.post(
        "/api/admin/families", json={"name": "New Family"}, headers=admin_headers
    ).json()
    created = client.post(
        "/api/admin/users",
        json={"alias": "movable", "displayName": "Movable", "email": "movable@example.com"},
        headers=admin_headers,
    ).json()
    assert created["familyId"] == "default"

    resp = client.patch(
        f"/api/admin/users/{created['uid']}",
        json={"familyId": family["id"]},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["familyId"] == family["id"]

    refreshed = fb_auth.get_user(created["uid"])
    assert refreshed.custom_claims == {"role": "member", "fam": family["id"]}


def test_patch_user_role_super_is_accepted(client: TestClient, admin_headers: dict[str, str]):
    created = client.post(
        "/api/admin/users",
        json={"alias": "futuresuper", "displayName": "Future Super", "email": "fs@example.com"},
        headers=admin_headers,
    ).json()

    resp = client.patch(
        f"/api/admin/users/{created['uid']}", json={"role": "super"}, headers=admin_headers
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["role"] == "super"
    refreshed = fb_auth.get_user(created["uid"])
    assert refreshed.custom_claims == {"role": "super", "fam": "default"}


def test_create_user_with_family_id(client: TestClient, admin_headers: dict[str, str]):
    family = families_store.create_family(name="Pinned Family", created_by="root-uid")
    resp = client.post(
        "/api/admin/users",
        json={
            "alias": "famkid",
            "displayName": "Fam Kid",
            "email": "famkid@example.com",
            "familyId": family.id,
        },
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["familyId"] == family.id
    refreshed = fb_auth.get_user(resp.json()["uid"])
    assert refreshed.custom_claims == {"role": "member", "fam": family.id}


def test_create_user_defaults_family_to_callers_family(client: TestClient):
    """A real deployment's super always has a family of their own
    (`app.bootstrap` puts them in `families/default`) -- an omitted
    `familyId` must pick that up, not fall back to `None`, so a `person`
    user never ends up family-less (the bug CI's e2e caught: `tools/
    e2e_v2.py` created users with no `familyId`, which then made its
    `locate: true` allow-list `PUT` 400 with `locate_cross_family`)."""
    family = families_store.create_family(name="Caller Family", created_by="root-uid")
    caller = fb_auth.create_user(email="fam-super@example.com")
    users_store.create_user(
        uid=caller.uid,
        alias="famsuper",
        display_name="Fam Super",
        role="super",
        family_id=family.id,
    )
    fb_auth.set_custom_user_claims(caller.uid, {"role": "super", "fam": family.id})
    headers = auth_header(caller.uid)

    resp = client.post(
        "/api/admin/users",
        json={"alias": "childkid", "displayName": "Child", "email": "childkid@example.com"},
        headers=headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["familyId"] == family.id
    refreshed = fb_auth.get_user(resp.json()["uid"])
    assert refreshed.custom_claims == {"role": "member", "fam": family.id}


def test_patch_user_cannot_null_out_family_id(client: TestClient, admin_headers: dict[str, str]):
    """`PATCH /api/admin/users/{uid}` moves a `person` between families but
    must never null one out -- an explicit `"familyId": null` is
    indistinguishable from an omitted field (patch semantics) and is a
    no-op here, same as omitting it."""
    family = families_store.create_family(name="Sticky Family", created_by="root-uid")
    created = client.post(
        "/api/admin/users",
        json={
            "alias": "sticky",
            "displayName": "Sticky",
            "email": "sticky@example.com",
            "familyId": family.id,
        },
        headers=admin_headers,
    ).json()
    assert created["familyId"] == family.id

    resp = client.patch(
        f"/api/admin/users/{created['uid']}",
        json={"familyId": None},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["familyId"] == family.id
    assert users_store.get_user(created["uid"]).familyId == family.id


def test_list_users_filters_by_family_query_param(client: TestClient, admin_headers: dict[str, str]):
    family_a = families_store.create_family(name="List A", created_by="root-uid")
    family_b = families_store.create_family(name="List B", created_by="root-uid")
    client.post(
        "/api/admin/users",
        json={"alias": "usera", "displayName": "A", "email": "usera@example.com", "familyId": family_a.id},
        headers=admin_headers,
    )
    client.post(
        "/api/admin/users",
        json={"alias": "userb", "displayName": "B", "email": "userb@example.com", "familyId": family_b.id},
        headers=admin_headers,
    )

    resp = client.get(f"/api/admin/users?family={family_a.id}", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    aliases = {u["alias"] for u in resp.json()}
    assert aliases == {"usera"}


def test_list_devices_filters_by_family_query_param(client: TestClient, admin_headers: dict[str, str]):
    family_a = families_store.create_family(name="Dev List A", created_by="root-uid")
    family_b = families_store.create_family(name="Dev List B", created_by="root-uid")
    users_store.create_user(
        uid="devowner-a", alias="devownera", display_name="Owner A", family_id=family_a.id
    )
    users_store.create_user(
        uid="devowner-b", alias="devownerb", display_name="Owner B", family_id=family_b.id
    )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-filt-a", "ownerAlias": "devownera", "label": "a"},
        headers=admin_headers,
    )
    client.post(
        "/api/admin/devices",
        json={"deviceId": "pgr-filt-b", "ownerAlias": "devownerb", "label": "b"},
        headers=admin_headers,
    )

    resp = client.get(f"/api/admin/devices?family={family_a.id}", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    ids = {d["id"] for d in resp.json()}
    assert ids == {"pgr-filt-a"}


# ---- docs/CONTACT_REQ_DESIGN.md decisions 3, 5, 6 ----


def test_create_user_without_any_family_is_400_and_leaves_no_auth_user(
    client: TestClient,
):
    # A super with no family of their own and no `familyId` in the body.
    auth_user = fb_auth.create_user(email="nofam-super@example.com")
    users_store.create_user(
        uid=auth_user.uid, alias="nofamsuper", display_name="S", role="super", family_id="x"
    )
    fb_auth.set_custom_user_claims(auth_user.uid, {"role": "super", "fam": ""})

    resp = client.post(
        "/api/admin/users",
        json={"alias": "orphan", "displayName": "O", "email": "orphan@example.com"},
        headers=auth_header(auth_user.uid),
    )
    assert resp.status_code == 400, resp.text
    assert resp.json()["detail"] == "familyId is required"
    with pytest.raises(fb_auth.UserNotFoundError):
        fb_auth.get_user_by_email("orphan@example.com")
    assert users_store.get_uid_for_alias("orphan") is None


def test_create_user_normalises_phone_and_rejects_garbage(
    client: TestClient, admin_headers: dict[str, str]
):
    resp = client.post(
        "/api/admin/users",
        json={"alias": "phoney", "displayName": "P", "phone": "2065550100"},
        headers=admin_headers,
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["phone"] == "+12065550100"

    resp = client.post(
        "/api/admin/users",
        json={"alias": "phoney2", "displayName": "P", "phone": "abc"},
        headers=admin_headers,
    )
    assert resp.status_code == 400
    assert resp.json()["detail"] == "phone must be a number like +12065550100"


def test_super_family_patch_validates_the_name(client: TestClient, admin_headers: dict[str, str]):
    family = families_store.create_family(name="Orig", created_by="rootadmin")
    ok = client.patch(f"/api/admin/families/{family.id}", json={"name": " New "}, headers=admin_headers)
    assert ok.status_code == 200 and ok.json()["name"] == "New"
    for bad in ("", "y" * 41, "a\tb"):
        resp = client.patch(
            f"/api/admin/families/{family.id}", json={"name": bad}, headers=admin_headers
        )
        assert resp.status_code == 400, bad
    assert families_store.get_family(family.id).name == "New"


# ---- OTA firmware routes (docs/OTA_DESIGN.md D10, §5) ----


from tests.test_firmware import (
    BASE_URL,
    ID_NEW,
    ID_OLD,
    INDEX_URL,
    index_server,  # noqa: F401 -- fixture
)


@pytest.fixture
def ota_client(
    fake_emqx: FakeEmqxAdmin, broker: FakeBrokerClient, index_server: dict  # noqa: F811
) -> Iterator[TestClient]:
    settings = make_settings(fw_index_url=INDEX_URL, fw_bucket_base=BASE_URL)
    app = create_app(settings=settings, broker_client=broker)
    app.state.emqx_admin = fake_emqx
    with TestClient(app) as c:
        yield c


def _ota_device(device_id: str = "pgr-ota-r", *, img: str | None = None, cap: int | None = 1) -> None:
    devices_store.create_device(
        device_id=device_id,
        owner_uid="owner-ota",
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )
    fields: dict = {"state": "online", "img": img, "otaCap": cap}
    devices_store.update_status(device_id, **fields)


def test_ota_routes_503_when_not_configured(client: TestClient, admin_headers: dict[str, str]):
    _ota_device()
    assert client.get("/api/admin/firmware", headers=admin_headers).status_code == 503
    r = client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
    )
    assert r.status_code == 503
    assert r.json()["detail"] == "OTA not configured"


def test_ota_routes_502_when_index_unreachable(
    ota_client: TestClient, admin_headers: dict[str, str], index_server: dict  # noqa: F811
):
    _ota_device()
    index_server["status"] = 503
    assert ota_client.get("/api/admin/firmware", headers=admin_headers).status_code == 502
    r = ota_client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
    )
    assert r.status_code == 502


def test_ota_routes_require_super(ota_client: TestClient):
    assert ota_client.get("/api/admin/firmware").status_code in (401, 403)


def test_list_firmware_newest_first_with_delta_for_matching_device(
    ota_client: TestClient, admin_headers: dict[str, str]
):
    _ota_device(img=ID_OLD[:16])
    builds = ota_client.get(
        "/api/admin/firmware?device=pgr-ota-r", headers=admin_headers
    ).json()["builds"]
    assert [b["id16"] for b in builds] == [ID_NEW[:16], ID_OLD[:16]]
    assert builds[0] == {
        "id16": ID_NEW[:16], "version": "beta-57-gdeadbee", "size": 685168,
        "published": 1_790_000_000, "kind": "delta", "osz": 42513,
        "estBytes": int(42513 * 1.045) + 7168,
    }
    assert builds[1]["kind"] == "full"
    # no device parameter: everything is full
    plain = ota_client.get("/api/admin/firmware", headers=admin_headers).json()["builds"]
    assert {b["kind"] for b in plain} == {"full"}
    assert ota_client.get(
        "/api/admin/firmware?device=nope", headers=admin_headers
    ).status_code == 404


def test_push_ota_404s(ota_client: TestClient, admin_headers: dict[str, str]):
    _ota_device()
    r = ota_client.post(
        "/api/admin/devices/nope/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
    )
    assert r.status_code == 404
    r = ota_client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"target": "0" * 16}, headers=admin_headers
    )
    assert r.status_code == 404


def test_push_ota_409_without_gate(
    ota_client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _ota_device(cap=None)
    r = ota_client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
    )
    assert r.status_code == 409
    assert r.json()["detail"] == "device does not report ota:1"
    assert broker.published == []


def test_push_ota_409_already_running(
    ota_client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _ota_device(img=ID_NEW[:16])
    r = ota_client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
    )
    assert r.status_code == 409
    assert r.json()["detail"] == "already running that build"
    assert broker.published == []


def test_push_ota_409_already_running_img_variants(
    ota_client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    for img in (ID_NEW[:16].upper(), ID_NEW, " " + ID_NEW[:16]):
        _ota_device(img=img)
        r = ota_client.post(
            "/api/admin/devices/pgr-ota-r/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
        )
        assert r.status_code == 409, img
        assert r.json()["detail"] == "already running that build"
    assert broker.published == []


def test_push_ota_422_bad_body(ota_client: TestClient, admin_headers: dict[str, str]):
    _ota_device()
    for body in ({}, {"target": "xyz"}, {"target": ID_NEW[:16], "cancel": True}, {"cancel": False}):
        r = ota_client.post("/api/admin/devices/pgr-ota-r/ota", json=body, headers=admin_headers)
        assert r.status_code == 422, body


def test_push_ota_delta_when_img_matches_base(
    ota_client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _ota_device(img=ID_OLD[:16])
    r = ota_client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
    )
    assert r.status_code == 200, r.text
    assert r.json() == {
        "ok": True, "kind": "delta", "osz": 42513, "estBytes": int(42513 * 1.045) + 7168,
    }
    sent = json.loads(broker.published[-1].payload)
    ota = sent["cfg"]["ota"]
    assert ota["fmt"] == "delta" and ota["base"] == ID_OLD and ota["psz"] == 686466
    assert ota["url"] == BASE_URL + f"fw/{ID_NEW[:16]}/from-{ID_OLD[:16]}.dz"
    job = devices_store.get_device("pgr-ota-r").otaJob
    assert job.target16 == ID_NEW[:16] and job.kind == "delta" and job.osz == 42513
    assert job.estBytes == r.json()["estBytes"] and job.by_uid and job.at is not None


def test_push_ota_full_otherwise_then_cancel(
    ota_client: TestClient, admin_headers: dict[str, str], broker: FakeBrokerClient
):
    _ota_device(img="9" * 16)
    r = ota_client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"target": ID_NEW[:16]}, headers=admin_headers
    )
    assert r.status_code == 200, r.text
    assert r.json()["kind"] == "full" and r.json()["osz"] == 338784
    assert json.loads(broker.published[-1].payload)["cfg"]["ota"]["fmt"] == "full"
    assert devices_store.get_device("pgr-ota-r").otaJob is not None

    r = ota_client.post(
        "/api/admin/devices/pgr-ota-r/ota", json={"cancel": True}, headers=admin_headers
    )
    assert r.status_code == 200 and r.json() == {"ok": True}
    assert json.loads(broker.published[-1].payload)["cfg"] == {"ota": {"cancel": True}}
    assert devices_store.get_device("pgr-ota-r").otaJob is None
