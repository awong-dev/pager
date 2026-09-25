"""`GET /api/device/book` -- docs/PROTOCOL.md §3.7 (book pull, v0.4) /
§14.7 (request authentication, response signature). `app/routers/
device_book.py`'s own docstring has the endpoint's design notes; this file
is the pytest-against-the-real-app counterpart, same split
tests/test_devcfg.py uses for the admin `/cfg` route.
"""

from __future__ import annotations

import base64
import json
import time
from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient

from app import devauth, wirecbor
from app.config import Settings
from app.ingest import Ingest
from app.main import create_app
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import conversations as conversations_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import users as users_store
from tests.conftest import up_topic
from tests.fake_transport import FakeBrokerClient

_HMAC_KEY = b"k" * 32


def _make_user(uid: str, alias: str, display_name: str | None = None) -> None:
    users_store.create_user(uid=uid, alias=alias, display_name=display_name or alias)


def _make_hmac_pager_device(
    device_id: str, owner_uid: str, *, key: bytes = _HMAC_KEY, default_to_uid: str | None = None
) -> bytes:
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        default_to_uid=default_to_uid,
        auth_mode="hmac",
    )
    backends_store.create_backend(
        owner_uid, kind="pager", config={"deviceId": device_id}, enabled=True
    )
    device_secrets_store.create(device_id, hmac_key=key, mqtt_password_hash="y")
    return key


def _approve(owner_uid: str, contact_uid: str) -> None:
    allow_store.set_edge(owner_uid, contact_uid, message=True, locate=False)
    allow_store.set_edge(contact_uid, owner_uid, message=True, locate=False)


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
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def client(broker: FakeBrokerClient) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        yield c


def _headers(
    key: bytes,
    device_id: str,
    n: int,
    bv: int,
    *,
    bad_sig: bool = False,
) -> dict[str, str]:
    tag = devauth.request_tag(key, device_id, n, bv)
    if bad_sig:
        tag = bytes(b ^ 0xFF for b in tag)
    sig = base64.urlsafe_b64encode(tag).rstrip(b"=").decode("ascii")
    return {"X-Device-Id": device_id, "X-N": str(n), "X-Sig": sig}


def _get(client: TestClient, device_id: str, key: bytes, n: int, bv: int, **kw):
    return client.get(
        "/api/device/book", params={"bv": str(bv)}, headers=_headers(key, device_id, n, bv, **kw)
    )


def _decode_body(key: bytes, content: bytes) -> dict:
    ok, unsigned = devauth.verify(key, "/api/device/book", content)
    assert ok, "response failed to verify against the device's own key"
    return wirecbor.decode(unsigned)


# ---------------------------------------------------------------------------
# happy path / contact ordering / truncation
# ---------------------------------------------------------------------------


def test_device_book_serves_20_contacts(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-20", "student-db20")
    _make_user("student-db20", "student-db20")
    for i in range(20):
        uid = f"dbcontact20_{i}"
        _make_user(uid, f"dbc20{i:02d}")
        _approve("student-db20", uid)

    resp = _get(client, "pgr-db-20", key, 1, 0)

    assert resp.status_code == 200, resp.text
    body = _decode_body(key, resp.content)
    assert len(body["c"]) == 20
    assert "more" not in body


def test_device_book_truncates_at_32_sets_more(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-40", "student-db40")
    _make_user("student-db40", "student-db40")
    for i in range(40):
        uid = f"dbcontact40_{i}"
        _make_user(uid, f"dbc40{i:03d}")
        _approve("student-db40", uid)

    resp = _get(client, "pgr-db-40", key, 1, 0)

    assert resp.status_code == 200, resp.text
    assert len(resp.content) < 4096
    body = _decode_body(key, resp.content)
    assert len(body["c"]) == 32
    assert body["more"] is True


def test_device_book_default_first_then_groups(client: TestClient):
    _make_user("student-db-order", "student-db-order")
    _make_user("zzz-mom-db", "zzz-mom-db", "Zzz Mom")
    key = _make_hmac_pager_device(
        "pgr-db-order", "student-db-order", default_to_uid="zzz-mom-db"
    )
    _approve("student-db-order", "zzz-mom-db")
    _make_user("aaa-friend-db", "aaa-friend-db", "Aaa Friend")
    _approve("student-db-order", "aaa-friend-db")
    conversations_store.create_group(
        name="Bbb Group",
        alias="bbb-grp-db",
        member_uids=["student-db-order", "aaa-friend-db"],
        created_by="student-db-order",
    )

    resp = _get(client, "pgr-db-order", key, 1, 0)

    assert resp.status_code == 200, resp.text
    body = _decode_body(key, resp.content)
    assert body["d"] == "zzz-mom-db"
    aliases_in_order = [c["a"] for c in body["c"]]
    assert aliases_in_order[0] == "zzz-mom-db"
    assert aliases_in_order[1] == "bbb-grp-db"
    assert aliases_in_order[2] == "aaa-friend-db"


# ---------------------------------------------------------------------------
# §14.7's ordered error path
# ---------------------------------------------------------------------------


def test_device_book_malformed_headers_400(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-malformed", "std-malform")
    _make_user("std-malform", "std-malform")

    # Missing bv query param entirely.
    resp = client.get(
        "/api/device/book", headers=_headers(key, "pgr-db-malformed", 1, 0)
    )
    assert resp.status_code == 400

    # bv out of range (2**32).
    resp = client.get(
        "/api/device/book",
        params={"bv": str(2**32)},
        headers=_headers(key, "pgr-db-malformed", 1, 0),
    )
    assert resp.status_code == 400

    # X-N with a leading zero -- §14.7: "decimal, no leading zeros."
    headers = _headers(key, "pgr-db-malformed", 1, 0)
    headers["X-N"] = "01"
    resp = client.get("/api/device/book", params={"bv": "0"}, headers=headers)
    assert resp.status_code == 400

    # X-Sig the wrong length.
    headers = _headers(key, "pgr-db-malformed", 1, 0)
    headers["X-Sig"] = "short"
    resp = client.get("/api/device/book", params={"bv": "0"}, headers=headers)
    assert resp.status_code == 400

    # Missing X-Device-Id.
    resp = client.get(
        "/api/device/book", params={"bv": "0"}, headers={"X-N": "1", "X-Sig": "a" * 11}
    )
    assert resp.status_code == 400


def test_device_book_unknown_device_404(client: TestClient):
    resp = _get(client, "no-such-device", _HMAC_KEY, 1, 0)
    assert resp.status_code == 404


def test_device_book_revoked_404(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-revoked", "student-revoked")
    _make_user("student-revoked", "student-revoked")
    devices_store.revoke_device("pgr-db-revoked")

    resp = _get(client, "pgr-db-revoked", key, 1, 0)
    assert resp.status_code == 404


def test_device_book_not_hmac_404(client: TestClient):
    devices_store.create_device(
        device_id="pgr-db-password",
        owner_uid="std-db-pw",
        label="d",
        mqtt_username="pgr-db-password",
        mqtt_password_hash="x",
        auth_mode="password",
    )
    _make_user("std-db-pw", "std-db-pw")

    resp = _get(client, "pgr-db-password", _HMAC_KEY, 1, 0)
    assert resp.status_code == 404


def test_device_book_bad_sig_401_and_counts_failure(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-badsig", "student-badsig")
    _make_user("student-badsig", "student-badsig")

    resp = _get(client, "pgr-db-badsig", key, 1, 0, bad_sig=True)

    assert resp.status_code == 401
    assert device_secrets_store.get("pgr-db-badsig").sigFailures == 1
    # upN must be untouched by a bad-signature request -- it fails before
    # step 4's counter transaction ever runs.
    assert device_secrets_store.get("pgr-db-badsig").upN == 0


def test_device_book_replayed_n_409(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-replay", "student-replay")
    _make_user("student-replay", "student-replay")

    resp1 = _get(client, "pgr-db-replay", key, 5, 0)
    assert resp1.status_code == 200
    up_n_after_first = device_secrets_store.get("pgr-db-replay").upN
    assert up_n_after_first == 5

    resp2 = _get(client, "pgr-db-replay", key, 5, 0)
    assert resp2.status_code == 409
    assert device_secrets_store.get("pgr-db-replay").upN == up_n_after_first


def test_device_book_n_below_upn_409_even_inside_window(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-window", "student-window")
    _make_user("student-window", "student-window")
    assert device_secrets_store.accept_up_n("pgr-db-window", 100) is True

    resp = _get(client, "pgr-db-window", key, 99, 0)

    assert resp.status_code == 409
    assert device_secrets_store.get("pgr-db-window").upN == 100


def test_device_book_consumes_counter(client: TestClient, broker: FakeBrokerClient):
    key = _make_hmac_pager_device("pgr-db-consume", "student-consume")
    _make_user("student-consume", "student-consume")

    resp = _get(client, "pgr-db-consume", key, 200, 0)
    assert resp.status_code == 200
    assert device_secrets_store.get("pgr-db-consume").upN == 200

    ingest = Ingest(broker)
    up_t = up_topic("pgr-db-consume")

    # n=201 > upN(200) -> accepted, becomes the new top.
    ingest.handle_up(
        up_t,
        devauth.sign_json(
            key, up_t, {"v": 1, "id": "m_c1", "ts": int(time.time()), "ack": "shown", "n": 201}
        ),
    )
    assert device_secrets_store.get("pgr-db-consume").upN == 201

    # n=190 is inside the 64-wide window below 201 and has never been seen
    # -> accepted, but does not become the new top.
    ingest.handle_up(
        up_t,
        devauth.sign_json(
            key, up_t, {"v": 1, "id": "m_c2", "ts": int(time.time()), "ack": "shown", "n": 190}
        ),
    )
    assert device_secrets_store.get("pgr-db-consume").upN == 201

    # n=200 was already consumed by the fetch above -> replay, rejected;
    # neither upN nor upBits move.
    before = device_secrets_store.get("pgr-db-consume")
    ingest.handle_up(
        up_t,
        devauth.sign_json(
            key, up_t, {"v": 1, "id": "m_c3", "ts": int(time.time()), "ack": "shown", "n": 200}
        ),
    )
    after = device_secrets_store.get("pgr-db-consume")
    assert after.upN == before.upN
    assert after.upBits == before.upBits


# ---------------------------------------------------------------------------
# response shape
# ---------------------------------------------------------------------------


def test_device_book_response_signature_verifies(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-sig", "student-sig")
    _make_user("student-sig", "student-sig")

    resp = _get(client, "pgr-db-sig", key, 7, 0)

    assert resp.status_code == 200
    ok, unsigned = devauth.verify(key, "/api/device/book", resp.content)
    assert ok
    body = wirecbor.decode(unsigned)
    assert body["n"] == 7


def test_device_book_cache_control_no_store(client: TestClient):
    key = _make_hmac_pager_device("pgr-db-cache", "student-cache")
    _make_user("student-cache", "student-cache")

    resp = _get(client, "pgr-db-cache", key, 1, 0)

    assert resp.status_code == 200
    assert resp.headers["cache-control"] == "private, no-store"
    assert resp.headers["content-type"].startswith("application/cbor")


# ---------------------------------------------------------------------------
# §14.7 domain separation -- a request tag is never valid as an envelope tag.
# ---------------------------------------------------------------------------


def test_request_tag_not_valid_as_envelope_tag():
    key = b"k" * 32
    device_id = "pgr-domain1"
    n = 42
    bv = 7
    topic = f"pager/{device_id}/up"

    # A real signed envelope over the MQTT `/up` topic.
    envelope = devauth.sign_json(
        key, topic, {"v": 1, "id": "m_dom1", "ts": 1_700_000_000, "ack": "shown", "n": n}
    )
    ok, _unsigned = devauth.verify(key, topic, envelope)
    assert ok

    # That same envelope never verifies against the book-pull request's
    # topic-substitute string -- no MQTT topic contains a space, so the two
    # domains can never collide.
    assert devauth.verify(key, "GET /api/device/book", envelope)[0] is False

    # And the request tag computed for this same device/n/bv never equals
    # the envelope's own signature.
    req_tag = devauth.request_tag(key, device_id, n, bv)
    req_tag_b64 = base64.urlsafe_b64encode(req_tag).rstrip(b"=").decode("ascii")
    envelope_sig_b64 = json.loads(envelope)["sig"]
    assert req_tag_b64 != envelope_sig_b64
