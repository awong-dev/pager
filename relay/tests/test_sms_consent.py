# ruff: noqa: F811
"""SMS consent, keywords, disclosure and defang -- docs/RELAY_SMS_DESIGN.md
decision 11. The compliance strings below are written out literally (not
imported) so a drift in `app/sms_compliance.py` fails here."""

from __future__ import annotations

import logging

import pytest
from fastapi.testclient import TestClient

from app import jobs
from app.backends import sms_twilio
from app.db.firestore import get_db
from app.notify.sms import TwilioSendResult
from app.sms_compliance import defang, keyword, relay_body
from app.store import alerts as alerts_store
from app.store import held_sms as held_sms_store
from app.store import messages as messages_store
from app.store import sms_consent as sms_consent_store
from app.store import users as users_store
from app.tasks import InlineTaskQueue
from tests.fake_transport import FakeBrokerClient
from tests.test_sms_twilio import (  # noqa: F401  (fixtures and helpers)
    KID_NUMBER,
    MOM_NUMBER,
    STRANGER,
    OutboundSms,
    World,
    _approved_contact,
    _inbound,
    _outcome,
    _routing,
    _twilio_env,
    broker,
    client,
    fcm,
    outbound,
    world,
)

WELCOME = (
    "Welcome to Pager, run by Albert Wong. You'll receive two-way coordination messages "
    "relayed from a Pager device. Message frequency varies. Msg & data rates may apply. "
    "Reply HELP for help, STOP to opt out."
)
OPT_OUT = (
    "You have been unsubscribed from Pager (Albert Wong). You will receive no further "
    "messages. Email awong.dev@gmail.com with any questions."
)
HELP = (
    "Pager (Albert Wong): two-way coordination messages relayed from a Pager device. "
    "For help, email awong.dev@gmail.com. Msg & data rates may apply. Reply STOP to opt out."
)
DISCLOSURE = ". Reply STOP to opt out, HELP for help."


@pytest.fixture(autouse=True)
def _default_operator(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SMS_OPERATOR_NAME", raising=False)
    monkeypatch.delenv("SMS_SUPPORT_EMAIL", raising=False)


# ---------------------------------------------------------------------------
# pure: keyword(), relay_body(), defang()
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("body", "expected"),
    [
        (" stop ", "stop"),
        ("Quit", "stop"),
        ("END", "stop"),
        ("unsubscribe", "stop"),
        ("OPTIN", "start"),
        ("in", "start"),
        ("Start", "start"),
        ("Info", "help"),
        ("support", "help"),
        ("HELP", "help"),
        ("please stop", None),
        ("stop it", None),
        ("", None),
    ],
)
def test_keyword_table(body: str, expected: str | None):
    assert keyword(body) == expected


def test_compliance_constants_match_the_spec_byte_for_byte():
    from app import sms_compliance

    assert sms_compliance.WELCOME == WELCOME
    assert sms_compliance.OPT_OUT_REPLY == OPT_OUT
    assert sms_compliance.HELP_REPLY == HELP
    assert sms_compliance.DISCLOSURE == DISCLOSURE


def test_operator_and_email_come_from_env(monkeypatch: pytest.MonkeyPatch):
    from app import sms_compliance

    monkeypatch.setenv("SMS_OPERATOR_NAME", "Op Name")
    monkeypatch.setenv("SMS_SUPPORT_EMAIL", "help@example.org")
    assert "run by Op Name." in sms_compliance.WELCOME
    assert "(Op Name)" in relay_body("Kid", "hi")
    assert "help@example.org" in sms_compliance.HELP_REPLY


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        ("https://example.com/path", "https:// example. com/path"),
        ("www.foo.co.uk/x", "www. foo. co. uk/x"),
        ("example.com", "example. com"),
        ("http://a.b.org:8080/p.q/r?x=1.2", "http:// a. b. org:8080/p.q/r?x=1.2"),
        ("2065551234", "206 555 123 4"),
        ("206-555-1234", "206 -55 5-1 234"),
        ("(206) 555-1234", "(20 6) 555 -12 34"),
        ("123456", "123456"),
        ("e.g. Mr. Smith said so", "e.g. Mr. Smith said so"),
        ("hello there", "hello there"),
        (
            "see www.foo.com or call 2065551234 now",
            "see www. foo. com or call 206 555 123 4 now",
        ),
    ],
)
def test_defang_examples(raw: str, expected: str):
    assert defang(raw) == expected
    assert defang(expected) == expected
    assert defang(defang(raw)) == defang(raw)


def test_defang_leaves_already_defanged_text_alone():
    for text in ("https:// example. com/path", "206 555 123 4", "206 -55 5-1 234"):
        assert defang(text) == text


def test_relay_body_wraps_without_brackets():
    assert relay_body("Kid", "hi") == 'Kid says: "hi" - Pager (Albert Wong)'


# ---------------------------------------------------------------------------
# store
# ---------------------------------------------------------------------------


def test_consent_store_transitions():
    assert sms_consent_store.get(MOM_NUMBER) is None
    assert not sms_consent_store.is_opted_in(MOM_NUMBER)
    assert sms_consent_store.mark_opted_in(MOM_NUMBER, source="keyword") is True
    assert sms_consent_store.mark_opted_in(MOM_NUMBER, source="admin") is False
    assert sms_consent_store.is_opted_in(MOM_NUMBER)
    sms_consent_store.mark_opted_out(MOM_NUMBER)
    row = sms_consent_store.get(MOM_NUMBER)
    assert row is not None and row.status == "opted_out" and row.optedOutAt is not None
    assert row.optedInAt is not None
    assert sms_consent_store.mark_opted_in(MOM_NUMBER, source="keyword") is True


def test_claim_disclosure_once_per_day():
    assert sms_consent_store.claim_disclosure(MOM_NUMBER, "2026-10-08") is True
    assert sms_consent_store.claim_disclosure(MOM_NUMBER, "2026-10-08") is False
    assert sms_consent_store.claim_disclosure(MOM_NUMBER, "2026-10-09") is True


# ---------------------------------------------------------------------------
# inbound keywords
# ---------------------------------------------------------------------------


def _nothing_stored(world: World) -> None:
    assert held_sms_store.list_held(world.family_id, MOM_NUMBER, None, status=None) == []
    assert get_db().collection("messages").get() == []
    assert alerts_store.list_alerts(world.family_id, "open") == []


@pytest.mark.parametrize(
    ("body", "kw", "reply"),
    [
        ("START", "start", WELCOME),
        ("optin", "start", WELCOME),
        ("in", "start", WELCOME),
        (" stop ", "stop", OPT_OUT),
        ("Quit", "stop", OPT_OUT),
        ("UNSUBSCRIBE", "stop", OPT_OUT),
        ("end", "stop", OPT_OUT),
        ("HELP", "help", HELP),
        ("Info", "help", HELP),
        ("support", "help", HELP),
    ],
)
def test_keyword_replies_from_the_members_number_and_stores_nothing(
    client: TestClient,
    world: World,
    outbound: OutboundSms,
    caplog: pytest.LogCaptureFixture,
    body: str,
    kw: str,
    reply: str,
):
    caplog.set_level(logging.INFO, logger="relay.routers.webhooks")
    assert _inbound(client, body).status_code == 200
    assert outbound.sent == [{"to": MOM_NUMBER, "body": reply, "from": KID_NUMBER}]
    assert _outcome(caplog) == f"keyword_{kw}"
    _nothing_stored(world)
    status = sms_consent_store.get(MOM_NUMBER)
    expected = {"start": "opted_in", "stop": "opted_out", "help": None}[kw]
    assert (status.status if status else None) == expected


def test_a_text_containing_a_keyword_is_not_a_keyword(
    client: TestClient, world: World, outbound: OutboundSms, caplog: pytest.LogCaptureFixture
):
    caplog.set_level(logging.INFO, logger="relay.routers.webhooks")
    assert _inbound(client, "please stop").status_code == 200
    assert outbound.sent == []
    assert _outcome(caplog) == "held"


def test_start_twice_welcomes_both_times(client: TestClient, world: World, outbound: OutboundSms):
    assert _inbound(client, "START").status_code == 200
    assert _inbound(client, "start").status_code == 200
    assert [s["body"] for s in outbound.sent] == [WELCOME, WELCOME]
    assert sms_consent_store.is_opted_in(MOM_NUMBER)


def test_stop_works_for_a_blocked_number(client: TestClient, world: World, outbound: OutboundSms):
    from app.store import families as families_store

    families_store.add_blocked_number(world.family_id, MOM_NUMBER)
    assert _inbound(client, "STOP").status_code == 200
    assert [s["body"] for s in outbound.sent] == [OPT_OUT]


# ---------------------------------------------------------------------------
# outbound gate and format
# ---------------------------------------------------------------------------


def _kid_says(broker: FakeBrokerClient, ext_alias: str, body: str):
    return _routing(broker).send(
        sender_uid="kid",
        recipient_alias=ext_alias,
        kind="text",
        body=body,
        origin_backend_kind="pager",
    )


def _delivery_states(result) -> list[tuple[str, str | None]]:
    (msg,) = result.messages
    return [
        (d.state, d.error) for d in messages_store.get_message(msg.id).deliveries.values()
    ]


def test_never_opted_in_number_fails_not_opted_in(
    world: World,
    broker: FakeBrokerClient,
    outbound: OutboundSms,
    caplog: pytest.LogCaptureFixture,
):
    caplog.set_level(logging.INFO, logger="relay.backends.sms_twilio")
    ext = _approved_contact(world)
    get_db().collection("smsConsent").document(MOM_NUMBER).delete()
    result = _kid_says(broker, ext.alias, "hi")
    assert _delivery_states(result) == [("failed", "not_opted_in")]
    assert outbound.sent == []
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("sms out"))
    assert line == "sms out to=...0111 from=...0100 sid=- status=failed code=not_opted_in"
    # Never retried by the tick.
    jobs.tick(_routing(broker), task_queue=InlineTaskQueue())
    assert outbound.sent == []


def test_opted_out_number_fails_until_start(
    client: TestClient,
    world: World,
    broker: FakeBrokerClient,
    outbound: OutboundSms,
    caplog: pytest.LogCaptureFixture,
):
    caplog.set_level(logging.INFO, logger="relay.backends.sms_twilio")
    ext = _approved_contact(world)
    assert _inbound(client, "STOP").status_code == 200
    assert [s["body"] for s in outbound.sent] == [OPT_OUT]  # reaches an opted-out number

    result = _kid_says(broker, ext.alias, "hi")
    assert _delivery_states(result) == [("failed", "opted_out")]
    assert len(outbound.sent) == 1  # no Twilio call for the page
    assert any("code=opted_out" in r.getMessage() for r in caplog.records)

    assert _inbound(client, "START").status_code == 200
    _kid_says(broker, ext.alias, "again")
    assert outbound.sent[-1]["body"] == 'Kid says: "again" - Pager (Albert Wong)' + DISCLOSURE


def test_outbound_format_and_daily_disclosure(
    world: World,
    broker: FakeBrokerClient,
    outbound: OutboundSms,
    monkeypatch: pytest.MonkeyPatch,
):
    ext = _approved_contact(world)
    monkeypatch.setattr(sms_twilio, "_today_utc", lambda: "2026-10-08")
    _kid_says(broker, ext.alias, "hi")
    _kid_says(broker, ext.alias, "hi again")
    monkeypatch.setattr(sms_twilio, "_today_utc", lambda: "2026-10-09")
    _kid_says(broker, ext.alias, "next day")
    assert [s["body"] for s in outbound.sent] == [
        'Kid says: "hi" - Pager (Albert Wong). Reply STOP to opt out, HELP for help.',
        'Kid says: "hi again" - Pager (Albert Wong)',
        'Kid says: "next day" - Pager (Albert Wong). Reply STOP to opt out, HELP for help.',
    ]


def test_outbound_defangs_text_but_not_prefix_or_suffix(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms, monkeypatch: pytest.MonkeyPatch
):
    ext = _approved_contact(world)
    monkeypatch.setattr(sms_twilio, "_today_utc", lambda: "2026-10-08")
    _kid_says(broker, ext.alias, "see www.foo.com or call 2065551234")
    assert outbound.sent[0]["body"] == (
        'Kid says: "see www. foo. com or call 206 555 123 4" - Pager (Albert Wong)' + DISCLOSURE
    )


def test_failed_send_still_consumes_the_days_disclosure(
    world: World, broker: FakeBrokerClient, outbound: OutboundSms, monkeypatch: pytest.MonkeyPatch
):
    ext = _approved_contact(world)
    monkeypatch.setattr(sms_twilio, "_today_utc", lambda: "2026-10-08")
    outbound.result = TwilioSendResult(ok=False, error="twilio returned 503", transient=True)
    _kid_says(broker, ext.alias, "a")
    outbound.result = TwilioSendResult(ok=True, sid="SMx")
    jobs.tick(_routing(broker), task_queue=InlineTaskQueue())
    assert outbound.sent[0]["body"].endswith(DISCLOSURE)
    assert outbound.sent[1]["body"] == 'Kid says: "a" - Pager (Albert Wong)'


# ---------------------------------------------------------------------------
# admin consent
# ---------------------------------------------------------------------------


def test_create_contact_welcomes_once(client: TestClient, world: World, outbound: OutboundSms):
    payload = {"phone": MOM_NUMBER, "name": "Mom"}
    resp = client.post("/api/family/contacts", json=payload, headers=world.admin_headers)
    assert resp.status_code == 201, resp.text
    assert outbound.sent == [{"to": MOM_NUMBER, "body": WELCOME, "from": KID_NUMBER}]
    row = sms_consent_store.get(MOM_NUMBER)
    assert row is not None and row.status == "opted_in" and row.source == "admin"

    resp = client.post("/api/family/contacts", json=payload, headers=world.admin_headers)
    assert resp.status_code == 201, resp.text
    assert len(outbound.sent) == 1


def test_create_contact_without_any_member_number_warns_and_still_opts_in(
    client: TestClient,
    world: World,
    outbound: OutboundSms,
    caplog: pytest.LogCaptureFixture,
):
    users_store.set_sms_number("kid", None)
    caplog.set_level(logging.WARNING, logger="relay.family")
    resp = client.post(
        "/api/family/contacts", json={"phone": MOM_NUMBER, "name": "Mom"}, headers=world.admin_headers
    )
    assert resp.status_code == 201, resp.text
    assert outbound.sent == []
    assert sms_consent_store.is_opted_in(MOM_NUMBER)
    assert any("sms welcome skipped: no member number" in r.getMessage() for r in caplog.records)


def test_approving_a_held_alert_welcomes_before_the_backlog_and_replies_go_out(
    client: TestClient, world: World, broker: FakeBrokerClient, outbound: OutboundSms
):
    assert _inbound(client, "first", from_=STRANGER, sid="SMc1").status_code == 200
    assert _inbound(client, "second", from_=STRANGER, sid="SMc2").status_code == 200
    assert outbound.sent == []
    (alert,) = alerts_store.list_alerts(world.family_id, "open")
    resp = client.post(
        f"/api/family/alerts/{alert.id}/approve", json={"name": "Neighbor"}, headers=world.admin_headers
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["delivered"] == 2
    assert outbound.sent == [{"to": STRANGER, "body": WELCOME, "from": KID_NUMBER}]

    from app.store import externals as externals_store

    ext = externals_store.get_family_contact(world.family_id, STRANGER)
    assert ext is not None
    result = _kid_says(broker, ext.alias, "got it")
    assert _delivery_states(result) == [("sent", None)]
    assert outbound.sent[-1]["body"].startswith('Kid says: "got it"')
