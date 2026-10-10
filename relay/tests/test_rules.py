"""`relay/firestore.rules`, tested through the Firestore emulator's own REST
API with real ID tokens -- not relay-side logic. This is the one place
`firestore.rules` enforcement is independently verified: everything the
relay does bypasses these rules entirely (firebase-admin), so a bug in the
rules file would otherwise go undetected until a real client (the web app)
tripped over it. docs/SERVER_PLAN.md §5.9 calls this out explicitly:
"a signed-in, registered user cannot read another pair's messages/{id} or an
unrelated conversations/{k} ... test this through the emulator's REST API
with a real ID token".

Firestore's REST API evaluates security rules against the bearer token on
every call, exactly like the JS/Web SDK a real browser uses -- the Python
`google-cloud-firestore`/firebase-admin client always uses admin
credentials, which bypass rules entirely, so it cannot be used here.
"""

from __future__ import annotations

import os
import time

import httpx
import pytest
from firebase_admin import auth as fb_auth

from app.db.firestore import get_db
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import cas as cas_store
from app.store import cells as cells_store
from app.store import conversations as conversations_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import users as users_store
from tests.firebase_test_utils import mint_id_token, seed_legacy_contact_request

FIRESTORE_HOST = os.environ.get("FIRESTORE_EMULATOR_HOST", "localhost:8080")
PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "demo-pager")
BASE_URL = f"http://{FIRESTORE_HOST}/v1/projects/{PROJECT_ID}/databases/(default)/documents"


def _get(path: str, token: str | None) -> httpx.Response:
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    return httpx.get(f"{BASE_URL}/{path}", headers=headers, timeout=10.0)


def _write(path: str, token: str | None, fields: dict) -> httpx.Response:
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    # Firestore REST's document body shape: {"fields": {name: {stringValue: ...}}}.
    # Only used here to prove clients CANNOT write -- values don't matter.
    body = {"fields": {k: {"stringValue": str(v)} for k, v in fields.items()}}
    return httpx.patch(f"{BASE_URL}/{path}", headers=headers, json=body, timeout=10.0)


def _run_query(parent_path: str, token: str | None, body: dict) -> httpx.Response:
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    base = f"{BASE_URL}/{parent_path}" if parent_path else BASE_URL
    return httpx.post(f"{base}:runQuery", headers=headers, json=body, timeout=10.0)


def set_claims(uid: str, *, role: str | None = None, fam: str | None = None) -> None:
    """Sets exactly the `role`/`fam` custom claims `firestore.rules`'
    `role()`/`fam()` helpers read (docs/FAMILIES_DESIGN.md §1 decision 2) --
    the legacy `admin` claim is never set by this helper; a caller wanting to
    pin "a bare `admin: true` claim grants nothing" sets that claim directly
    via `fb_auth.set_custom_user_claims`, as the pre-families tests already
    do below."""
    claims: dict[str, str] = {}
    if role is not None:
        claims["role"] = role
    if fam is not None:
        claims["fam"] = fam
    fb_auth.set_custom_user_claims(uid, claims)


def _set_family(collection: str, doc_id: str, family_id: str | None) -> None:
    """Stamps `familyId` straight onto an existing document via the admin
    SDK (bypassing rules, like every other store write in this file) --
    `families.py`/the `familyId` fields on `users`/`devices`/
    `contactRequests` are task 1.1's own `Files`, not this task's, so tests
    here that need a document to carry a `familyId` write it directly rather
    than depending on 1.1's still-landing store APIs."""
    get_db().collection(collection).document(doc_id).set(
        {"familyId": family_id}, merge=True
    )


def _set_allow_family_ids(from_uid: str, to_uid: str, family_ids: list[str]) -> None:
    get_db().collection("allow").document(f"{from_uid}_{to_uid}").set(
        {"familyIds": family_ids}, merge=True
    )


def _set_family_ids(collection: str, doc_id: str, family_ids: list[str]) -> None:
    """Stamps `familyIds` straight onto an existing `conversations`/
    `messages` document via the admin SDK -- 2.1 (concurrent with this task)
    is what makes `create_message`/group-create write this field for real;
    until it lands, tests here stamp it directly, exactly as 1.4 did for
    `devices`/`users`/`contactRequests` (`_set_family` above)."""
    get_db().collection(collection).document(doc_id).set(
        {"familyIds": family_ids}, merge=True
    )


def _family_ids_query(collection: str, family_id: str) -> dict:
    return {
        "structuredQuery": {
            "from": [{"collectionId": collection}],
            "where": {
                "fieldFilter": {
                    "field": {"fieldPath": "familyIds"},
                    "op": "ARRAY_CONTAINS",
                    "value": {"stringValue": family_id},
                }
            },
        }
    }


def _make_family_admin(uid: str, alias: str, fam: str) -> str:
    """Creates a registered `role: admin` user claimed for `fam` and
    returns a fresh ID token for them."""
    fb_auth.create_user(uid=uid, email=f"{uid}@example.com")
    users_store.create_user(uid=uid, alias=alias, display_name="Admin", role="admin")
    set_claims(uid, role="admin", fam=fam)
    return mint_id_token(uid)


def _create_family(family_id: str, name: str = "Fam") -> None:
    get_db().collection("families").document(family_id).set(
        {"name": name, "blockedNumbers": [], "createdBy": "system"}
    )


@pytest.fixture
def two_pairs():
    """u1<->u2 have a message/conversation; u3 is unrelated to both."""
    for uid, email, alias in (
        ("u1", "u1@example.com", "alice"),
        ("u2", "u2@example.com", "bob"),
        ("u3", "u3@example.com", "carol"),
    ):
        fb_auth.create_user(uid=uid, email=email)
        users_store.create_user(uid=uid, alias=alias, display_name=alias)

    msg = messages_store.create_message(
        sender_uid="u1", recipient_uid="u2", kind="text", ts=1000, body="hello"
    )
    return msg


def test_party_can_read_their_own_message(two_pairs):
    msg = two_pairs
    token = mint_id_token("u1")
    resp = _get(f"messages/{msg.id}", token)
    assert resp.status_code == 200


def test_non_party_cannot_read_the_message(two_pairs):
    msg = two_pairs
    token = mint_id_token("u3")
    resp = _get(f"messages/{msg.id}", token)
    assert resp.status_code == 403


def test_unauthenticated_cannot_read_the_message(two_pairs):
    msg = two_pairs
    resp = _get(f"messages/{msg.id}", None)
    assert resp.status_code == 403


def test_party_can_list_query_their_thread(two_pairs):
    """The web app's thread listener (`web/app/chat/[alias]/
    ThreadPageClient.tsx`): `messages` filtered by `convKey` **and** by
    `uids array-contains <self>`. The second filter is what makes the query
    authorisable -- a `list` is checked abstractly against the query's own
    declared filters, before any document is read, so a `convKey`-only query
    asserts nothing about who may read the result and is denied (the next
    test pins that). Single-document `get`s above never exercised this, which
    is how a thread that 403'd in the browser passed the suite."""
    key = messages_store.conv_key("u1", "u2")
    body = {
        "structuredQuery": {
            "from": [{"collectionId": "messages"}],
            "where": {
                "compositeFilter": {
                    "op": "AND",
                    "filters": [
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "convKey"},
                                "op": "EQUAL",
                                "value": {"stringValue": key},
                            }
                        },
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "uids"},
                                "op": "ARRAY_CONTAINS",
                                "value": {"stringValue": "u1"},
                            }
                        },
                    ],
                }
            },
            "orderBy": [{"field": {"fieldPath": "seq"}, "direction": "DESCENDING"}],
            "limit": 50,
        }
    }
    resp = _run_query("", mint_id_token("u1"), body)
    assert resp.status_code == 200, resp.text
    assert any("document" in entry for entry in resp.json()), resp.text


def test_thread_list_query_without_the_uids_filter_is_denied(two_pairs):
    """The same query minus `uids array-contains` -- denied even for a real
    party to the conversation, because the rule has nothing to prove itself
    from. Pinned so nobody "simplifies" the redundant-looking filter out of
    `ThreadPageClient.tsx` and silently empties every thread."""
    key = messages_store.conv_key("u1", "u2")
    body = {
        "structuredQuery": {
            "from": [{"collectionId": "messages"}],
            "where": {
                "fieldFilter": {
                    "field": {"fieldPath": "convKey"},
                    "op": "EQUAL",
                    "value": {"stringValue": key},
                }
            },
        }
    }
    resp = _run_query("", mint_id_token("u1"), body)
    assert resp.status_code == 403, resp.text


def test_non_party_cannot_list_query_a_thread(two_pairs):
    """A `uids array-contains u3` filter is provable, but matches nothing of
    u1<->u2's -- an outsider gets an empty result set, never other people's
    messages."""
    body = {
        "structuredQuery": {
            "from": [{"collectionId": "messages"}],
            "where": {
                "fieldFilter": {
                    "field": {"fieldPath": "uids"},
                    "op": "ARRAY_CONTAINS",
                    "value": {"stringValue": "u3"},
                }
            },
        }
    }
    resp = _run_query("", mint_id_token("u3"), body)
    assert resp.status_code == 200, resp.text
    assert not any("document" in entry for entry in resp.json()), resp.text


def test_party_can_read_their_conversation(two_pairs):
    key = messages_store.conv_key("u1", "u2")
    token = mint_id_token("u2")
    resp = _get(f"conversations/{key}", token)
    assert resp.status_code == 200


def test_non_party_cannot_read_an_unrelated_conversation(two_pairs):
    key = messages_store.conv_key("u1", "u2")
    token = mint_id_token("u3")
    resp = _get(f"conversations/{key}", token)
    assert resp.status_code == 403


def test_self_can_read_own_user_doc(two_pairs):
    token = mint_id_token("u1")
    resp = _get("users/u1", token)
    assert resp.status_code == 200


def test_other_user_cannot_read_someone_elses_user_doc(two_pairs):
    token = mint_id_token("u3")
    resp = _get("users/u1", token)
    assert resp.status_code == 403


def test_unregistered_signed_in_uid_cannot_read_settings(two_pairs):
    # A real Firebase Auth account, but no users/{uid} doc -- registered()
    # must fail, same gate app.auth.require_user enforces server-side.
    fb_auth.create_user(uid="stranger", email="stranger@example.com")
    token = mint_id_token("stranger")
    resp = _get("settings/retention", token)
    assert resp.status_code == 403


def test_registered_user_can_read_settings(two_pairs):
    from app.store import settings as settings_store

    settings_store.set_retention(
        messages=settings_store.RetentionSetting(n=4, unit="weeks"),
        locations=settings_store.RetentionSetting(n=1, unit="weeks"),
    )
    token = mint_id_token("u1")
    resp = _get("settings/retention", token)
    assert resp.status_code == 200


def test_device_owner_can_read_their_device(two_pairs):
    devices_store.create_device(
        device_id="pgr-rules-1",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-1",
        mqtt_password_hash="x",
    )
    # devices/{d} now requires the reader to share the device's family
    # (FAMILIES_DESIGN.md §1 decision 5) -- in production the owner always
    # does, by construction (task 1.1 copies the owner's familyId onto the
    # device at creation); this fixture makes that explicit.
    _set_family("devices", "pgr-rules-1", "famA")
    set_claims("u2", fam="famA")
    token = mint_id_token("u2")
    resp = _get("devices/pgr-rules-1", token)
    assert resp.status_code == 200


def test_non_owner_non_locator_cannot_read_device(two_pairs):
    devices_store.create_device(
        device_id="pgr-rules-2",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-2",
        mqtt_password_hash="x",
    )
    token = mint_id_token("u3")
    resp = _get("devices/pgr-rules-2", token)
    assert resp.status_code == 403


def test_locatable_by_uid_can_read_device_and_its_locations(two_pairs):
    devices_store.create_device(
        device_id="pgr-rules-3",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-3",
        mqtt_password_hash="x",
    )
    devices_store.set_locatable_by("pgr-rules-3", ["u1"])
    _set_family("devices", "pgr-rules-3", "famA")
    set_claims("u1", fam="famA")

    token = mint_id_token("u1")
    resp = _get("devices/pgr-rules-3", token)
    assert resp.status_code == 200

    from app.store.locations import LocationFix, add_location

    loc_id = add_location(
        "pgr-rules-3", LocationFix(ts=1, fixTs=1, lat=1.0, lon=2.0)
    )
    resp2 = _get(f"devices/pgr-rules-3/locations/{loc_id}", token)
    assert resp2.status_code == 200

    other_token = mint_id_token("u3")
    resp3 = _get(f"devices/pgr-rules-3/locations/{loc_id}", other_token)
    assert resp3.status_code == 403


def test_owner_can_read_own_device_locations_but_not_non_owner_or_admin(two_pairs):
    """Owner decision (27 Sep 2026): a device's owner reads its own
    `locations` with no `locate` edge. It is owner-only, not admin-sees-all:
    a non-owner without `locatableBy` and an admin who is neither owner nor
    in `locatableBy` are both still denied, for `get` and `list`."""
    from app.store.locations import LocationFix, add_location

    devices_store.create_device(
        device_id="pgr-rules-own-1",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-own-1",
        mqtt_password_hash="x",
    )
    _set_family("devices", "pgr-rules-own-1", "famA")
    set_claims("u2", fam="famA")
    loc_id = add_location("pgr-rules-own-1", LocationFix(ts=1, fixTs=1, lat=1.0, lon=2.0))
    path = f"devices/pgr-rules-own-1/locations/{loc_id}"
    list_query = {
        "structuredQuery": {
            "from": [{"collectionId": "locations"}],
            "orderBy": [{"field": {"fieldPath": "createdAt"}, "direction": "DESCENDING"}],
        }
    }

    owner_token = mint_id_token("u2")
    assert _get(path, owner_token).status_code == 200
    resp = _run_query("devices/pgr-rules-own-1", owner_token, list_query)
    assert resp.status_code == 200, resp.text
    assert len([row for row in resp.json() if "document" in row]) == 1

    other_token = mint_id_token("u3")
    assert _get(path, other_token).status_code == 403
    assert _run_query("devices/pgr-rules-own-1", other_token, list_query).status_code == 403

    fb_auth.create_user(uid="admin-loc-1", email="admin-loc-1@example.com")
    users_store.create_user(uid="admin-loc-1", alias="adminloc1", display_name="Admin", role="admin")
    fb_auth.set_custom_user_claims("admin-loc-1", {"admin": True})
    admin_token = mint_id_token("admin-loc-1")
    assert _get(path, admin_token).status_code == 403
    assert _run_query("devices/pgr-rules-own-1", admin_token, list_query).status_code == 403

    assert _get(path, None).status_code == 403


def test_locations_are_client_read_only(two_pairs):
    """docs/LOCATION_TRACKING_DESIGN.md §5's owner note (this task): "the
    rules are unchanged and still relay-write-only" even with tracking on --
    a create/update of `devices/{d}/locations/{id}` by the device's own
    owner (who can *read* it, per the test above) is still denied.
    `firestore.rules` has no write rule for this collection, so this should
    already pass; added because the task spec calls it out explicitly."""
    devices_store.create_device(
        device_id="pgr-rules-3b",
        owner_uid="u1",
        label="d",
        mqtt_username="pgr-rules-3b",
        mqtt_password_hash="x",
    )
    token = mint_id_token("u1")
    resp = _write("devices/pgr-rules-3b/locations/fake1", token, {"lat": 1.0, "lon": 2.0})
    assert resp.status_code == 403


def test_locatable_by_uid_can_list_query_devices_and_their_locations(two_pairs):
    """A Firestore *list* (collection query)
    request evaluates `allow read` abstractly, against the query's own
    declared filters alone, before touching any real document -- unlike the
    single-document `get`s the rest of this file exercises. A bare
    `resource.data.locatableBy`/`request.auth.token.admin` field access
    throws `PERMISSION_DENIED: Property ...  is
    undefined on object` for *every* `list` query, and even the null-safe
    `.get(key, default)` fix alone still can't make a query filtered only on
    `ownerUid` succeed for a `locatableBy`-only-authorized caller (Firestore
    can't statically prove the rule from that filter). This is
    `tools/pager_client.py`'s `ServerClient.locations()` path
    (`_device_id_for_owner` filters on `locatableBy array_contains
    <caller>`, which Firestore *can* verify) -- exercised here directly
    against the emulator's real rule enforcement, the same as every other
    test in this file."""
    devices_store.create_device(
        device_id="pgr-rules-4",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-4",
        mqtt_password_hash="x",
    )
    devices_store.set_locatable_by("pgr-rules-4", ["u1"])
    _set_family("devices", "pgr-rules-4", "famA")
    set_claims("u1", fam="famA")
    from app.store.locations import LocationFix, add_location

    add_location("pgr-rules-4", LocationFix(ts=1, fixTs=1, lat=1.0, lon=2.0))

    token = mint_id_token("u1")
    # The `devices/{d}` predicate now also requires `sameFam(familyId)`
    # (FAMILIES_DESIGN.md §1 decision 5), which the abstract `list`
    # pre-check cannot prove from a `locatableBy`-only filter (`familyId`
    # isn't one of the query's own declared filters) -- same class of
    # "unprovable, so denied outright" problem this file's own comments
    # describe for `locatableBy`/`admin` above. A real client must now also
    # filter on `familyId` (FAMILIES_DESIGN.md §5.3: "every query adds
    # `where('familyId','==', fam)`"), so this query does too.
    devices_query = {
        "structuredQuery": {
            "from": [{"collectionId": "devices"}],
            "where": {
                "compositeFilter": {
                    "op": "AND",
                    "filters": [
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "locatableBy"},
                                "op": "ARRAY_CONTAINS",
                                "value": {"stringValue": "u1"},
                            }
                        },
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "familyId"},
                                "op": "EQUAL",
                                "value": {"stringValue": "famA"},
                            }
                        },
                    ],
                }
            },
        }
    }
    resp = _run_query("", token, devices_query)
    assert resp.status_code == 200, resp.text
    names = [row["document"]["name"] for row in resp.json() if "document" in row]
    assert any(n.endswith("/devices/pgr-rules-4") for n in names)

    locations_query = {
        "structuredQuery": {
            "from": [{"collectionId": "locations"}],
            "orderBy": [{"field": {"fieldPath": "createdAt"}, "direction": "DESCENDING"}],
        }
    }
    resp2 = _run_query("devices/pgr-rules-4", token, locations_query)
    assert resp2.status_code == 200, resp2.text
    assert len([row for row in resp2.json() if "document" in row]) == 1

    # A same-family caller with no locatableBy entry gets an empty (not an
    # error) result for the devices-list query -- the array-contains filter
    # itself excludes it, matching PROTOCOL/SERVER_PLAN's "not permitted"
    # outcome.
    set_claims("u3", fam="famA")
    other_token = mint_id_token("u3")
    other_query = {
        "structuredQuery": {
            "from": [{"collectionId": "devices"}],
            "where": {
                "compositeFilter": {
                    "op": "AND",
                    "filters": [
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "locatableBy"},
                                "op": "ARRAY_CONTAINS",
                                "value": {"stringValue": "u3"},
                            }
                        },
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "familyId"},
                                "op": "EQUAL",
                                "value": {"stringValue": "famA"},
                            }
                        },
                    ],
                }
            },
        }
    }
    resp3 = _run_query("", other_token, other_query)
    assert resp3.status_code == 200, resp3.text
    assert [row for row in resp3.json() if "document" in row] == []


def test_allow_edge_readable_by_either_party_not_a_third_party(two_pairs):
    allow_store.set_edge("u1", "u2", message=True, locate=True)
    token_party = mint_id_token("u2")
    resp = _get("allow/u1_u2", token_party)
    assert resp.status_code == 200

    token_other = mint_id_token("u3")
    resp2 = _get("allow/u1_u2", token_other)
    assert resp2.status_code == 403


def test_clients_cannot_write_anything(two_pairs):
    token = mint_id_token("u1")
    resp = _write("users/u1", token, {"displayName": "hacked"})
    assert resp.status_code == 403


# ---------------------------------------------------------------------------
# Default-deny on the server-only lookup collections -- docs/SERVER_PLAN.md §3
# ("None of these is client-readable: firestore.rules has no match block for
# them (default-deny read) ... relay/tests/test_rules.py should pin it so a
# future broadened rule cannot expose them").
# ---------------------------------------------------------------------------


def test_gchat_spaces_is_default_deny(two_pairs):
    backends_store.set_gchat_space("SPACERULES1", "u1", "bid1", "users/111")

    owner_token = mint_id_token("u1")
    resp = _get("gchatSpaces/SPACERULES1", owner_token)
    assert resp.status_code == 403

    resp_unauth = _get("gchatSpaces/SPACERULES1", None)
    assert resp_unauth.status_code == 403

    resp_write = _write("gchatSpaces/SPACERULES1", owner_token, {"uid": "u3"})
    assert resp_write.status_code == 403


def test_gchat_link_codes_is_default_deny(two_pairs):
    backends_store.set_gchat_link_code("999111", "u1", "bid1", int(time.time()) + 600)

    owner_token = mint_id_token("u1")
    resp = _get("gchatLinkCodes/999111", owner_token)
    assert resp.status_code == 403

    resp_unauth = _get("gchatLinkCodes/999111", None)
    assert resp_unauth.status_code == 403

    resp_write = _write("gchatLinkCodes/999111", owner_token, {"uid": "u3"})
    assert resp_write.status_code == 403


def test_contact_names_is_default_deny(two_pairs):
    """`contactNames/{fid}_{hash}` (the per-family unique-name reservation
    behind SMS contacts, `app/store/externals.py`) is relay-only: no client
    -- not even a member of the owning family -- may read or write it."""
    from app.store import externals as externals_store

    ext = externals_store.get_or_create("fam-rules", "+15550001111", "Grandma")
    doc_id = externals_store._name_ref("fam-rules", externals_store.name_key("Grandma")).id

    owner_token = mint_id_token("u1")
    resp = _get(f"contactNames/{doc_id}", owner_token)
    assert resp.status_code == 403
    assert ext.uid  # the reservation exists because the contact was created

    resp_unauth = _get(f"contactNames/{doc_id}", None)
    assert resp_unauth.status_code == 403

    resp_write = _write(f"contactNames/{doc_id}", owner_token, {"uid": "hacked"})
    assert resp_write.status_code == 403


def test_sms_numbers_and_held_sms_are_default_deny(two_pairs):
    """`smsNumbers/{e164}` (the number -> uid reverse index) and
    `heldSms/{sid}` (inbound texts awaiting a parent's decision,
    docs/RELAY_SMS_DESIGN.md) are relay-only: no client, not even the
    number's owner or a family member, may read or write either."""
    from app.store import held_sms as held_sms_store
    from app.store import users as users_store

    users_store.set_sms_number("u1", "+15550007777")
    held_sms_store.create(
        "SMrules1", family_id="fam-rules", to_uid="u1", from_phone="+15550008888", body="secret"
    )
    owner_token = mint_id_token("u1")
    for path in ("smsNumbers/+15550007777", "heldSms/SMrules1"):
        assert _get(path, owner_token).status_code == 403
        assert _get(path, None).status_code == 403
        assert _write(path, owner_token, {"uid": "hacked"}).status_code == 403


def test_bridge_collections_are_default_deny(two_pairs):
    """docs/BRIDGE_PHONE_DESIGN.md decision 1/15: `bridges/{b}`, its `outbox`,
    `bridgePairCodes/{c}`, `bridgeConversations/{r}` and `heldChat/{h}` carry
    token hashes, FCM tokens and chat text; no match block, so nobody -- a
    family admin, a member, another family's admin, an unauthenticated
    caller -- reads or writes any of them."""
    from app.store import bridge_conversations as conv_store
    from app.store import bridge_outbox
    from app.store import bridges as bridges_store
    from app.store import held_chat as held_chat_store

    _create_family("fam-bridge")
    admin_token = _make_family_admin("bridge-admin", "badmin", "fam-bridge")
    other_admin_token = _make_family_admin("bridge-admin2", "badmin2", "fam-other")
    member_token = mint_id_token("u1")
    bridge = bridges_store.create("u1", "fam-bridge", "phone", "bridge-admin")
    item = bridge_outbox.enqueue_send(bridge, "m_1", "sms", source="sms", to={}, text="secret")
    code, _ = bridges_store.create_pair_code(bridge.id)
    row = conv_store.upsert_seen(
        bridge, source="gchat", conversation_id="c1", title="t", is_group=False, link=None,
        speaker="x", people=[], preview="p",
    )
    held_chat_store.create(
        "br_x_1", bridge_id=bridge.id, conversation_id="c1", conv_row_id=row.id,
        family_id="fam-bridge", to_uid="u1", sender_name="x", body="secret",
    )
    paths = (
        f"bridges/{bridge.id}",
        f"bridges/{bridge.id}/outbox/{item.id}",
        f"bridgePairCodes/{code}",
        f"bridgeConversations/{row.id}",
        "heldChat/br_x_1",
    )
    for token in (admin_token, other_admin_token, member_token, None):
        for path in paths:
            assert _get(path, token).status_code == 403, (path, token is None)
            assert _write(path, token, {"tokenHash": "hacked"}).status_code == 403


def test_device_secrets_is_default_deny(two_pairs):
    """`deviceSecrets/{d}` (docs/DEVICE_PLAN.md §2.6) holds the device's HMAC
    key and MQTT password hash -- unreadable by the device's own owner (whose
    browser can read `devices/{d}` itself) and unreadable by an admin client
    (whose elevated `request.auth.token.admin` claim reaches `devices/{d}`
    and `users/{uid}` but must not reach this collection either), the same
    default-deny-with-no-`match`-block posture the other server-only collections pin."""
    devices_store.create_device(
        device_id="pgr-secret-rules-1",
        owner_uid="u1",
        label="d",
        mqtt_username="pgr-secret-rules-1",
        mqtt_password_hash="x",
    )
    device_secrets_store.create(
        "pgr-secret-rules-1", hmac_key=b"k" * 32, mqtt_password_hash="hash"
    )

    owner_token = mint_id_token("u1")
    resp = _get("deviceSecrets/pgr-secret-rules-1", owner_token)
    assert resp.status_code == 403

    fb_auth.create_user(uid="admin1", email="admin1@example.com")
    users_store.create_user(uid="admin1", alias="admin1", display_name="Admin", role="admin")
    fb_auth.set_custom_user_claims("admin1", {"admin": True})
    admin_token = mint_id_token("admin1")
    resp_admin = _get("deviceSecrets/pgr-secret-rules-1", admin_token)
    assert resp_admin.status_code == 403

    resp_unauth = _get("deviceSecrets/pgr-secret-rules-1", None)
    assert resp_unauth.status_code == 403

    resp_write = _write(
        "deviceSecrets/pgr-secret-rules-1", owner_token, {"mqttPasswordHash": "hacked"}
    )
    assert resp_write.status_code == 403


def test_cas_is_default_deny(two_pairs):
    """`cas/{sha256hex}` (docs/V02_DESIGN.md §4.4) holds a CA PEM the relay
    has served a pointer for -- not secret (a CA is public by construction),
    but there is deliberately no client Firestore read path for it either:
    the one public read is `GET /ca/{sha256hex}.pem` (`app/routers/ca.py`),
    which can apply its own cache headers and 404 semantics. Same default-
    deny-with-no-`match`-block posture `deviceSecrets` above
    pin -- unreadable by an ordinary registered user and by an admin alike."""
    sha_hex = "ab" * 32
    cas_store.remember(sha_hex, "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n")

    owner_token = mint_id_token("u1")
    resp = _get(f"cas/{sha_hex}", owner_token)
    assert resp.status_code == 403

    fb_auth.create_user(uid="admin-cas", email="admin-cas@example.com")
    users_store.create_user(uid="admin-cas", alias="admincas", display_name="Admin", role="admin")
    fb_auth.set_custom_user_claims("admin-cas", {"admin": True})
    admin_token = mint_id_token("admin-cas")
    resp_admin = _get(f"cas/{sha_hex}", admin_token)
    assert resp_admin.status_code == 403

    resp_unauth = _get(f"cas/{sha_hex}", None)
    assert resp_unauth.status_code == 403

    resp_write = _write(f"cas/{sha_hex}", owner_token, {"pem": "hacked"})
    assert resp_write.status_code == 403


def test_cells_is_default_deny(two_pairs):
    """`cells/{mcc}-{mnc}-{tac}-{ci}` (docs/PROTOCOL.md §13.2, this task):
    the cell-tower geolocation cache. Nothing in it is useful to a client
    directly (the web app reads resolved fixes through
    `devices/{d}/locations`, not this collection), so it gets the same
    default-deny-with-no-`match`-block posture as `cas`/`deviceSecrets`
    -- unreadable by an ordinary registered user and by an
    admin alike."""
    cells_store.remember_resolved(
        "310", "410", 12345, 87654321, lat=1.0, lon=2.0, acc_m=1000, provider="google"
    )
    key = cells_store.cache_key("310", "410", 12345, 87654321)

    owner_token = mint_id_token("u1")
    resp = _get(f"cells/{key}", owner_token)
    assert resp.status_code == 403

    fb_auth.create_user(uid="admin-cells", email="admin-cells@example.com")
    users_store.create_user(uid="admin-cells", alias="admincells", display_name="Admin", role="admin")
    fb_auth.set_custom_user_claims("admin-cells", {"admin": True})
    admin_token = mint_id_token("admin-cells")
    resp_admin = _get(f"cells/{key}", admin_token)
    assert resp_admin.status_code == 403

    resp_unauth = _get(f"cells/{key}", None)
    assert resp_unauth.status_code == 403

    resp_write = _write(f"cells/{key}", owner_token, {"lat": 999.0})
    assert resp_write.status_code == 403


def test_contact_request_readable_by_owner_and_family_admin_not_a_third_party(two_pairs):
    """`contactRequests/{deviceId}_{reqId}` (docs/DEVICE_PLAN.md §4.1):
    readable by the device owner and by a family admin of the request's
    family (claims-only, docs/FAMILIES_TASKS.md 1.4), not by an unrelated
    registered user, not by another family's admin, and not by a bare
    `admin: true` claim with no `role`/`fam`."""
    from app.store import contacts as contacts_store

    devices_store.create_device(
        device_id="pgr-rules-contacts-1",
        owner_uid="u1",
        label="d",
        mqtt_username="pgr-rules-contacts-1",
        mqtt_password_hash="x",
    )
    seed_legacy_contact_request(
        "pgr-rules-contacts-1", "u1", "u_rules1", "Grandma", "+15551230000"
    )
    doc_key = contacts_store.key("pgr-rules-contacts-1", "u_rules1")
    _set_family("contactRequests", doc_key, "famA")

    owner_token = mint_id_token("u1")
    resp_owner = _get(f"contactRequests/{doc_key}", owner_token)
    assert resp_owner.status_code == 200

    other_token = mint_id_token("u3")
    resp_other = _get(f"contactRequests/{doc_key}", other_token)
    assert resp_other.status_code == 403

    fb_auth.create_user(uid="admin-contacts-1", email="admin-contacts-1@example.com")
    users_store.create_user(
        uid="admin-contacts-1", alias="admincontacts1", display_name="Admin", role="admin"
    )
    set_claims("admin-contacts-1", role="admin", fam="famA")
    admin_token = mint_id_token("admin-contacts-1")
    resp_admin = _get(f"contactRequests/{doc_key}", admin_token)
    assert resp_admin.status_code == 200

    fb_auth.create_user(uid="admin-contacts-2", email="admin-contacts-2@example.com")
    users_store.create_user(
        uid="admin-contacts-2", alias="admincontacts2", display_name="Admin", role="admin"
    )
    set_claims("admin-contacts-2", role="admin", fam="famB")
    other_family_admin_token = mint_id_token("admin-contacts-2")
    resp_other_family = _get(f"contactRequests/{doc_key}", other_family_admin_token)
    assert resp_other_family.status_code == 403

    fb_auth.create_user(uid="bare-admin-contacts-1", email="bare-admin-contacts-1@example.com")
    users_store.create_user(uid="bare-admin-contacts-1", alias="bareadmincr1", display_name="Bare")
    fb_auth.set_custom_user_claims("bare-admin-contacts-1", {"admin": True})
    bare_admin_token = mint_id_token("bare-admin-contacts-1")
    resp_bare_admin = _get(f"contactRequests/{doc_key}", bare_admin_token)
    assert resp_bare_admin.status_code == 403

    resp_unauth = _get(f"contactRequests/{doc_key}", None)
    assert resp_unauth.status_code == 403


def test_sms_log_readable_by_owner_and_family_admin_not_a_third_party(two_pairs):
    """`devices/{id}/smsLog/{logId}` (docs/V02_DESIGN.md §6/§7): owner and
    the device's family admin only (claims-only, docs/FAMILIES_TASKS.md
    1.4) -- unlike `devices/{id}/locations`, a `locate`-only `locatableBy`
    grant does NOT extend to reading the SMS audit log, and neither does
    another family's admin."""
    from app.store import sms as sms_store

    devices_store.create_device(
        device_id="pgr-rules-sms-1",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-sms-1",
        mqtt_password_hash="x",
    )
    _set_family("devices", "pgr-rules-sms-1", "famA")
    devices_store.set_locatable_by("pgr-rules-sms-1", ["u1"])
    sms_store.create_log(
        "pgr-rules-sms-1",
        "s_rules1",
        ts=1000,
        sms_ts=1000,
        dir_="out",
        peer="+15550001111",
        st="sent",
        body="hi",
    )

    owner_token = mint_id_token("u2")
    resp_owner = _get("devices/pgr-rules-sms-1/smsLog/s_rules1", owner_token)
    assert resp_owner.status_code == 200

    # A `locate`-permission uid is not the owner and is not an admin --
    # denied, even though it can read this same device's `locations`.
    locate_token = mint_id_token("u1")
    resp_locate = _get("devices/pgr-rules-sms-1/smsLog/s_rules1", locate_token)
    assert resp_locate.status_code == 403

    other_token = mint_id_token("u3")
    resp_other = _get("devices/pgr-rules-sms-1/smsLog/s_rules1", other_token)
    assert resp_other.status_code == 403

    resp_unauth = _get("devices/pgr-rules-sms-1/smsLog/s_rules1", None)
    assert resp_unauth.status_code == 403

    fb_auth.create_user(uid="admin-sms-1", email="admin-sms-1@example.com")
    users_store.create_user(uid="admin-sms-1", alias="adminsms1", display_name="Admin", role="admin")
    set_claims("admin-sms-1", role="admin", fam="famA")
    admin_token = mint_id_token("admin-sms-1")
    resp_admin = _get("devices/pgr-rules-sms-1/smsLog/s_rules1", admin_token)
    assert resp_admin.status_code == 200

    fb_auth.create_user(uid="admin-sms-2", email="admin-sms-2@example.com")
    users_store.create_user(uid="admin-sms-2", alias="adminsms2", display_name="Admin", role="admin")
    set_claims("admin-sms-2", role="admin", fam="famB")
    other_family_admin_token = mint_id_token("admin-sms-2")
    resp_other_family = _get(
        "devices/pgr-rules-sms-1/smsLog/s_rules1", other_family_admin_token
    )
    assert resp_other_family.status_code == 403

    resp_write = _write(
        "devices/pgr-rules-sms-1/smsLog/s_rules1", owner_token, {"body": "hacked"}
    )
    assert resp_write.status_code == 403


def test_sms_contacts_field_not_client_writable(two_pairs):
    """`devices/{id}.smsContacts` is a plain field on the already-owner/
    admin-readable `devices/{id}` document (docs/V02_DESIGN.md §6: "if the
    rules already give owners read access to their device document" -- they
    do, so no separate read rule is added) -- pinned here is only the write
    side, which the blanket `allow write: if false` denies to every client
    regardless of field, same as every other device field."""
    devices_store.create_device(
        device_id="pgr-rules-sms-2",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-sms-2",
        mqtt_password_hash="x",
    )
    owner_token = mint_id_token("u2")
    resp_write = _write(
        "devices/pgr-rules-sms-2",
        owner_token,
        {"smsContacts": "hacked"},
    )
    assert resp_write.status_code == 403


# ---------------------------------------------------------------------------
# Group chat (docs/GROUP_CHAT_DESIGN.md §2, §8.5, task G5). No rule change:
# a group conversation's `uids` is the member list and a group copy's
# `uids` is still the sender/recipient pair, so `messages/{id}` and
# `conversations/{k}`'s existing "party to this document"/"member of this
# conversation" rules already cover both -- these tests are what proves
# that rather than assuming it.
# ---------------------------------------------------------------------------


@pytest.fixture
def group_convo():
    """A 3-member group g1/g2/g3 (an outsider, g4, is unrelated to it) with
    one logical group message from g1 -- two copies, `messages/{}` addressed
    `g1->g2` and `g1->g3`, sharing one `groupMsgId`/`seq` and the group's
    `convKey`."""
    for uid, email, alias in (
        ("g1", "g1@example.com", "galice"),
        ("g2", "g2@example.com", "gbob"),
        ("g3", "g3@example.com", "gcarol"),
        ("g4", "g4@example.com", "goutsider"),
    ):
        fb_auth.create_user(uid=uid, email=email)
        users_store.create_user(uid=uid, alias=alias, display_name=alias)

    conv = conversations_store.create_group(
        name="Family", alias="rules-fam", member_uids=["g1", "g2", "g3"], created_by="g1"
    )
    seq = messages_store.allocate_seq()
    group_msg_id = "gm_rules1"
    copy_to_g2 = messages_store.create_message(
        sender_uid="g1",
        recipient_uid="g2",
        kind="text",
        ts=1000,
        body="hi",
        conv_key=conv.convKey,
        uids=sorted(["g1", "g2"]),
        seq=seq,
        group_msg_id=group_msg_id,
        sender_alias="galice",
    )
    copy_to_g3 = messages_store.create_message(
        sender_uid="g1",
        recipient_uid="g3",
        kind="text",
        ts=1000,
        body="hi",
        conv_key=conv.convKey,
        uids=sorted(["g1", "g3"]),
        seq=seq,
        group_msg_id=group_msg_id,
        sender_alias="galice",
    )
    return conv, copy_to_g2, copy_to_g3


def test_group_member_can_read_conversation_and_own_copy(group_convo):
    conv, copy_to_g2, _copy_to_g3 = group_convo
    token = mint_id_token("g2")
    resp_conv = _get(f"conversations/{conv.convKey}", token)
    assert resp_conv.status_code == 200
    resp_msg = _get(f"messages/{copy_to_g2.id}", token)
    assert resp_msg.status_code == 200


def test_group_non_member_cannot_read_conversation_or_any_copy(group_convo):
    conv, copy_to_g2, copy_to_g3 = group_convo
    token = mint_id_token("g4")
    assert _get(f"conversations/{conv.convKey}", token).status_code == 403
    assert _get(f"messages/{copy_to_g2.id}", token).status_code == 403
    assert _get(f"messages/{copy_to_g3.id}", token).status_code == 403


def test_group_member_cannot_read_a_copy_addressed_to_a_different_member(group_convo):
    """Per-copy visibility (docs/GROUP_CHAT_DESIGN.md §0, §2): a group copy's
    own `uids` field is the sender/recipient *pair*, not the full member
    list, so a member who isn't party to a particular copy can't read it --
    even though they can read the group's `conversations/{k}` summary and
    every copy addressed to *them*."""
    _conv, _copy_to_g2, copy_to_g3 = group_convo
    token = mint_id_token("g2")
    resp = _get(f"messages/{copy_to_g3.id}", token)
    assert resp.status_code == 403


def test_group_thread_convkey_only_query_is_denied(group_convo):
    """Same rule `test_thread_list_query_without_the_uids_filter_is_denied`
    pins for a DM thread, exercised again for a group's minted `convKey`."""
    conv, _copy_to_g2, _copy_to_g3 = group_convo
    body = {
        "structuredQuery": {
            "from": [{"collectionId": "messages"}],
            "where": {
                "fieldFilter": {
                    "field": {"fieldPath": "convKey"},
                    "op": "EQUAL",
                    "value": {"stringValue": conv.convKey},
                }
            },
        }
    }
    resp = _run_query("", mint_id_token("g1"), body)
    assert resp.status_code == 403, resp.text


def test_group_sender_can_list_query_both_of_their_own_copies(group_convo):
    """The sender's uid is in *every* copy's pair (docs/GROUP_CHAT_DESIGN.md
    §2: "the sender holds N-1 copies of their own message"), so the same
    `convKey` + `uids array-contains <self>` query the DM thread view uses
    returns both copies to the sender -- this is exactly what the web
    client's `groupMsgId` dedupe (§5) exists to collapse back into one
    bubble; the rules layer's job is only to prove both reads are allowed."""
    conv, _copy_to_g2, _copy_to_g3 = group_convo
    body = {
        "structuredQuery": {
            "from": [{"collectionId": "messages"}],
            "where": {
                "compositeFilter": {
                    "op": "AND",
                    "filters": [
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "convKey"},
                                "op": "EQUAL",
                                "value": {"stringValue": conv.convKey},
                            }
                        },
                        {
                            "fieldFilter": {
                                "field": {"fieldPath": "uids"},
                                "op": "ARRAY_CONTAINS",
                                "value": {"stringValue": "g1"},
                            }
                        },
                    ],
                }
            },
        }
    }
    resp = _run_query("", mint_id_token("g1"), body)
    assert resp.status_code == 200, resp.text
    docs = [entry for entry in resp.json() if "document" in entry]
    assert len(docs) == 2


def test_former_group_member_keeps_old_copies_but_loses_the_summary(group_convo):
    """docs/GROUP_CHAT_DESIGN.md §8.5: pin that a **former** member keeps
    read access to copies they were party to (intended -- per-copy
    visibility never depends on current membership) and loses the
    conversation summary (which freezes their unread badge -- cosmetic, per
    the same risk note)."""
    conv, copy_to_g2, _copy_to_g3 = group_convo
    conversations_store.remove_member(conv.convKey, "g2")
    token = mint_id_token("g2")

    resp_msg = _get(f"messages/{copy_to_g2.id}", token)
    assert resp_msg.status_code == 200

    resp_conv = _get(f"conversations/{conv.convKey}", token)
    assert resp_conv.status_code == 403


# ---------------------------------------------------------------------------
# Rules v2 -- claims-only `role()`/`fam()`, families, and family scoping
# (docs/FAMILIES_DESIGN.md §3's rules sketch, docs/FAMILIES_TASKS.md 1.4).
# The legacy `admin` custom claim is never consulted by any predicate below
# (or above -- `isAdmin()` is gone); a bare `admin: true` claim with no
# `role`/`fam` is pinned denied wherever a plain member would be denied.
# ---------------------------------------------------------------------------


def test_family_doc_readable_by_same_family_member_and_super_not_other_family(two_pairs):
    """`sameFam(f)` compares the *caller's* `fam` claim against the family
    id in the path -- `_set_family` (used elsewhere in this section) only
    stamps a document's own `familyId` field and is not enough on its own
    for a reader; the reader's `fam` claim is what `set_claims` sets."""
    _create_family("famA")
    _create_family("famB")

    set_claims("u2", fam="famA")
    same_family_token = mint_id_token("u2")
    assert _get("families/famA", same_family_token).status_code == 200

    set_claims("u3", fam="famB")
    other_family_token = mint_id_token("u3")
    assert _get("families/famA", other_family_token).status_code == 403

    fb_auth.create_user(uid="super-fam-1", email="super-fam-1@example.com")
    users_store.create_user(uid="super-fam-1", alias="superfam1", display_name="Super")
    set_claims("super-fam-1", role="super", fam="famA")
    super_token = mint_id_token("super-fam-1")
    assert _get("families/famA", super_token).status_code == 200
    assert _get("families/famB", super_token).status_code == 200


def test_family_alerts_readable_only_by_family_admin_or_super(two_pairs):
    _create_family("famA")
    _set_family("users", "u1", "famA")
    get_db().collection("families").document("famA").collection("alerts").document(
        "a1"
    ).set({"kind": "sms_unknown", "status": "open", "ts": 1})

    fb_auth.create_user(uid="admin-alerts-1", email="admin-alerts-1@example.com")
    users_store.create_user(uid="admin-alerts-1", alias="adminalerts1", display_name="Admin", role="admin")
    set_claims("admin-alerts-1", role="admin", fam="famA")
    admin_token = mint_id_token("admin-alerts-1")
    assert _get("families/famA/alerts/a1", admin_token).status_code == 200

    fb_auth.create_user(uid="admin-alerts-2", email="admin-alerts-2@example.com")
    users_store.create_user(uid="admin-alerts-2", alias="adminalerts2", display_name="Admin", role="admin")
    set_claims("admin-alerts-2", role="admin", fam="famB")
    other_family_admin_token = mint_id_token("admin-alerts-2")
    assert _get("families/famA/alerts/a1", other_family_admin_token).status_code == 403

    # A member of famA (not an admin) is denied too -- alerts are an admin
    # inbox, not a general family read.
    member_token = mint_id_token("u1")
    assert _get("families/famA/alerts/a1", member_token).status_code == 403

    fb_auth.create_user(uid="super-alerts-1", email="super-alerts-1@example.com")
    users_store.create_user(uid="super-alerts-1", alias="superalerts1", display_name="Super")
    set_claims("super-alerts-1", role="super", fam="famA")
    super_token = mint_id_token("super-alerts-1")
    assert _get("families/famA/alerts/a1", super_token).status_code == 200


def test_same_family_member_reads_users_doc_other_family_member_denied(two_pairs):
    _set_family("users", "u1", "famA")
    _set_family("users", "u2", "famA")
    _set_family("users", "u3", "famB")

    set_claims("u2", fam="famA")
    same_family_token = mint_id_token("u2")
    assert _get("users/u1", same_family_token).status_code == 200

    set_claims("u3", fam="famB")
    other_family_token = mint_id_token("u3")
    assert _get("users/u1", other_family_token).status_code == 403


def test_bare_legacy_admin_claim_grants_nothing(two_pairs):
    """A bare `admin: true` custom claim -- no `role`, no `fam` -- is denied
    everywhere a plain member would be denied: the legacy claim is never
    consulted by any predicate (docs/FAMILIES_DESIGN.md §1 decision 2,
    docs/FAMILIES_TASKS.md 1.4)."""
    _create_family("famA")
    _set_family("users", "u2", "famA")
    devices_store.create_device(
        device_id="pgr-rules-bare-1",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-bare-1",
        mqtt_password_hash="x",
    )
    _set_family("devices", "pgr-rules-bare-1", "famA")
    allow_store.set_edge("u1", "u2", message=True, locate=False)
    _set_allow_family_ids("u1", "u2", ["famA"])

    fb_auth.create_user(uid="bare-admin-1", email="bare-admin-1@example.com")
    users_store.create_user(uid="bare-admin-1", alias="bareadmin1", display_name="Bare")
    fb_auth.set_custom_user_claims("bare-admin-1", {"admin": True})
    token = mint_id_token("bare-admin-1")

    assert _get("families/famA", token).status_code == 403
    assert _get("users/u2", token).status_code == 403
    assert _get("devices/pgr-rules-bare-1", token).status_code == 403
    assert _get("allow/u1_u2", token).status_code == 403


def test_family_admin_reads_family_device_and_locations_other_family_admin_denied(two_pairs):
    devices_store.create_device(
        device_id="pgr-rules-famdev-1",
        owner_uid="u2",
        label="d",
        mqtt_username="pgr-rules-famdev-1",
        mqtt_password_hash="x",
    )
    _set_family("devices", "pgr-rules-famdev-1", "famA")
    from app.store.locations import LocationFix, add_location

    loc_id = add_location(
        "pgr-rules-famdev-1", LocationFix(ts=1, fixTs=1, lat=1.0, lon=2.0)
    )

    fb_auth.create_user(uid="admin-famdev-1", email="admin-famdev-1@example.com")
    users_store.create_user(uid="admin-famdev-1", alias="adminfamdev1", display_name="Admin", role="admin")
    set_claims("admin-famdev-1", role="admin", fam="famA")
    admin_token = mint_id_token("admin-famdev-1")
    assert _get("devices/pgr-rules-famdev-1", admin_token).status_code == 200
    assert (
        _get(f"devices/pgr-rules-famdev-1/locations/{loc_id}", admin_token).status_code
        == 200
    )

    fb_auth.create_user(uid="admin-famdev-2", email="admin-famdev-2@example.com")
    users_store.create_user(uid="admin-famdev-2", alias="adminfamdev2", display_name="Admin", role="admin")
    set_claims("admin-famdev-2", role="admin", fam="famB")
    other_family_admin_token = mint_id_token("admin-famdev-2")
    assert _get("devices/pgr-rules-famdev-1", other_family_admin_token).status_code == 403
    assert (
        _get(
            f"devices/pgr-rules-famdev-1/locations/{loc_id}", other_family_admin_token
        ).status_code
        == 403
    )


def test_allow_edge_readable_by_family_admin_of_either_party_not_a_third_family(two_pairs):
    allow_store.set_edge("u1", "u2", message=True, locate=False)
    _set_allow_family_ids("u1", "u2", ["famA", "famB"])

    fb_auth.create_user(uid="admin-allow-1", email="admin-allow-1@example.com")
    users_store.create_user(uid="admin-allow-1", alias="adminallow1", display_name="Admin", role="admin")
    set_claims("admin-allow-1", role="admin", fam="famA")
    admin_a_token = mint_id_token("admin-allow-1")
    assert _get("allow/u1_u2", admin_a_token).status_code == 200

    fb_auth.create_user(uid="admin-allow-2", email="admin-allow-2@example.com")
    users_store.create_user(uid="admin-allow-2", alias="adminallow2", display_name="Admin", role="admin")
    set_claims("admin-allow-2", role="admin", fam="famB")
    admin_b_token = mint_id_token("admin-allow-2")
    assert _get("allow/u1_u2", admin_b_token).status_code == 200

    fb_auth.create_user(uid="admin-allow-3", email="admin-allow-3@example.com")
    users_store.create_user(uid="admin-allow-3", alias="adminallow3", display_name="Admin", role="admin")
    set_claims("admin-allow-3", role="admin", fam="famC")
    admin_c_token = mint_id_token("admin-allow-3")
    assert _get("allow/u1_u2", admin_c_token).status_code == 403


# --- 2.2: admin read of conversations/messages via `familyIds` -----------
#
# docs/FAMILIES_TASKS.md 2.2 / docs/FAMILIES_DESIGN.md §3: conversation read
# = participants ∪ family admins of any participant ∪ super. `familyIds` is
# 2.1's field (concurrent); these tests stamp it directly via `_set_family_
# ids` rather than depending on 2.1's still-landing writers, same as 1.4's
# tests stamped `familyId` on `devices`/`users`/`contactRequests`.


def test_family_admin_reads_cross_family_dm_symmetric_third_family_and_member_denied(
    two_pairs,
):
    """u1<->u2's DM, `familyIds = [famA, famB]` (a cross-family DM):
    readable by a family admin of *either* family (§1 decision 4 is
    symmetric by construction -- `familyIds` is the union of both
    participants' families), denied to a third family's admin and to a
    plain (non-party) member."""
    key = messages_store.conv_key("u1", "u2")
    msg = two_pairs
    _set_family_ids("conversations", key, ["famA", "famB"])
    _set_family_ids("messages", msg.id, ["famA", "famB"])

    admin_a_token = _make_family_admin("admin-conv-a", "adminconva", "famA")
    assert _get(f"conversations/{key}", admin_a_token).status_code == 200
    assert _get(f"messages/{msg.id}", admin_a_token).status_code == 200

    admin_b_token = _make_family_admin("admin-conv-b", "adminconvb", "famB")
    assert _get(f"conversations/{key}", admin_b_token).status_code == 200
    assert _get(f"messages/{msg.id}", admin_b_token).status_code == 200

    admin_c_token = _make_family_admin("admin-conv-c", "adminconvc", "famC")
    assert _get(f"conversations/{key}", admin_c_token).status_code == 403
    assert _get(f"messages/{msg.id}", admin_c_token).status_code == 403

    # u3: a plain member (no `role` claim) of famA, not a party to the DM --
    # `role() == 'admin'` is false, so the family-admin clause never even
    # applies; denied same as before 2.2.
    set_claims("u3", fam="famA")
    member_token = mint_id_token("u3")
    assert _get(f"conversations/{key}", member_token).status_code == 403
    assert _get(f"messages/{msg.id}", member_token).status_code == 403


def test_sms_conversation_denied_to_other_family_admin(two_pairs):
    """An SMS conversation's `familyIds` carries only the member's own
    family (externals contribute none, §1 decision 4) -- readable by that
    family's own admin, denied to any other family's admin."""
    key = messages_store.conv_key("u1", "u2")
    msg = two_pairs
    _set_family_ids("conversations", key, ["famA"])
    _set_family_ids("messages", msg.id, ["famA"])

    same_family_admin_token = _make_family_admin(
        "admin-sms-conv-1", "adminsmsconv1", "famA"
    )
    assert _get(f"conversations/{key}", same_family_admin_token).status_code == 200
    assert _get(f"messages/{msg.id}", same_family_admin_token).status_code == 200

    other_family_admin_token = _make_family_admin(
        "admin-sms-conv-2", "adminsmsconv2", "famB"
    )
    assert _get(f"conversations/{key}", other_family_admin_token).status_code == 403
    assert _get(f"messages/{msg.id}", other_family_admin_token).status_code == 403


def test_family_admin_query_by_own_familyIds_allowed_other_family_denied(two_pairs):
    """The family monitor view's query must carry `familyIds
    array-contains <own fam>` for the abstract per-query precheck to prove
    itself, exactly like the existing `uids array-contains <self>` case
    (`test_party_can_list_query_their_thread` above) -- a query naming a
    *different* family cannot be proven from the admin's own claim and is
    denied outright, not merely empty."""
    key = messages_store.conv_key("u1", "u2")
    msg = two_pairs
    _set_family_ids("conversations", key, ["famA", "famB"])
    _set_family_ids("messages", msg.id, ["famA", "famB"])

    admin_token = _make_family_admin("admin-conv-query-1", "adminconvquery1", "famA")

    resp = _run_query("", admin_token, _family_ids_query("conversations", "famA"))
    assert resp.status_code == 200, resp.text
    assert any("document" in entry for entry in resp.json()), resp.text

    resp_msgs = _run_query("", admin_token, _family_ids_query("messages", "famA"))
    assert resp_msgs.status_code == 200, resp_msgs.text
    assert any("document" in entry for entry in resp_msgs.json()), resp_msgs.text

    denied_conv = _run_query("", admin_token, _family_ids_query("conversations", "famB"))
    assert denied_conv.status_code == 403, denied_conv.text

    denied_msgs = _run_query("", admin_token, _family_ids_query("messages", "famB"))
    assert denied_msgs.status_code == 403, denied_msgs.text


def test_book_nick_readable_by_owner_and_family_admin_only_writes_denied(two_pairs):
    """docs/ADDRESS_BOOK_DESIGN.md decision 8: `users/{o}/book/{p}` is read by
    the owner, a family admin of the stamped `familyId`, or super -- not by
    another member of the same family, not by another family's admin; no
    client may write."""
    _set_family("users", "u1", "famA")
    _set_family("users", "u2", "famA")
    get_db().collection("users").document("u1").collection("book").document("u2").set(
        {"nick": "Bro", "familyId": "famA", "updatedBy": "u1"}
    )
    path = "users/u1/book/u2"

    set_claims("u1", fam="famA")
    owner_token = mint_id_token("u1")
    assert _get(path, owner_token).status_code == 200

    same_family_admin = _make_family_admin("admin-book-a", "adminbooka", "famA")
    assert _get(path, same_family_admin).status_code == 200

    other_family_admin = _make_family_admin("admin-book-b", "adminbookb", "famB")
    assert _get(path, other_family_admin).status_code == 403

    set_claims("u2", fam="famA")
    sibling_token = mint_id_token("u2")
    assert _get(path, sibling_token).status_code == 403

    for token in (owner_token, same_family_admin, sibling_token):
        assert _write(path, token, {"nick": "Hacked"}).status_code == 403
        assert _write("users/u1/book/u3", token, {"nick": "New"}).status_code == 403


def test_book_added_marker_is_not_client_writable(two_pairs):
    """docs/BOOK_ADD_ANYONE_DESIGN.md D4: the `added` marker is relay-only,
    even for its owner and a family admin; the read rule is unchanged."""
    _set_family("users", "u1", "famA")
    _set_family("users", "u2", "famA")
    get_db().collection("users").document("u1").collection("book").document("u2").set(
        {"added": True, "familyId": "famA", "addedBy": "pgr-1"}
    )
    path = "users/u1/book/u2"

    set_claims("u1", fam="famA")
    owner_token = mint_id_token("u1")
    assert _get(path, owner_token).status_code == 200
    admin_token = _make_family_admin("admin-book-c", "adminbookc", "famA")
    assert _get(path, admin_token).status_code == 200

    for token in (owner_token, admin_token):
        assert _write(path, token, {"added": False}).status_code == 403
        assert _write("users/u1/book/u3", token, {"added": True, "familyId": "famA"}).status_code == 403
