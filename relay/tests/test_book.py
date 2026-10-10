"""docs/ADDRESS_BOOK_DESIGN.md: the derived address book (`app/book.py`),
`/api/book`, per-owner nicknames, in-family approval and the `bv` triggers."""

from __future__ import annotations

import json
from collections.abc import Iterator

import pytest
from fastapi.testclient import TestClient
from firebase_admin import auth as fb_auth

from app import book as book_module
from app import devcfg
from app.config import Settings
from app.db.firestore import get_db
from app.main import create_app
from app.routing import Routing
from app.store import allow as allow_store
from app.store import devices as devices_store
from app.store import externals as externals_store
from app.store import families as families_store
from app.store import users as users_store
from tests.fake_transport import FakeBrokerClient
from tests.firebase_test_utils import auth_header


def make_settings() -> Settings:
    return Settings(
        broker_api_url="http://unused.invalid/api/v5",
        broker_api_key=None,
        broker_api_secret=None,
        webhook_key="test-webhook-key",
        dev_mode=True,
        google_cloud_project=None,
        firestore_emulator_host=None,
        firebase_auth_emulator_host=None,
    )


@pytest.fixture
def broker() -> FakeBrokerClient:
    return FakeBrokerClient()


@pytest.fixture
def client(broker: FakeBrokerClient) -> Iterator[TestClient]:
    with TestClient(create_app(settings=make_settings(), broker_client=broker)) as c:
        yield c


def _family(name: str = "F") -> str:
    return families_store.create_family(name=name, created_by="root").id


def _user(
    uid: str, family_id: str | None, role: str = "member", *, name: str | None = None, auth: bool = True
) -> dict[str, str]:
    """A registered person; with `auth` also a real Auth account + claims and
    the returned bearer header."""
    users_store.create_user(
        uid=uid, alias=uid, display_name=name or uid, role=role, family_id=family_id
    )
    if not auth:
        return {}
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    fb_auth.set_custom_user_claims(uid, {"role": role, "fam": family_id or ""})
    return auth_header(uid)


def _device(device_id: str, owner_uid: str, family_id: str | None) -> None:
    # password mode: published envelopes stay plain JSON.
    devices_store.create_device(
        device_id=device_id,
        owner_uid=owner_uid,
        label="d",
        mqtt_username=device_id,
        mqtt_password_hash="x",
        auth_mode="password",
        family_id=family_id,
    )


def _set_policy(uid: str, out: str, in_: str) -> None:
    get_db().collection("users").document(uid).update({"policy": {"out": out, "in": in_}})


def _bv(device_id: str) -> int:
    return devcfg.get_book_version(device_id)


def _aliases(body: dict) -> dict[str, dict]:
    return {e["alias"]: e for e in body["entries"]}


def _book_pushes(broker: FakeBrokerClient) -> list[dict]:
    return [d for d in (json.loads(m.payload) for m in broker.published) if d.get("kind") == "book"]


# a ---------------------------------------------------------------------------


def test_same_family_members_listed_sendable_and_dm_delivered(
    client: TestClient, broker: FakeBrokerClient
):
    fam = _family()
    ha = _user("ana", fam)
    hb = _user("ben", fam)

    for headers, other in ((ha, "ben"), (hb, "ana")):
        resp = client.get("/api/book", headers=headers)
        assert resp.status_code == 200, resp.text
        entry = _aliases(resp.json())[other]
        assert entry["sendable"] is True
        assert entry["inFamily"] is True
        assert entry["kind"] == "person"
        assert entry["label"] == other

    assert allow_store.get_edge("ana", "ben") is None
    result = Routing(broker).send(
        sender_uid="ana",
        recipient_alias="ben",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert result.rejected == []
    assert len(result.messages) == 1


def test_explicit_deny_edge_beats_family_implied_approval(
    client: TestClient, broker: FakeBrokerClient
):
    fam = _family()
    ha = _user("ana", fam)
    hb = _user("ben", fam)
    _set_policy("ana", "people", "people")
    _set_policy("ben", "people", "people")
    allow_store.set_edge("ana", "ben", message=False, locate=False)

    # ana -> ben: her own deny.
    ben = _aliases(client.get("/api/book", headers=ha).json())["ben"]
    assert ben["sendable"] is False
    assert ben["reason"] == "not_allowed"
    result = Routing(broker).send(
        sender_uid="ana",
        recipient_alias="ben",
        kind="text",
        body="hi",
        origin_backend_kind="webapp",
    )
    assert [r.reason for r in result.rejected] == ["not_allowed"]
    assert result.messages == []

    # ben -> ana: ana's `people` inbound rule needs ben on her approved list,
    # and her deny removes him -- the book and the send check agree.
    ana = _aliases(client.get("/api/book", headers=hb).json())["ana"]
    assert (ana["sendable"], ana["reason"]) == (False, "not_allowed")

    # Lifting the deny (edge removed) restores the family default.
    allow_store.delete_edge("ana", "ben")
    assert _aliases(client.get("/api/book", headers=ha).json())["ben"]["sendable"] is True


def test_edge_or_family_deny_wins_but_other_policy_refusals_stay(client: TestClient):
    from app import book
    from app.store import users as users_store

    fam = _family()
    _user("ana", fam)
    _user("ben", fam)
    ana, ben = users_store.get_user("ana"), users_store.get_user("ben")
    assert ana is not None and ben is not None
    assert book.edge_or_family(ana, ben) is True  # no edge: family default
    allow_store.set_edge("ana", "ben", message=False, locate=False)
    assert book.edge_or_family(ana, ben) is False  # explicit deny
    assert book.edge_or_family(ben, ana) is True  # one-way
    allow_store.set_edge("ana", "ben", message=True, locate=False)
    assert book.edge_or_family(ana, ben) is True


# b ---------------------------------------------------------------------------


def test_policy_sms_family_entries_not_sendable_and_off_pager(client: TestClient):
    fam = _family()
    h = _user("ana", fam)
    _user("ben", fam)
    _device("pgr-b", "ana", fam)
    _set_policy("ana", "sms", "people")

    body = client.get("/api/book", headers=h).json()
    ben = _aliases(body)["ben"]
    assert ben["sendable"] is False
    assert ben["reason"] == "policy_out"
    assert ben["onPager"] is False
    assert [c["a"] for c in devcfg.build_book_body("pgr-b")["c"]] == []


# c, f ------------------------------------------------------------------------


def test_put_nick_by_self_bumps_and_pushes_then_delete_clears(
    client: TestClient, broker: FakeBrokerClient
):
    fam = _family()
    h = _user("ana", fam)
    _user("ben", fam, name="Benjamin")
    _device("pgr-c", "ana", fam)
    before = _bv("pgr-c")
    broker.clear()

    resp = client.put("/api/book/ana/entries/ben", json={"nick": "Benny"}, headers=h)
    assert resp.status_code == 200, resp.text
    assert resp.json()["nick"] == "Benny"
    assert resp.json()["label"] == "Benny"
    assert _bv("pgr-c") == before + 1
    assert _book_pushes(broker), "no book push published"
    assert devcfg.build_book_body("pgr-c")["c"][0]["n"] == "Benny"
    assert client.get("/api/book", headers=h).json()["bv"] == before + 1

    resp = client.delete("/api/book/ana/entries/ben", headers=h)
    assert resp.status_code == 200, resp.text
    assert resp.json()["nick"] is None
    assert resp.json()["label"] == "Benjamin"
    assert _bv("pgr-c") == before + 2
    assert devcfg.build_book_body("pgr-c")["c"][0]["n"] == "Benjamin"
    assert get_db().collection("users").document("ana").collection("book").document("ben").get().exists is False


def test_set_nick_transaction_writes_doc_and_bumps_every_device(client: TestClient):
    fam = _family()
    _user("ana", fam, auth=False)
    _user("ben", fam, auth=False)
    _device("pgr-t1", "ana", fam)
    _device("pgr-t2", "ana", fam)

    book_module.set_nick("ana", "ben", "B", "ana")

    assert (_bv("pgr-t1"), _bv("pgr-t2")) == (1, 1)
    doc = get_db().collection("users").document("ana").collection("book").document("ben").get()
    assert doc.to_dict()["nick"] == "B"
    assert doc.to_dict()["familyId"] == fam
    assert doc.to_dict()["updatedBy"] == "ana"
    book_module.set_nick("ana", "ben", None, "ana")
    assert (_bv("pgr-t1"), _bv("pgr-t2")) == (2, 2)
    assert get_db().collection("users").document("ana").collection("book").document("ben").get().exists is False


# d ---------------------------------------------------------------------------


def test_put_nick_authorization(client: TestClient):
    fam, other = _family("A"), _family("B")
    _user("mom", fam, role="admin")
    h_kid = _user("kid", fam)
    h_sib = _user("sib", fam)
    h_mom = auth_header("mom")
    h_other_admin = _user("oadmin", other, role="admin")
    h_super = _user("root", None, role="super")

    def put(headers: dict[str, str]):
        return client.put("/api/book/kid/entries/sib", json={"nick": "S"}, headers=headers)

    assert put(h_mom).status_code == 200
    assert put(h_sib).status_code == 404
    assert put(h_other_admin).status_code == 404
    assert put(h_super).status_code == 200
    assert put(h_kid).status_code == 200
    assert client.get("/api/book?uid=kid", headers=h_sib).status_code == 404
    assert client.get("/api/book?uid=kid", headers=h_mom).status_code == 200
    assert client.get("/api/book?uid=nobody", headers=h_super).status_code == 404


# e ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "nick",
    ["x" * 17, "😀" * 16, "  ", "Mom\n", "a\x00b"],
)
def test_put_nick_rejects_invalid(client: TestClient, nick: str):
    fam = _family()
    h = _user("ana", fam)
    _user("ben", fam)
    assert len("😀" * 16) == 16 and len(("😀" * 16).encode()) == 64
    resp = client.put("/api/book/ana/entries/ben", json={"nick": nick}, headers=h)
    assert resp.status_code == 422, resp.text
    assert (
        get_db().collection("users").document("ana").collection("book").document("ben").get().exists
        is False
    )


def test_put_nick_trims_and_stores(client: TestClient):
    fam = _family()
    h = _user("ana", fam)
    _user("ben", fam)
    resp = client.put("/api/book/ana/entries/ben", json={"nick": " Mom "}, headers=h)
    assert resp.status_code == 200
    doc = get_db().collection("users").document("ana").collection("book").document("ben").get()
    assert doc.to_dict()["nick"] == "Mom"


def test_validate_nick_boundaries():
    assert book_module.validate_nick("x" * 16) == "x" * 16
    assert book_module.validate_nick("é" * 16) == "é" * 16  # 32 bytes
    with pytest.raises(ValueError):
        book_module.validate_nick("😀" * 13)  # 13 cp, 52 bytes


# g ---------------------------------------------------------------------------


def test_put_nick_for_peer_not_in_book_is_404(client: TestClient):
    fam, other = _family("A"), _family("B")
    h = _user("ana", fam)
    _user("stranger", other)
    assert (
        client.put("/api/book/ana/entries/stranger", json={"nick": "x"}, headers=h).status_code
        == 404
    )
    assert client.delete("/api/book/ana/entries/stranger", headers=h).status_code == 404
    assert client.put("/api/book/ana/entries/ghost", json={"nick": "x"}, headers=h).status_code == 404


# h ---------------------------------------------------------------------------


def test_create_member_bumps_every_family_device(client: TestClient, broker: FakeBrokerClient):
    fam = _family()
    h = _user("mom", fam, role="admin")
    _user("kid", fam)
    _device("pgr-h1", "mom", fam)
    _device("pgr-h2", "kid", fam)
    b1, b2 = _bv("pgr-h1"), _bv("pgr-h2")
    broker.clear()

    resp = client.post(
        "/api/family/members",
        json={"alias": "newbie", "displayName": "Newbie", "email": "newbie@example.com"},
        headers=h,
    )
    assert resp.status_code == 200, resp.text
    assert (_bv("pgr-h1"), _bv("pgr-h2")) == (b1 + 1, b2 + 1)
    assert len(_book_pushes(broker)) == 2
    assert "newbie" in [c["a"] for c in devcfg.build_book_body("pgr-h2")["c"]]


# i ---------------------------------------------------------------------------


def test_external_rename_rederives_cfg_sms_without_a_book_bump(
    client: TestClient, broker: FakeBrokerClient
):
    """A contact is never in `c[]` (it reaches the pager as `cfg.sms`), so a
    rename re-derives `cfg.sms` for its holders and bumps no book."""
    fam = _family()
    h = _user("mom", fam, role="admin")
    _user("kid1", fam)
    _user("kid2", fam)
    _device("pgr-i1", "kid1", fam)
    _device("pgr-i2", "kid2", fam)
    ext = externals_store.get_or_create(fam, "+12065550100", "Gran")
    for kid in ("kid1", "kid2"):
        allow_store.set_edge(kid, ext.uid, message=True, locate=False)
        _set_policy(kid, "people_sms", "people")
    b1, b2 = _bv("pgr-i1"), _bv("pgr-i2")
    broker.published.clear()

    resp = client.patch(f"/api/family/contacts/{ext.uid}", json={"name": "Grandma"}, headers=h)
    assert resp.status_code == 200, resp.text
    cfgs = [d for d in (json.loads(m.payload) for m in broker.published) if d.get("kind") == "cfg"]
    sms_cfgs = [d["cfg"]["sms"] for d in cfgs if "sms" in d.get("cfg", {})]
    assert len(sms_cfgs) == 2 and all(s == [{"n": "Grandma", "p": ext.phone}] for s in sms_cfgs)
    assert not _book_pushes(broker)
    assert (_bv("pgr-i1"), _bv("pgr-i2")) == (b1, b2)
    for device_id in ("pgr-i1", "pgr-i2"):
        assert [c.name for c in devices_store.get_device(device_id).smsContacts] == ["Grandma"]
        assert ext.alias not in [c["a"] for c in devcfg.build_book_body(device_id)["c"]]


# sms_contacts_for -----------------------------------------------------------


def _contact(fam: str, i: int, name: str):
    return externals_store.get_or_create(fam, f"+1206555{i:04d}", name)


def _sms_names(uid: str) -> list[str]:
    owner = users_store.get_user(uid)
    assert owner is not None
    return [u.displayName for u in book_module.sms_contacts_for(owner)]


def test_sms_contacts_for_open_member_includes_every_family_contact():
    fam = _family()
    _user("kid", fam, auth=False)
    _set_policy("kid", "open", "any")
    _contact(fam, 1, "Bob")
    _contact(fam, 2, "Alice")
    assert _sms_names("kid") == ["Alice", "Bob"]


def test_sms_contacts_for_any_sms_member_includes_every_family_contact():
    fam = _family()
    _user("kid", fam, auth=False)
    _set_policy("kid", "any_sms", "people")
    _contact(fam, 1, "Bob")
    assert _sms_names("kid") == ["Bob"]


def test_sms_contacts_for_people_member_includes_only_edge_contacts():
    fam = _family()
    _user("kid", fam, auth=False)  # default policy: people/people
    bob = _contact(fam, 1, "Bob")
    _contact(fam, 2, "Alice")
    assert _sms_names("kid") == []
    allow_store.set_edge("kid", bob.uid, message=True, locate=False)
    assert _sms_names("kid") == ["Bob"]


@pytest.mark.parametrize("out", ["people_sms", "sms"])
def test_sms_contacts_for_people_sms_and_sms_members_require_edges(out: str):
    fam = _family()
    _user("kid", fam, auth=False)
    _set_policy("kid", out, "people")
    bob = _contact(fam, 1, "Bob")
    _contact(fam, 2, "Alice")
    assert _sms_names("kid") == []
    allow_store.set_edge("kid", bob.uid, message=True, locate=False)
    assert _sms_names("kid") == ["Bob"]


def test_sms_contacts_for_explicit_deny_beats_open_policy():
    fam = _family()
    _user("kid", fam, auth=False)
    _set_policy("kid", "open", "any")
    bob = _contact(fam, 1, "Bob")
    _contact(fam, 2, "Alice")
    allow_store.set_edge("kid", bob.uid, message=False, locate=False)
    assert _sms_names("kid") == ["Alice"]


def test_sms_contacts_for_never_includes_other_family_contacts():
    fam, other = _family("A"), _family("B")
    _user("kid", fam, auth=False)
    _set_policy("kid", "open", "any")
    theirs = _contact(other, 1, "Theirs")
    mine = _contact(fam, 2, "Mine")
    allow_store.set_edge("kid", theirs.uid, message=True, locate=False)
    assert _sms_names("kid") == ["Mine"]
    assert mine.uid != theirs.uid


def test_sms_contacts_for_is_empty_for_a_disabled_or_non_person_owner():
    fam = _family()
    _user("kid", fam, auth=False)
    _set_policy("kid", "open", "any")
    ext = _contact(fam, 1, "Bob")
    users_store.update_user("kid", disabled=True)
    assert _sms_names("kid") == []
    assert book_module.sms_contacts_for(ext) == []


def test_sms_contacts_for_sorted_by_truncated_name_casefold():
    fam = _family()
    _user("kid", fam, auth=False)
    _set_policy("kid", "open", "any")
    _contact(fam, 1, "charlie")
    _contact(fam, 2, "Bob")
    _contact(fam, 3, "alice")
    assert _sms_names("kid") == ["alice", "Bob", "charlie"]
    # Names equal after truncation (not creatable through the API) tie-break
    # by uid.
    for uid in ("x_b", "x_a"):
        users_store.create_user(
            uid=uid,
            alias=uid.replace("_", ""),
            display_name="ZZZ sixteen chars A" if uid == "x_a" else "ZZZ sixteen chars B",
            phone="+12065559999" if uid == "x_a" else "+12065559998",
            kind="external",
            owner_family_id=fam,
        )
    owner = users_store.get_user("kid")
    assert [u.uid for u in book_module.sms_contacts_for(owner)][-2:] == ["x_a", "x_b"]


def test_external_entry_comes_from_sms_contacts_for_and_is_sendable_with_phone(
    client: TestClient,
):
    fam, other = _family("A"), _family("B")
    h = _user("kid", fam)  # people: only an explicit edge lists a contact
    bob = _contact(fam, 1, "Bob")
    _contact(fam, 2, "Unlisted")
    theirs = _contact(other, 3, "Theirs")
    allow_store.set_edge("kid", bob.uid, message=True, locate=False)
    allow_store.set_edge("kid", theirs.uid, message=True, locate=False)  # a stale cross-family edge

    entries = book_module.entries_for("kid")
    externals = [e for e in entries if e.kind == "external"]
    assert [e.uid for e in externals] == [bob.uid]
    (entry,) = externals
    assert entry.phone == "+12065550001"
    # No relay number for `kid`: the contact is the modem's, not sendable by the relay.
    assert entry.sendable is False and entry.reason == "no_sms_number"
    assert entry.inFamily is False and entry.onPager is True
    assert [e.sendable for e in book_module.entries_for("kid") if e.kind == "external"] == [False]

    # With a relay number (policy people_sms + the edge) the same contact is sendable.
    _set_policy("kid", "people_sms", "people")
    users_store.set_sms_number("kid", "+12065550999")
    (entry,) = [e for e in book_module.entries_for("kid") if e.kind == "external"]
    assert entry.sendable is True and entry.reason is None and entry.onPager is True

    body = client.get("/api/book", headers=h).json()
    assert _aliases(body)[bob.alias]["phone"] == "+12065550001"
    assert theirs.alias not in _aliases(body)


def test_open_member_book_lists_family_contacts_sendable_with_phone_and_on_pager_cap(
    client: TestClient,
):
    fam = _family()
    h = _user("kid", fam)
    _set_policy("kid", "open", "people")
    _device("pgr-open", "kid", fam)
    for i in range(10):
        _contact(fam, i, f"c{i}")

    body = client.get("/api/book", headers=h).json()
    externals = sorted((e for e in body["entries"] if e["kind"] == "external"), key=lambda e: e["label"])
    assert [e["label"] for e in externals] == [f"c{i}" for i in range(10)]
    assert all(e["phone"] and not e["sendable"] and e["reason"] == "no_sms_number" for e in externals)
    assert [e["onPager"] for e in externals] == [True] * 8 + [False] * 2
    # And none of them is in the pager's `c[]` (no relay number: they are `cfg.sms`).
    assert devcfg.build_book_body("pgr-open")["c"] == []

    # With a relay number every contact is in `c[]` as t:"sms".
    users_store.set_sms_number("kid", "+12065550999")
    c = devcfg.build_book_body("pgr-open")["c"]
    assert len(c) == 10 and {x["t"] for x in c} == {"sms"}


# j ---------------------------------------------------------------------------


def test_more_than_32_entries_truncates(client: TestClient):
    fam = _family()
    h = _user("owner", fam)
    _device("pgr-j", "owner", fam)
    for i in range(33):
        _user(f"m{i:02d}", fam, auth=False)

    body = client.get("/api/book", headers=h).json()
    assert body["pagerCap"] == 32
    assert body["truncated"] is True
    assert len(body["entries"]) == 33
    assert sum(1 for e in body["entries"] if e["onPager"]) == 32
    pull = devcfg.build_book_body("pgr-j")
    assert pull["more"] is True
    assert len(pull["c"]) == 32


# k ---------------------------------------------------------------------------


def test_disabled_member_not_listed(client: TestClient):
    fam = _family()
    h = _user("ana", fam)
    _user("ben", fam)
    _user("cat", fam)
    users_store.update_user("cat", disabled=True)
    assert set(_aliases(client.get("/api/book", headers=h).json())) == {"ben"}


# extras ----------------------------------------------------------------------


def test_edge_peers_groups_and_dedup(client: TestClient):
    fam, other = _family("A"), _family("B")
    h = _user("ana", fam)
    _user("ben", fam)
    _user("zed", other)
    allow_store.set_edge("ana", "ben", message=True, locate=False)  # also family: one entry
    allow_store.set_edge("ana", "zed", message=True, locate=False)
    allow_store.set_edge("zed", "ana", message=True, locate=False)
    aliases = _aliases(client.get("/api/book", headers=h).json())
    assert set(aliases) == {"ben", "zed"}
    assert aliases["zed"]["inFamily"] is False
    assert aliases["zed"]["sendable"] is True


def test_disabling_member_bumps_family_books(client: TestClient):
    fam = _family()
    h = _user("mom", fam, role="admin")
    _user("kid", fam)
    _device("pgr-x", "mom", fam)
    before = _bv("pgr-x")
    resp = client.patch("/api/family/members/kid", json={"disabled": True}, headers=h)
    assert resp.status_code == 200, resp.text
    assert _bv("pgr-x") == before + 1
    assert devcfg.build_book_body("pgr-x")["c"] == []


def test_policy_change_bumps_family_books(client: TestClient):
    fam = _family()
    h = _user("mom", fam, role="admin")
    _user("kid", fam)
    _device("pgr-p", "kid", fam)
    before = _bv("pgr-p")
    resp = client.patch(
        "/api/family/members/kid", json={"policy": {"out": "sms", "in": "people"}}, headers=h
    )
    assert resp.status_code == 200, resp.text
    assert _bv("pgr-p") == before + 1
    assert devcfg.build_book_body("pgr-p")["c"] == []
