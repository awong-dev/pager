"""Held texts, their approval, and bridge-only SMS -- docs/RELAY_SMS_DESIGN.md
(held/approve model) on top of docs/BRIDGE_PHONE_DESIGN.md (the only transport).

Inbound goes through `app/inbound_text.handle_text`, the shared core that
`POST /bridge/events` calls (the event plumbing itself is covered by
tests/test_bridge_events.py); outbound is the `sms` backend's bridge branch.
Only FCM is stubbed.
"""

from __future__ import annotations

import logging
from collections.abc import Iterator
from dataclasses import dataclass, field

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import alerts as alerts_module
from app import jobs
from app.backends.sms import SmsBackend
from app.config import Settings
from app.db.firestore import get_db
from app.inbound_text import handle_text
from app.main import create_app
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
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header

WEBHOOK_KEY = "test-webhook-key"

KID_NUMBER = "+12065550100"
MOM_NUMBER = "+12065550111"
STRANGER = "+12065550222"


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
    """Texts `handle_text` asked to send back to the sender (the `too_long` hint)."""

    sent: list[dict] = field(default_factory=list)
    last_outcome: str = ""


_OUT: list[OutboundSms] = []


@pytest.fixture
def outbound() -> OutboundSms:
    out = OutboundSms()
    _OUT[:] = [out]
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


_sid_counter = 0


def _inbound(
    client: TestClient,
    body: str,
    *,
    from_: str = MOM_NUMBER,
    to: str = KID_NUMBER,
    sid: str | None = None,
    attachments: tuple[str, ...] = (),
) -> str:
    """One inbound text to the member holding `to`, as the bridge event
    handler would deliver it; returns the outcome label."""
    global _sid_counter
    _sid_counter += 1
    target = users_store.get_user(users_store.get_uid_for_sms_number(to))
    out = _OUT[0]
    from_number = externals_store.normalize_phone(from_)
    outcome = handle_text(
        target,
        from_number,
        body,
        sid or f"SM{_sid_counter:032d}",
        client.app.state.routing,
        reply=lambda text: out.sent.append({"to": from_number, "body": text, "from": to}),
        attachments=attachments,
    )
    out.last_outcome = outcome
    return outcome


def _outcome(_caplog: pytest.LogCaptureFixture | None = None) -> str:
    return _OUT[0].last_outcome


def _kid_inbox() -> list:
    return [m for m in get_db().collection("messages").stream() if m.to_dict()["recipientUid"] == "kid"]


def _open_alerts(world: World):
    return alerts_store.list_alerts(world.family_id, "open")


# ---------------------------------------------------------------------------
# inbound: the step table
# ---------------------------------------------------------------------------


def test_blocked_number_stores_nothing(client: TestClient, world: World, caplog):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    families_store.add_blocked_number(world.family_id, STRANGER)
    _inbound(client, "buy now", from_=STRANGER)
    assert _outcome(caplog) == "blocked"
    assert get_db().collection("heldSms").get() == []
    assert _open_alerts(world) == []


def test_known_approved_contact_is_delivered_with_wire_id(
    client: TestClient, world: World, caplog
):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    ext = _approved_contact(world)
    _inbound(client, "hi from mom", sid="SMdeliver1")
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
    _inbound(client, "hi from mom", sid="SMdeliver1")
    assert _outcome(caplog) == "duplicate"
    assert len(messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))) == 1


def test_inbound_any_delivers_a_known_contact_without_an_edge(client: TestClient, world: World):
    ext = externals_store.get_or_create(world.family_id, MOM_NUMBER, "Mom")
    _set_policy("kid", "people_sms", "any")
    _inbound(client, "no edge needed")
    assert len(messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))) == 1


def test_unknown_number_even_under_any_is_held_not_delivered(
    client: TestClient, world: World, caplog
):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    _set_policy("kid", "people_sms", "any")
    _inbound(client, "who dis", from_=STRANGER)
    assert _outcome(caplog) == "held"
    assert _kid_inbox() == []


@pytest.mark.parametrize("scenario", ["no_contact", "contact_without_edge", "rule_none"])
def test_otherwise_is_held(client: TestClient, world: World, caplog, scenario: str):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    if scenario == "contact_without_edge":
        externals_store.get_or_create(world.family_id, MOM_NUMBER, "Mom")
    elif scenario == "rule_none":
        _approved_contact(world)
        _set_policy("kid", "people_sms", "people")  # numbers: none
    _inbound(client, "hello")
    assert _outcome(caplog) == "held"
    assert _kid_inbox() == []
    (row,) = held_sms_store.list_held(world.family_id, MOM_NUMBER, "kid")
    assert row.body == "hello" and row.status == "held" and row.alertId


def test_photo_without_body_becomes_photo_marker(client: TestClient, world: World):
    ext = _approved_contact(world)
    _inbound(client, "", attachments=("image",))
    (msg,) = messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))
    assert msg.body == "[photo]"


def test_too_long_from_known_contact_replies_with_hint_and_stores_nothing(
    client: TestClient, world: World, outbound: OutboundSms, caplog
):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    ext = _approved_contact(world)
    _inbound(client, "x" * 161)
    assert _outcome(caplog) == "too_long"
    assert messages_store.list_thread(messages_store.conv_key(ext.uid, "kid")) == []
    assert len(outbound.sent) == 1
    assert outbound.sent[0]["to"] == MOM_NUMBER and outbound.sent[0]["from"] == KID_NUMBER
    assert outbound.sent[0]["body"] == "Message too long (max 160 characters) -- not sent."


def test_too_long_from_unknown_number_is_held_without_reply(
    client: TestClient, world: World, outbound: OutboundSms
):
    _inbound(client, "y" * 400, from_=STRANGER)
    assert outbound.sent == []
    (row,) = held_sms_store.list_held(world.family_id, STRANGER, "kid")
    assert len(row.body) == 400


# ---------------------------------------------------------------------------
# held texts: one alert per number, a push per text, cap
# ---------------------------------------------------------------------------


def test_two_held_texts_make_one_alert_and_two_pushes(
    client: TestClient, world: World, fcm: RecordingFCM
):
    _inbound(client, "first one", from_=STRANGER)
    _inbound(client, "second one", from_=STRANGER)
    (alert,) = _open_alerts(world)
    assert alert.kind == "sms_unknown" and alert.subjectUid == "kid"
    assert alert.peerPhone == STRANGER
    assert alert.heldCount == 2 and alert.preview == "second one"
    assert alert.updatedAt is not None
    bodies = [data["body"] for _tokens, data in fcm.calls]
    assert bodies == [f"{STRANGER} → @kid: first one", f"{STRANGER} → @kid: second one"]
    assert all(data["alertKind"] == "sms_unknown" for _t, data in fcm.calls)
    # A different number is a different alert.
    _inbound(client, "other", from_="+12065550333")
    assert len(_open_alerts(world)) == 2


def test_held_cap_drops_the_26th_without_a_push(
    client: TestClient, world: World, fcm: RecordingFCM, caplog
):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    for i in range(held_sms_store.HELD_CAP):
        assert held_sms_store.create(
            f"SMpre{i}", family_id=world.family_id, to_uid="kid", from_phone=STRANGER, body="x"
        )
    _inbound(client, "one too many", from_=STRANGER)
    assert _outcome(caplog) == "held_cap"
    assert held_sms_store.count_held(world.family_id, STRANGER, "kid") == held_sms_store.HELD_CAP
    assert fcm.calls == []


def test_held_duplicate_sid_is_not_stored_or_pushed_twice(
    client: TestClient, world: World, fcm: RecordingFCM, caplog
):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    _inbound(client, "once", from_=STRANGER, sid="SMdup")
    _inbound(client, "once", from_=STRANGER, sid="SMdup")
    assert _outcome(caplog) == "duplicate"
    assert held_sms_store.count_held(world.family_id, STRANGER, "kid") == 1
    assert len(fcm.calls) == 1


# ---------------------------------------------------------------------------
# approve / block / dismiss
# ---------------------------------------------------------------------------


def _hold_two(client: TestClient) -> None:
    _inbound(client, "first", from_=STRANGER, sid="SMa1")
    _inbound(client, "second", from_=STRANGER, sid="SMa2")


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
    # wire_id dedup (the event id) keeps the thread at two messages.
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
    _inbound(client, "z" * 200, from_=STRANGER, sid="SMb1")
    _inbound(client, "short", from_=STRANGER, sid="SMb2")
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
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    _hold_two(client)
    (alert,) = _open_alerts(world)
    resp = client.post(f"/api/family/alerts/{alert.id}/block", json={}, headers=world.admin_headers)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "handled"
    rows = held_sms_store.list_held(world.family_id, STRANGER, None, status=None)
    assert [r.status for r in rows] == ["blocked", "blocked"]
    _inbound(client, "again", from_=STRANGER)
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


def test_sender_without_a_bridge_number_fails_no_bridge(world: World, broker: FakeBrokerClient):
    """SMS is bridge-only: KID_NUMBER is set but belongs to no paired bridge."""
    ext = _approved_contact(world)
    result = _routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="hi", origin_backend_kind="pager"
    )
    assert result.rejected == []
    (msg,) = result.messages
    (d,) = messages_store.get_message(msg.id).deliveries.values()
    assert (d.kind, d.state, d.error) == ("sms", "failed", "no_bridge")
    assert get_db().collection("bridgeOutbox").get() == []


def test_deliver_fails_at_once_when_sender_loses_number(world: World, broker: FakeBrokerClient):
    ext = _approved_contact(world)
    result = _routing(broker).send(
        sender_uid="kid", recipient_alias=ext.alias, kind="text", body="a", origin_backend_kind="pager"
    )
    (msg,) = result.messages
    row = backends_store.list_backends(ext.uid)[0]
    users_store.set_sms_number("kid", None)
    out = SmsBackend().deliver(messages_store.get_message(msg.id), next(iter(msg.deliveries.values())), row)
    assert out.ok is False and out.state == "failed" and out.error == "no_sms_number"


def test_external_gets_an_sms_backend_row_and_ensure_is_idempotent(world: World):
    ext = _approved_contact(world)
    rows = backends_store.list_backends(ext.uid)
    assert [(b.kind, b.config, b.enabled) for b in rows] == [("sms", {"phone": MOM_NUMBER}, True)]
    assert rows[0].verifiedAt is not None
    assert externals_store.ensure_sms_backend(ext) == rows[0].id
    assert len(backends_store.list_backends(ext.uid)) == 1


def test_render_state_sent_by_sms():
    from app.store.messages import Delivery

    backend = SmsBackend()
    assert backend.render_state(Delivery(kind="sms", state="sent")) == "sent by SMS"
    assert backend.render_state(Delivery(kind="sms", state="queued")) == "queued"
    assert backend.start_link(None, None) is None  # type: ignore[arg-type]
    assert backend.complete_link(None, "123") is False  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# a number belongs to one person; it is no longer admin-settable
# ---------------------------------------------------------------------------


def test_sms_number_unique_per_person_clear_frees_it(world: World):
    users_store.create_user(uid="kid2", alias="kid2", display_name="Kid2", family_id=world.family_id)
    with pytest.raises(users_store.SmsNumberTaken) as exc:
        users_store.set_sms_number("kid2", KID_NUMBER)
    assert exc.value.holder_uid == "kid"
    users_store.set_sms_number("kid", None)
    assert users_store.get_uid_for_sms_number(KID_NUMBER) is None
    users_store.set_sms_number("kid2", KID_NUMBER)
    assert users_store.get_uid_for_sms_number(KID_NUMBER) == "kid2"


def test_admin_and_family_patches_ignore_sms_number(client: TestClient, world: World):
    h = world.admin_headers
    resp = client.patch("/api/family/members/kid", json={"smsNumber": "+12065550444"}, headers=h)
    assert resp.status_code == 200 and resp.json()["smsNumber"] == KID_NUMBER
    fb_auth.create_user(uid="root", email="root@example.com")
    users_store.create_user(uid="root", alias="root", display_name="root", role="super", family_id=world.family_id)
    fb_auth.set_custom_user_claims("root", {"role": "super", "fam": ""})
    resp = client.patch(
        "/api/admin/users/kid", json={"smsNumber": None, "displayName": "K"}, headers=auth_header("root")
    )
    assert resp.status_code == 200 and resp.json()["smsNumber"] == KID_NUMBER
    assert users_store.get_uid_for_sms_number("+12065550444") is None


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
    _inbound(client, "a\nb\x00c\x7f")
    (msg,) = messages_store.list_thread(messages_store.conv_key(ext.uid, "kid"))
    assert msg.body == "a b c"


def test_inbound_over_320_bytes_is_too_long_even_under_160_codepoints(
    client: TestClient, world: World, outbound: OutboundSms, caplog
):
    caplog.set_level(logging.INFO, logger="relay.inbound_text")
    ext = _approved_contact(world)
    _inbound(client, "\U0001F600" * 100)
    assert _outcome(caplog) == "too_long"
    assert len(outbound.sent) == 1 and "max 160" in outbound.sent[0]["body"]
    assert messages_store.list_thread(messages_store.conv_key(ext.uid, "kid")) == []
    assert get_db().collection("heldSms").get() == []


def test_approve_uses_pager_body_and_keeps_raw_in_held_row(client: TestClient, world: World):
    _inbound(client, "x\ny", from_=STRANGER, sid="SMc1")
    _inbound(client, "\U0001F600" * 100, from_=STRANGER, sid="SMc2")
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
    world: World, broker: FakeBrokerClient
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


def test_block_decides_other_open_alerts_for_the_same_number(client: TestClient, world: World):
    users_store.create_user(uid="kid2", alias="kid2", display_name="K2", family_id=world.family_id)
    _set_policy("kid2", "people_sms", "people_sms")
    users_store.set_sms_number("kid2", "+12065550999")
    _inbound(client, "to kid", from_=STRANGER)
    _inbound(client, "to kid2", from_=STRANGER, to="+12065550999")
    first, second = _open_alerts(world)
    resp = client.post(f"/api/family/alerts/{first.id}/block", json={}, headers=world.admin_headers)
    assert resp.status_code == 200, resp.text
    assert _open_alerts(world) == []
    assert alerts_store.get(world.family_id, second.id).status == "handled"
    assert {r.status for r in held_sms_store.list_held(world.family_id, STRANGER, None, status=None)} == {"blocked"}
