"""Shared fixtures-as-functions for the bridge-phone tests
(docs/BRIDGE_PHONE_DESIGN.md)."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass, field

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import alerts as alerts_module
from app.config import Settings
from app.main import create_app
from app.store import bridges as bridges_store
from app.store import devices as devices_store
from app.store import families as families_store
from app.store import push_tokens as push_tokens_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

WEBHOOK_KEY = "test-webhook-key"
SIM = "+12065550100"
VOICE = "+12065550199"
MOM = "+12065550111"


def make_settings() -> Settings:
    return Settings(
        broker_api_url="http://unused.invalid/api/v5",
        broker_api_key=None,
        broker_api_secret=None,
        webhook_key=WEBHOOK_KEY,
        dev_mode=False,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
    )


@dataclass
class RecordingFCM:
    calls: list[tuple[list[str], dict[str, str]]] = field(default_factory=list)

    def send_data(self, tokens: list[str], data: dict[str, str]) -> None:
        self.calls.append((tokens, data))


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient(webhook_key=WEBHOOK_KEY)


@pytest.fixture
def fcm() -> Iterator[RecordingFCM]:
    fake = RecordingFCM()
    previous = alerts_module.get_fcm_client()
    alerts_module.set_fcm_client(fake)
    yield fake
    alerts_module.set_fcm_client(previous)


@pytest.fixture
def client(broker: FakeBrokerClient, fcm: RecordingFCM) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        alerts_module.set_fcm_client(fcm)
        yield c


@dataclass
class World:
    family_id: str
    admin_headers: dict[str, str]


@pytest.fixture
def world(client: TestClient) -> World:
    """A family with an admin (one push token) and the member `kid`."""
    family = families_store.create_family(name="F", created_by="root")
    fb_auth.create_user(uid="adm", email="adm@example.com")
    users_store.create_user(
        uid="adm", alias="adm", display_name="adm", role="admin", family_id=family.id
    )
    fb_auth.set_custom_user_claims("adm", {"role": "admin", "fam": family.id})
    push_tokens_store.add_token("adm", "tok-adm")
    users_store.create_user(uid="kid", alias="kid", display_name="Kid", family_id=family.id)
    return World(family_id=family.id, admin_headers=auth_header("adm"))


def make_pager_device(device_id: str, owner_uid: str) -> None:
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
    )


def pair_bridge(
    client: TestClient,
    world: World,
    *,
    owner: str = "kid",
    sim: str | None = SIM,
    voice: str | None = None,
    caps: dict | None = None,
) -> tuple[bridges_store.Bridge, dict[str, str]]:
    """Creates a bridge row + code directly in the store and pairs it
    through `/bridge/pair`; returns the bridge and its bearer headers."""
    bridge = bridges_store.create(owner, world.family_id, "phone", "adm")
    code, _ = bridges_store.create_pair_code(bridge.id)
    body: dict = {
        "code": code,
        "version": "1.0",
        "accounts": ["kid@example.com"],
        "caps": caps or {"sms": True, "gchat": True, "gvoice": True},
    }
    if sim:
        body["simNumber"] = sim
    if voice:
        body["voiceNumber"] = voice
    resp = client.post("/bridge/pair", json=body)
    assert resp.status_code == 200, resp.text
    fresh = bridges_store.get(bridge.id)
    assert fresh is not None
    return fresh, {"Authorization": f"Bearer {resp.json()['token']}"}
