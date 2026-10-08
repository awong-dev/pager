"""Per-user Twilio SMS -- docs/RELAY_SMS_DESIGN.md: `app/backends/sms_twilio.py`,
`app/notify/sms.py`, `POST /webhooks/twilio/sms`, held texts and their approval.

`EXPECTED_SIGNATURE` was computed independently of this codebase (a
standalone `hmac`/`hashlib`/`base64` script) against Twilio's documented
algorithm, so a bug in `compute_twilio_signature` fails here rather than
passing against itself. Every webhook test goes through the real route
(`TestClient` -> `verify_twilio_signature` -> `Routing.send()`); only the
outbound Twilio call (`app.notify.sms.send_sms`) and FCM are stubbed.
"""

from __future__ import annotations

import logging
from collections.abc import Iterator
from dataclasses import dataclass, field

import httpx
import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import alerts as alerts_module
from app import jobs
from app.backends import sms_twilio
from app.backends.sms_twilio import SmsTwilioBackend
from app.config import Settings
from app.db.firestore import get_db
from app.main import create_app
from app.notify import sms as sms_client
from app.notify.sms import TwilioSendResult
from app.routing import Routing
from app.store import alerts as alerts_store
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import held_sms as held_sms_store
from app.store import messages as messages_store
from app.store import push_tokens as push_tokens_store
from app.store import users as users_store
from app.tasks import InlineTaskQueue
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

WEBHOOK_KEY = "test-webhook-key"
AUTH_TOKEN = "test-auth-token-xyz"
PUBLIC_URL = "https://relay.example.com"
WEBHOOK_URL = f"{PUBLIC_URL}/webhooks/twilio/sms"

KID_NUMBER = "+12065550100"
MOM_NUMBER = "+12065550111"
STRANGER = "+12065550222"

FIXTURE_PARAMS = {"To": "+15005550006", "From": "+15551234567", "Body": "hello there"}
EXPECTED_SIGNATURE = "yO/lPv1oXJR8brVrVg3Zqrl2J5w="


# ---------------------------------------------------------------------------
# pure signature checks
# ---------------------------------------------------------------------------


def test_compute_twilio_signature_matches_known_fixture():
    assert (
        sms_twilio.compute_twilio_signature(WEBHOOK_URL, FIXTURE_PARAMS, AUTH_TOKEN)
        == EXPECTED_SIGNATURE
    )


def test_verify_twilio_signature_accepts_valid_and_rejects_tampering():
    verify = sms_twilio.verify_twilio_signature
    assert verify(WEBHOOK_URL, FIXTURE_PARAMS, EXPECTED_SIGNATURE, AUTH_TOKEN)
    assert not verify(WEBHOOK_URL, FIXTURE_PARAMS, EXPECTED_SIGNATURE, "wrong-token")
    assert not verify(
        WEBHOOK_URL, {**FIXTURE_PARAMS, "Body": "hello there!"}, EXPECTED_SIGNATURE, AUTH_TOKEN
    )
    assert not verify(WEBHOOK_URL + "x", FIXTURE_PARAMS, EXPECTED_SIGNATURE, AUTH_TOKEN)
    assert not verify(WEBHOOK_URL, FIXTURE_PARAMS, None, AUTH_TOKEN)
    assert not verify(WEBHOOK_URL, FIXTURE_PARAMS, EXPECTED_SIGNATURE, "")
    # A non-ASCII forged header is a plain False, not a TypeError.
    assert not verify(WEBHOOK_URL, FIXTURE_PARAMS, "caf\xe9", AUTH_TOKEN)


# ---------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------


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


@dataclass
class OutboundSms:
    sent: list[dict] = field(default_factory=list)
    result: TwilioSendResult = field(default_factory=lambda: TwilioSendResult(ok=True, sid="SMout"))


@pytest.fixture(autouse=True)
def _twilio_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("TWILIO_AUTH_TOKEN", AUTH_TOKEN)
    monkeypatch.setenv("PUBLIC_BASE_URL", PUBLIC_URL)
    monkeypatch.delenv("TWILIO_BASE_URL", raising=False)


@pytest.fixture
def outbound(monkeypatch: pytest.MonkeyPatch) -> OutboundSms:
    out = OutboundSms()

    def fake_send(to: str, body: str, *, from_number: str) -> TwilioSendResult:
        out.sent.append({"to": to, "body": body, "from": from_number})
        return out.result

    monkeypatch.setattr(sms_client, "send_sms", fake_send)
    return out


@pytest.fixture
def fcm() -> Iterator[RecordingFCM]:
    fake = RecordingFCM()
    previous = alerts_module.get_fcm_client()
    alerts_module.set_fcm_client(fake)
    yield fake
    alerts_module.set_fcm_client(previous)


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient(webhook_key=WEBHOOK_KEY)


@pytest.fixture
def client(broker: FakeBrokerClient, outbound: OutboundSms, fcm: RecordingFCM) -> Iterator[TestClient]:
    app = create_app(settings=make_settings(), broker_client=broker)
    with TestClient(app) as c:
        # Startup resets the alert push client; install the recorder after it.
        alerts_module.set_fcm_client(fcm)
        yield c


@dataclass
class World:
    family_id: str
    admin_headers: dict[str, str]
    contact_uid: str | None = None


def _set_policy(uid: str, out: str, in_: str) -> None:
    get_db().collection("users").document(uid).update({"policy": {"out": out, "in": in_}})


@pytest.fixture
def world(client: TestClient) -> World:
    """A family with an admin (one push token) and the member `kid` holding
    `KID_NUMBER`, whose inbound policy lets numbers in once approved."""
    family = families_store.create_family(name="F", created_by="root")
    fb_auth.create_user(uid="adm", email="adm@example.com")
    users_store.create_user(
        uid="adm", alias="adm", display_name="adm", role="admin", family_id=family.id
    )
    fb_auth.set_custom_user_claims("adm", {"role": "admin", "fam": family.id})
    push_tokens_store.add_token("adm", "tok-adm")
    users_store.create_user(uid="kid", alias="kid", display_name="Kid", family_id=family.id)
    _set_policy("kid", "people_sms", "people_sms")
    users_store.set_sms_number("kid", KID_NUMBER)
    return World(family_id=family.id, admin_headers=auth_header("adm"))


def _approved_contact(world: World, phone: str = MOM_NUMBER, name: str = "Mom"):
    ext = externals_store.get_or_create(world.family_id, phone, name)
    allow_store.set_edge("kid", ext.uid, message=True, locate=False)
    return ext


def _post(client: TestClient, params: dict[str, str], *, signature: object = ...):
    sig = (
        sms_twilio.compute_twilio_signature(WEBHOOK_URL, params, AUTH_TOKEN)
        if signature is ...
        else signature
    )
    headers = {} if sig is None else {"X-Twilio-Signature": str(sig)}
    return client.post("/webhooks/twilio/sms", data=params, headers=headers)


_sid_counter = 0


def _inbound(
    client: TestClient,
    body: str,
    *,
    from_: str = MOM_NUMBER,
    to: str = KID_NUMBER,
    sid: str | None = None,
    **extra: str,
):
    global _sid_counter
    _sid_counter += 1
    params = {
        "To": to,
        "From": from_,
        "Body": body,
        "MessageSid": sid or f"SM{_sid_counter:032d}",
        **extra,
    }
    return _post(client, params)


def _outcome(caplog: pytest.LogCaptureFixture) -> str:
    lines = [r.getMessage() for r in caplog.records if r.getMessage().startswith("sms in ")]
    assert lines, "no 'sms in' log line"
    return lines[-1].rsplit("outcome=", 1)[1]


def _kid_inbox() -> list:
    return [m for m in get_db().collection("messages").stream() if m.to_dict()["recipientUid"] == "kid"]


def _open_alerts(world: World):
    return alerts_store.list_alerts(world.family_id, "open")


# ---------------------------------------------------------------------------
# webhook: auth and the step table
# ---------------------------------------------------------------------------


def test_webhook_missing_or_wrong_signature_is_401(client: TestClient, world: World):
    params = {"To": KID_NUMBER, "From": MOM_NUMBER, "Body": "hi", "MessageSid": "SMx"}
    assert _post(client, params, signature=None).status_code == 401
    assert _post(client, params, signature="nope").status_code == 401
    assert _kid_inbox() == []


def test_webhook_fails_closed_without_auth_token(
    client: TestClient, world: World, monkeypatch: pytest.MonkeyPatch
):
    params = {"To": KID_NUMBER, "From": MOM_NUMBER, "Body": "hi", "MessageSid": "SMx"}
    sig = sms_twilio.compute_twilio_signature(WEBHOOK_URL, params, "")
    monkeypatch.setenv("TWILIO_AUTH_TOKEN", "")
    assert _post(client, params, signature=sig).status_code == 401


def test_dropped_unknown_to(client: TestClient, world: World, caplog):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    assert _inbound(client, "hi", to="+12065559999").status_code == 200
    assert _outcome(caplog) == "dropped_unknown_to"
    # A number held by a disabled user is also unknown.
    users_store.update_user("kid", disabled=True)
    assert _inbound(client, "hi").status_code == 200
    assert _outcome(caplog) == "dropped_unknown_to"
    assert get_db().collection("heldSms").get() == []


def test_dropped_bad_from(client: TestClient, world: World, caplog):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    assert _inbound(client, "hi", from_="not-a-number").status_code == 200
    assert _outcome(caplog) == "dropped_bad_from"


def test_blocked_number_stores_nothing(client: TestClient, world: World, caplog):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    families_store.add_blocked_number(world.family_id, STRANGER)
    assert _inbound(client, "buy now", from_=STRANGER).status_code == 200
    assert _outcome(caplog) == "blocked"
    assert get_db().collection("heldSms").get() == []
    assert _open_alerts(world) == []


def test_known_approved_contact_is_delivered_with_wire_id(
    client: TestClient, world: World, caplog
):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    ext = _approved_contact(world)
    assert _inbound(client, "hi from mom", sid="SMdeliver1").status_code == 200
    assert _outcome(caplog) == "delivered"
    (msg,) = messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))
    assert msg.body == "hi from mom"
    assert msg.senderUid == ext.uid and msg.recipientUid == "kid"
    assert msg.originBackendKind == "sms"
    assert msg.originBackendId == externals_store.ensure_sms_backend(ext)
    assert messages_store.wire_id_exists("SMdeliver1", "kid")
    assert get_db().collection("heldSms").get() == []
    # No FCM alert push (an alert push carries alertKind), no reply.
    # A retry of the same sid is a duplicate and creates nothing.
    assert _inbound(client, "hi from mom", sid="SMdeliver1").status_code == 200
    assert _outcome(caplog) == "duplicate"
    assert len(messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))) == 1


def test_inbound_any_delivers_a_known_contact_without_an_edge(client: TestClient, world: World):
    ext = externals_store.get_or_create(world.family_id, MOM_NUMBER, "Mom")
    _set_policy("kid", "people_sms", "any")
    assert _inbound(client, "no edge needed").status_code == 200
    assert len(messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))) == 1


def test_unknown_number_even_under_any_is_held_not_delivered(
    client: TestClient, world: World, caplog
):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    _set_policy("kid", "people_sms", "any")
    assert _inbound(client, "who dis", from_=STRANGER).status_code == 200
    assert _outcome(caplog) == "held"
    assert _kid_inbox() == []


@pytest.mark.parametrize("scenario", ["no_contact", "contact_without_edge", "rule_none"])
def test_otherwise_is_held(client: TestClient, world: World, caplog, scenario: str):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    if scenario == "contact_without_edge":
        externals_store.get_or_create(world.family_id, MOM_NUMBER, "Mom")
    elif scenario == "rule_none":
        _approved_contact(world)
        _set_policy("kid", "people_sms", "people")  # numbers: none
    assert _inbound(client, "hello").status_code == 200
    assert _outcome(caplog) == "held"
    assert _kid_inbox() == []
    (row,) = held_sms_store.list_held(world.family_id, MOM_NUMBER, "kid")
    assert row.body == "hello" and row.status == "held" and row.alertId


def test_photo_without_body_becomes_photo_marker(client: TestClient, world: World):
    ext = _approved_contact(world)
    assert _inbound(client, "", NumMedia="1").status_code == 200
    (msg,) = messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))
    assert msg.body == "[photo]"


def test_too_long_from_known_contact_replies_with_hint_and_stores_nothing(
    client: TestClient, world: World, outbound: OutboundSms, caplog
):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    ext = _approved_contact(world)
    assert _inbound(client, "x" * 161).status_code == 200
    assert _outcome(caplog) == "too_long"
    assert messages_store.list_thread(messages_store.conv_key(ext.uid, "kid")) == []
    assert len(outbound.sent) == 1
    assert outbound.sent[0]["to"] == MOM_NUMBER and outbound.sent[0]["from"] == KID_NUMBER
    assert outbound.sent[0]["body"] == "Message too long (max 160 characters) -- not sent."


def test_too_long_from_unknown_number_is_held_without_reply(
    client: TestClient, world: World, outbound: OutboundSms
):
    assert _inbound(client, "y" * 400, from_=STRANGER).status_code == 200
    assert outbound.sent == []
    (row,) = held_sms_store.list_held(world.family_id, STRANGER, "kid")
    assert len(row.body) == 400


# ---------------------------------------------------------------------------
# held texts: one alert per number, a push per text, cap
# ---------------------------------------------------------------------------


def test_two_held_texts_make_one_alert_and_two_pushes(
    client: TestClient, world: World, fcm: RecordingFCM
):
    assert _inbound(client, "first one", from_=STRANGER).status_code == 200
    assert _inbound(client, "second one", from_=STRANGER).status_code == 200
    (alert,) = _open_alerts(world)
    assert alert.kind == "sms_unknown" and alert.subjectUid == "kid"
    assert alert.peerPhone == STRANGER
    assert alert.heldCount == 2 and alert.preview == "second one"
    assert alert.updatedAt is not None
    bodies = [data["body"] for _tokens, data in fcm.calls]
    assert bodies == [f"{STRANGER} → @kid: first one", f"{STRANGER} → @kid: second one"]
    assert all(data["alertKind"] == "sms_unknown" for _t, data in fcm.calls)
    # A different number is a different alert.
    assert _inbound(client, "other", from_="+12065550333").status_code == 200
    assert len(_open_alerts(world)) == 2


def test_held_cap_drops_the_26th_without_a_push(
    client: TestClient, world: World, fcm: RecordingFCM, caplog
):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    for i in range(held_sms_store.HELD_CAP):
        assert held_sms_store.create(
            f"SMpre{i}", family_id=world.family_id, to_uid="kid", from_phone=STRANGER, body="x"
        )
    assert _inbound(client, "one too many", from_=STRANGER).status_code == 200
    assert _outcome(caplog) == "held_cap"
    assert held_sms_store.count_held(world.family_id, STRANGER, "kid") == held_sms_store.HELD_CAP
    assert fcm.calls == []


def test_held_duplicate_sid_is_not_stored_or_pushed_twice(
    client: TestClient, world: World, fcm: RecordingFCM, caplog
):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    assert _inbound(client, "once", from_=STRANGER, sid="SMdup").status_code == 200
    assert _inbound(client, "once", from_=STRANGER, sid="SMdup").status_code == 200
    assert _outcome(caplog) == "duplicate"
    assert held_sms_store.count_held(world.family_id, STRANGER, "kid") == 1
    assert len(fcm.calls) == 1


# ---------------------------------------------------------------------------
# approve / block / dismiss
# ---------------------------------------------------------------------------


def _hold_two(client: TestClient) -> None:
    assert _inbound(client, "first", from_=STRANGER, sid="SMa1").status_code == 200
    assert _inbound(client, "second", from_=STRANGER, sid="SMa2").status_code == 200


def test_get_held_lists_rows_oldest_first_with_iso_timestamps(client: TestClient, world: World):
    _hold_two(client)
    (alert,) = _open_alerts(world)
    resp = client.get(f"/api/family/alerts/{alert.id}/held", headers=world.admin_headers)
    assert resp.status_code == 200, resp.text
    held = resp.json()["held"]
    assert [h["body"] for h in held] == ["first", "second"]
    assert [h["status"] for h in held] == ["held", "held"]
    assert held[0]["id"] == "SMa1" and isinstance(held[0]["receivedAt"], str)
    assert held[0]["receivedAt"].endswith("Z") or "+00:00" in held[0]["receivedAt"]


def test_approve_delivers_backlog_in_order_and_is_idempotent(client: TestClient, world: World):
    _hold_two(client)
    (alert,) = _open_alerts(world)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"name": "Neighbor"}, headers=world.admin_headers
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["delivered"] == 2 and body["undelivered"] == 0
    assert body["alert"]["status"] == "handled"

    ext = externals_store.get_family_contact(world.family_id, STRANGER)
    assert ext is not None and allow_store.is_message_allowed("kid", ext.uid)
    key = messages_store.conv_key(ext.uid, "kid")
    assert [m.body for m in messages_store.list_thread(key)] == ["first", "second"]
    rows = held_sms_store.list_held(world.family_id, STRANGER, "kid", status=None)
    assert [r.status for r in rows] == ["delivered", "delivered"]

    # Crash-retry: put the rows and the alert back and approve again -- the
    # wire_id dedup (the Twilio sid) keeps the thread at two messages.
    held_sms_store.set_status(["SMa1", "SMa2"], "held")
    get_db().collection("families").document(world.family_id).collection("alerts").document(
        alert.id
    ).update({"status": "open"})
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"name": "Neighbor"}, headers=world.admin_headers
    )
    assert resp.status_code == 200, resp.text
    assert [m.body for m in messages_store.list_thread(key)] == ["first", "second"]


def test_approve_over_160_marks_too_long_and_delivers_the_rest(client: TestClient, world: World):
    assert _inbound(client, "z" * 200, from_=STRANGER, sid="SMb1").status_code == 200
    assert _inbound(client, "short", from_=STRANGER, sid="SMb2").status_code == 200
    (alert,) = _open_alerts(world)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"name": "N"}, headers=world.admin_headers
    )
    assert resp.json()["delivered"] == 1
    rows = {r.id: r.status for r in held_sms_store.list_held(world.family_id, STRANGER, None, status=None)}
    assert rows == {"SMb1": "too_long", "SMb2": "delivered"}


def test_approve_refused_when_inbound_policy_is_none_and_writes_nothing(
    client: TestClient, world: World
):
    _hold_two(client)
    _set_policy("kid", "people_sms", "people")  # numbers: none
    (alert,) = _open_alerts(world)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"name": "N"}, headers=world.admin_headers
    )
    assert resp.status_code == 409, resp.text
    assert "inbound policy does not allow numbers" in resp.json()["detail"]
    assert externals_store.get_family_contact(world.family_id, STRANGER) is None
    assert get_db().collection("messages").get() == []
    assert alerts_store.get(world.family_id, alert.id).status == "open"
    assert [r.status for r in held_sms_store.list_held(world.family_id, STRANGER, "kid", status=None)] == [
        "held",
        "held",
    ]


def test_block_marks_rows_blocked_and_blocks_the_number(client: TestClient, world: World, caplog):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    _hold_two(client)
    (alert,) = _open_alerts(world)
    resp = client.post(f"/api/family/alerts/{alert.id}/block", json={}, headers=world.admin_headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"
    rows = held_sms_store.list_held(world.family_id, STRANGER, None, status=None)
    assert [r.status for r in rows] == ["blocked", "blocked"]
    assert _inbound(client, "again", from_=STRANGER).status_code == 200
    assert _outcome(caplog) == "blocked"


def test_dismiss_marks_rows_dismissed(client: TestClient, world: World):
    _hold_two(client)
    (alert,) = _open_alerts(world)
    resp = client.post(f"/api/family/alerts/{alert.id}/dismiss", json={}, headers=world.admin_headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "dismissed"
    rows = held_sms_store.list_held(world.family_id, STRANGER, None, status=None)
    assert [r.status for r in rows] == ["dismissed", "dismissed"]
    assert _kid_inbox() == []


# ---------------------------------------------------------------------------
# outbound: pager/web -> contact
# ---------------------------------------------------------------------------


def _routing(broker: FakeBrokerClient) -> Routing:
    return Routing(broker)


def test_pager_up_to_contact_goes_out_from_the_owners_number(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms, caplog
):
    caplog.set_level(logging.INFO, logger="relay.backends.sms_twilio")
    ext = _approved_contact(world)
    result = _routing(broker).send(
        sender_uid="kid",
        recipient_alias=ext.alias,
        kind="text",
        body="on my way",
        origin_backend_kind="pager",
        wire_id="u_out1",
    )
    assert result.rejected == []
    assert outbound.sent == [{"to": MOM_NUMBER, "body": "on my way", "from": KID_NUMBER}]
    (msg,) = result.messages
    deliveries = messages_store.get_message(msg.id).deliveries
    assert [(d.kind, d.state) for d in deliveries.values()] == [("sms", "sent")]
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("sms out"))
    assert line == "sms out to=...0111 from=...0100 sid=SMout status=sent code=-"
    assert MOM_NUMBER not in line and KID_NUMBER not in line


def test_sender_without_number_is_rejected_no_sms_number(world: World, broker: FakeBrokerClient):
    ext = _approved_contact(world)
    users_store.set_sms_number("kid", None)
    result = _routing(broker).send(
        sender_uid="kid",
        recipient_alias=ext.alias,
        kind="text",
        body="hi",
        origin_backend_kind="pager",
    )
    assert [r.reason for r in result.rejected] == ["no_sms_number"]
    assert get_db().collection("messages").get() == []


def test_deliver_fails_at_once_when_sender_loses_number(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms
):
    ext = _approved_contact(world)
    result = _routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="a", origin_backend_kind="pager"
    )
    (msg,) = result.messages
    row = backends_store.list_backends(ext.uid)[0]
    users_store.set_sms_number("kid", None)
    out = SmsTwilioBackend().deliver(messages_store.get_message(msg.id), next(iter(msg.deliveries.values())), row)
    assert out.ok is False and out.state == "failed" and out.error == "no_sms_number"
    assert len(outbound.sent) == 1  # only the first, real send


def test_twilio_4xx_fails_and_5xx_stays_queued_then_tick_retries(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms
):
    ext = _approved_contact(world)
    routing = _routing(broker)

    outbound.result = TwilioSendResult(ok=False, error="twilio returned 400 code 21610", code=21610)
    failed = routing.send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="a", origin_backend_kind="pager"
    ).messages[0]
    assert [d.state for d in messages_store.get_message(failed.id).deliveries.values()] == ["failed"]

    outbound.result = TwilioSendResult(ok=False, error="twilio returned 503", transient=True)
    queued = routing.send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="b", origin_backend_kind="pager"
    ).messages[0]
    (d,) = messages_store.get_message(queued.id).deliveries.values()
    assert d.state == "queued" and d.attempts == 1

    outbound.result = TwilioSendResult(ok=True, sid="SMretry")
    result = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert result.nonPagerRetriesAttempted == 1
    (d,) = messages_store.get_message(queued.id).deliveries.values()
    assert d.state == "sent"
    assert [s["body"] for s in outbound.sent][-1] == "b"


def test_send_sms_client_maps_twilio_responses(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("TWILIO_BASE_URL", "http://twilio.invalid")
    req = httpx.Request("POST", "http://twilio.invalid")

    def respond(status: int, json: dict) -> None:
        monkeypatch.setattr(
            httpx, "post", lambda *a, **k: httpx.Response(status, json=json, request=req)
        )

    respond(201, {"sid": "SM9"})
    ok = sms_client.send_sms("+12065550111", "x", from_number="+12065550100")
    assert ok.ok and ok.sid == "SM9"
    respond(400, {"code": 21211, "message": "bad"})
    bad = sms_client.send_sms("+12065550111", "x", from_number="+12065550100")
    assert not bad.ok and not bad.transient and bad.code == 21211
    respond(503, {})
    down = sms_client.send_sms("+12065550111", "x", from_number="+12065550100")
    assert not down.ok and down.transient

    def boom(*a, **k):
        raise httpx.ConnectError("no route")

    monkeypatch.setattr(httpx, "post", boom)
    assert sms_client.send_sms("+12065550111", "x", from_number="+12065550100").transient
    monkeypatch.delenv("TWILIO_BASE_URL")
    assert sms_client.send_sms("+12065550111", "x", from_number="+12065550100").transient


def test_stale_sms_row_on_a_person_is_ignored(world: World, broker: FakeBrokerClient, outbound: OutboundSms):
    get_db().collection("users").document("kid").collection("backends").document("stale").set(
        {"kind": "sms", "enabled": True, "config": {"phone": "+12065550999"}}
    )
    assert backends_store.get_backend("kid", "stale") is None
    assert [b.kind for b in backends_store.list_backends("kid")] == ["webapp"]
    ext = _approved_contact(world)
    # A message *to* kid fans out to webapp only; the stale row sends nothing.
    _routing(broker).send(
        sender_uid=ext.uid,
        recipient_alias="kid",
        kind="text",
        body="hi",
        origin_backend_kind="sms",
    )
    assert outbound.sent == []


def test_external_gets_an_sms_backend_row_and_ensure_is_idempotent(world: World):
    ext = _approved_contact(world)
    rows = backends_store.list_backends(ext.uid)
    assert [(b.kind, b.config, b.enabled) for b in rows] == [("sms", {"phone": MOM_NUMBER}, True)]
    assert rows[0].verifiedAt is not None
    assert externals_store.ensure_sms_backend(ext) == rows[0].id
    assert len(backends_store.list_backends(ext.uid)) == 1


def test_render_state_sent_by_sms():
    from app.store.messages import Delivery

    backend = SmsTwilioBackend()
    assert backend.render_state(Delivery(kind="sms", state="sent")) == "sent by SMS"
    assert backend.render_state(Delivery(kind="sms", state="queued")) == "queued"
    assert backend.start_link(None, None) is None  # type: ignore[arg-type]
    assert backend.complete_link(None, "123") is False  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# a number belongs to one person
# ---------------------------------------------------------------------------


def test_sms_number_unique_per_person_clear_frees_it(client: TestClient, world: World):
    users_store.create_user(uid="kid2", alias="kid2", display_name="Kid2", family_id=world.family_id)
    h = world.admin_headers
    resp = client.patch("/api/family/members/kid2", json={"smsNumber": "206-555-0100"}, headers=h)
    assert resp.status_code == 409, resp.text
    assert resp.json()["detail"] == "number already assigned to @kid"
    assert users_store.get_user("kid2").smsNumber is None

    resp = client.patch("/api/family/members/kid", json={"smsNumber": None}, headers=h)
    assert resp.status_code == 200 and resp.json()["smsNumber"] is None
    assert users_store.get_uid_for_sms_number(KID_NUMBER) is None
    resp = client.patch("/api/family/members/kid2", json={"smsNumber": "(206) 555-0100"}, headers=h)
    assert resp.status_code == 200 and resp.json()["smsNumber"] == KID_NUMBER
    assert users_store.get_uid_for_sms_number(KID_NUMBER) == "kid2"

    # Absent is not null: an unrelated patch leaves the number alone.
    resp = client.patch("/api/family/members/kid2", json={"displayName": "K2"}, headers=h)
    assert resp.json()["smsNumber"] == KID_NUMBER
    resp = client.patch("/api/family/members/kid2", json={"smsNumber": "garbage"}, headers=h)
    assert resp.status_code == 400


def test_super_can_set_sms_number_and_reuse_is_409(client: TestClient, world: World):
    fb_auth.create_user(uid="root", email="root@example.com")
    users_store.create_user(uid="root", alias="root", display_name="root", role="super", family_id=world.family_id)
    fb_auth.set_custom_user_claims("root", {"role": "super", "fam": ""})
    h = auth_header("root")
    users_store.create_user(uid="kid3", alias="kid3", display_name="K3", family_id=world.family_id)
    resp = client.patch("/api/admin/users/kid3", json={"smsNumber": KID_NUMBER}, headers=h)
    assert resp.status_code == 409 and resp.json()["detail"] == "number already assigned to @kid"
    resp = client.patch("/api/admin/users/kid3", json={"smsNumber": "+12065550444"}, headers=h)
    assert resp.status_code == 200 and resp.json()["smsNumber"] == "+12065550444"
    # Moving the number frees the old index entry.
    resp = client.patch("/api/admin/users/kid3", json={"smsNumber": "+12065550445"}, headers=h)
    assert users_store.get_uid_for_sms_number("+12065550444") is None
    assert users_store.get_uid_for_sms_number("+12065550445") == "kid3"


def test_sms_number_refused_on_an_external(client: TestClient, world: World):
    fb_auth.create_user(uid="root", email="root@example.com")
    users_store.create_user(uid="root", alias="root", display_name="root", role="super", family_id=world.family_id)
    fb_auth.set_custom_user_claims("root", {"role": "super", "fam": ""})
    ext = _approved_contact(world)
    resp = client.patch(
        f"/api/admin/users/{ext.uid}", json={"smsNumber": "+12065550444"}, headers=auth_header("root")
    )
    assert resp.status_code == 400


def test_set_sms_number_pushes_empty_cfg_sms_and_bumps_book(client: TestClient, world: World, broker: FakeBrokerClient):
    from app.store import devices as devices_store

    users_store.set_sms_number("kid", None)
    devices_store.create_device(
        device_id="pgr-n",
        owner_uid="kid",
        label="d",
        mqtt_username="pgr-n",
        mqtt_password_hash="x",
        auth_mode="password",
        family_id=world.family_id,
    )
    _set_policy("kid", "open", "people")
    externals_store.get_or_create(world.family_id, MOM_NUMBER, "Mom")
    resp = client.patch(
        "/api/family/members/kid", json={"smsNumber": KID_NUMBER}, headers=world.admin_headers
    )
    assert resp.status_code == 200, resp.text
    import json

    pushes = [json.loads(m.payload) for m in broker.published if m.topic == "pager/pgr-n/down"]
    sms = [p for p in pushes if p["kind"] == "cfg" and "sms" in p["cfg"]]
    assert sms and sms[-1]["cfg"]["sms"] == []
    assert any(p["kind"] == "book" for p in pushes)
    assert devices_store.get_device("pgr-n").smsContacts == []


# ---------------------------------------------------------------------------
# sweep
# ---------------------------------------------------------------------------


def test_sweep_deletes_old_held_sms_only(world: World):
    from datetime import UTC, datetime, timedelta

    held_sms_store.create("SMold", family_id=world.family_id, to_uid="kid", from_phone=STRANGER, body="o")
    held_sms_store.create("SMnew", family_id=world.family_id, to_uid="kid", from_phone=STRANGER, body="n")
    get_db().collection("heldSms").document("SMold").update(
        {"receivedAt": datetime.now(UTC) - timedelta(days=4000)}
    )
    result = jobs.sweep()
    assert result.heldSmsDeleted == 1
    assert held_sms_store.get("SMold") is None and held_sms_store.get("SMnew") is not None


# ---------------------------------------------------------------------------
# review fixes
# ---------------------------------------------------------------------------


def test_inbound_control_chars_become_spaces(client: TestClient, world: World):
    ext = _approved_contact(world)
    assert _inbound(client, "a\nb\x00c\x7f").status_code == 200
    (msg,) = messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))
    assert msg.body == "a b c"


def test_inbound_over_320_bytes_is_too_long_even_under_160_codepoints(
    client: TestClient, world: World, outbound: OutboundSms, caplog
):
    caplog.set_level(logging.INFO, logger="relay.webhooks")
    ext = _approved_contact(world)
    assert _inbound(client, "\U0001F600" * 100).status_code == 200
    assert _outcome(caplog) == "too_long"
    assert len(outbound.sent) == 1 and "max 160" in outbound.sent[0]["body"]
    assert messages_store.list_thread(messages_store.conv_key(ext.uid, "kid")) == []
    assert get_db().collection("heldSms").get() == []


def test_approve_uses_pager_body_and_keeps_raw_in_held_row(client: TestClient, world: World):
    assert _inbound(client, "x\ny", from_=STRANGER, sid="SMc1").status_code == 200
    assert _inbound(client, "\U0001F600" * 100, from_=STRANGER, sid="SMc2").status_code == 200
    assert held_sms_store.get("SMc1").body == "x\ny"
    (alert,) = _open_alerts(world)
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"name": "N"}, headers=world.admin_headers
    )
    assert resp.json()["delivered"] == 1
    ext = externals_store.get_family_contact(world.family_id, STRANGER)
    assert [m.body for m in messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))] == ["x y"]
    assert held_sms_store.get("SMc2").status == "too_long"


def test_contact_create_refreshes_book_of_a_numbered_owner(
    client: TestClient, world: World, broker: FakeBrokerClient
):
    import json

    from app.store import devices as devices_store

    users_store.set_sms_number("adm", "+12065550888")
    devices_store.create_device(
        device_id="pgr-adm",
        owner_uid="adm",
        label="d",
        mqtt_username="pgr-adm",
        mqtt_password_hash="x",
        auth_mode="password",
        family_id=world.family_id,
    )
    _set_policy("adm", "open", "any")

    def bvs() -> list[int]:
        msgs = [json.loads(m.payload) for m in broker.published if m.topic == "pager/pgr-adm/down"]
        return [m["bv"] for m in msgs if m["kind"] == "book"]

    before = bvs()
    resp = client.post(
        "/api/family/contacts",
        json={"phone": MOM_NUMBER, "name": "Mom"},
        headers=world.admin_headers,
    )
    assert resp.status_code == 201, resp.text
    after = bvs()
    assert len(after) > len(before) and max(after) > max(before or [0])


def test_tick_finds_a_queued_sms_delivery_beyond_the_first_50_messages(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms
):
    routing = _routing(broker)
    ext = _approved_contact(world)
    for i in range(54):
        routing.send(
            sender_uid="kid", recipient_alias="adm", kind="text", body=f"f{i}",
            origin_backend_kind="pager", wire_id=f"w_pre{i}",
        )
    outbound.result = TwilioSendResult(ok=False, error="twilio returned 503", transient=True)
    queued = routing.send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="late", origin_backend_kind="pager"
    ).messages[0]
    for i in range(5):
        routing.send(
            sender_uid="kid", recipient_alias="adm", kind="text", body=f"g{i}",
            origin_backend_kind="pager", wire_id=f"w_post{i}",
        )
    assert len(list(get_db().collection("messages").stream())) == 60
    outbound.result = TwilioSendResult(ok=True, sid="SMlate")
    result = jobs.tick(routing, task_queue=InlineTaskQueue())
    assert result.nonPagerRetriesAttempted == 1
    (d,) = messages_store.get_message(queued.id).deliveries.values()
    assert d.state == "sent"


def test_person_moved_to_another_family_cannot_reach_old_contact(world: World, broker: FakeBrokerClient):
    ext = _approved_contact(world)
    other = families_store.create_family(name="G", created_by="root")
    get_db().collection("users").document("kid").update({"familyId": other.id})
    result = _routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="hi", origin_backend_kind="pager"
    )
    assert [r.reason for r in result.rejected] == ["sms_contact"]


def test_disabled_contact_is_sms_contact_reject(world: World, broker: FakeBrokerClient):
    ext = _approved_contact(world)
    users_store.update_user(ext.uid, disabled=True)
    result = _routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="hi", origin_backend_kind="pager"
    )
    assert [r.reason for r in result.rejected] == ["sms_contact"]


def test_external_default_recipient_is_ignored_for_broadcast(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms
):
    from app.store import devices as devices_store

    ext = _approved_contact(world)
    allow_store.set_edge("kid", "adm", message=True, locate=False)
    devices_store.create_device(
        device_id="pgr-d", owner_uid="kid", label="d", mqtt_username="pgr-d",
        mqtt_password_hash="x", auth_mode="password", family_id=world.family_id,
        default_to_uid=ext.uid,
    )
    result = _routing(broker).send(
        sender_uid="kid", recipient_alias=None, kind="text", body="all", origin_backend_kind="pager",
        device_id="pgr-d",
    )
    assert {m.recipientUid for m in result.messages} == {"adm"}
    assert outbound.sent == []


def test_redeliver_skips_a_delivery_that_is_no_longer_queued(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms
):
    routing = _routing(broker)
    ext = _approved_contact(world)
    msg = routing.send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="once", origin_backend_kind="pager"
    ).messages[0]
    assert len(outbound.sent) == 1
    (bid,) = msg.deliveries.keys()  # the stale in-memory copy still says queued
    assert routing.redeliver(msg, bid) is False
    assert len(outbound.sent) == 1


def test_block_decides_other_open_alerts_for_the_same_number(client: TestClient, world: World):
    users_store.create_user(uid="kid2", alias="kid2", display_name="K2", family_id=world.family_id)
    _set_policy("kid2", "people_sms", "people_sms")
    users_store.set_sms_number("kid2", "+12065550999")
    assert _inbound(client, "to kid", from_=STRANGER).status_code == 200
    assert _inbound(client, "to kid2", from_=STRANGER, to="+12065550999").status_code == 200
    first, second = _open_alerts(world)
    resp = client.post(f"/api/family/alerts/{first.id}/block", json={}, headers=world.admin_headers)
    assert resp.status_code == 200, resp.text
    assert _open_alerts(world) == []
    assert alerts_store.get(world.family_id, second.id).status == "handled"
    assert {r.status for r in held_sms_store.list_held(world.family_id, STRANGER, None, status=None)} == {"blocked"}
