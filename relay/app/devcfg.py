"""`book` and `cfg` `/down` envelopes -- docs/DEVICE_PLAN.md §4.3 (book),
§5.8 (cfg), docs/PROTOCOL.md §3.1/§3.2's `kind:"book"`/`kind:"cfg"` rows,
docs/DEVICE_TASKS.md S4.2.

Both kinds share a shape this module treats uniformly:

- **Not a thread entry.** Neither lives in `messages/{id}` (that collection
  is for `(senderUid, recipientUid)` conversation entries, `app/store/
  messages.py`'s own docstring); a book/cfg addresses a *device*, has no
  sender/recipient pair, and is never rendered in any thread.
- **Acked `shown` once applied, like any down message** (§3.2), but through
  a state machine of exactly two positions -- "unacked" and "acked" -- not
  the full `queued -> sent -> shown -> read` machine `app/store/messages.py`
  models: there is no `read` for a book/cfg (it is never displayed as a
  message to press a button on), and "queued"/"sent" collapse into a single
  "unacked, still worth re-publishing" state since the only failure mode
  that matters here is "the device never applied it yet".
- **Newest only.** Creating a new book (or cfg) forgets the previous one:
  its `id` is no longer tracked anywhere, so it can never be re-published on
  a later online edge and a late-arriving ack for it is indistinguishable
  from an ack for an unknown id (`ack()` below returns `False`) -- exactly
  docs/PROTOCOL.md §3.2's "the relay expires any older unacked `book`/`cfg`
  when it creates a new one", achieved by simply not keeping the old one
  around rather than by writing an explicit `expired` state anywhere.
- **Included in the online-edge re-publish, one each, newest only**
  (docs/PROTOCOL.md §5.3) -- `republish_pending()` below, called by
  `app/ingest.py`'s `_republish_unacked` alongside its existing
  `pendingDeviceIds` loop.

**Where the pending state lives, and why not a new store module:** this
task's `docs/DEVICE_TASKS.md` `Files` list is `app/devcfg.py`,
`app/routers/admin.py`, `app/ingest.py` plus tests -- no new `app/store/*`
module, and `app/store/devices.py` (which would be the natural owner of
"this device's current book/cfg state") is not in that list either. So this
module writes two raw sub-fields directly onto `devices/{deviceId}` via
`app.db.firestore.get_db()` --

    devices/{d}.pendingBook = {"id": "m_...", "obj": {...the full down
                                envelope, unsigned...}, "acked": bool} | None
    devices/{d}.pendingCfg  = same shape, for the most recent `cfg`

-- the same "write/read a field the `Device` pydantic model doesn't declare
yet, via `get_db()` directly" pattern S4.1's `app/store/contacts.py` already
uses for `bookVersion` (see that module's docstring) and S2.1's
`record_sig_failure` uses for `authFailureTimes`; `Device.model_validate`'s
`extra="ignore"` drops both fields silently on every `devices_store.
get_device()` call, which is fine since nothing outside this module ever
needs to read them through that model. `obj` stores the exact unsigned
envelope this module built and handed to `BrokerClient.publish_down` --
*not* rebuilt from scratch on republish -- so a re-publish is guaranteed
byte-identical to the original push (same `id`, same content), which is
what makes device-side dedup (§4.1 rule 7) suppress a duplicate render
instead of showing two different books under the same id.

**Wiring `app/store/contacts.py`'s S4.1 placeholder:** that module's
`push_book(device_id)` is a documented no-op stub, and `app/store/
contacts.py` is *not* in this task's `Files` list, so it is left exactly as
S4.1 wrote it (dead code from here on). `app/routers/admin.py` *is* in this
task's `Files` list, and it is the only caller of `contacts_store.
push_book` (`approve_contact`/`reject_contact`) -- both call sites are
repointed to `devcfg.push_book` (this module's real implementation) instead,
which is the minimal way to make the real implementation take over without
touching the out-of-scope module at all.

**The `d` (default recipient alias) field, a doc gap flagged rather than
silently resolved:** docs/PROTOCOL.md §3.1's field table marks `d` as
required whenever `kind:"book"`, but `devices/{d}.defaultToUid` is optional
(`POST /api/admin/devices`'s `defaultToAlias` is not required), and when it
is unset there is no single alias to report -- the device broadcasts to
every user its owner may message instead (docs/PROTOCOL.md §4.2 rule 1),
which `d` (one alias) cannot represent. Absent an explicit rule for this
case, `build_book` omits `d` entirely when `defaultToUid` is unset, the same
"omit a field that has nothing to name" precedent §3.1 already sets for
`to` ("absent when it has no recipient to name"). Flagged in this task's
report rather than guessed past silently.
"""

from __future__ import annotations

import json
import logging
import time
from datetime import UTC, datetime
from typing import Any

from app import ca_resolve
from app.broker import BrokerClient
from app.config import Settings
from app.db.firestore import get_db
from app.ids import new_message_id
from app.store import allow as allow_store
from app.store import backends as backends_store
from app.store import contacts as contacts_store
from app.store import conversations as conversations_store
from app.store import devices as devices_store
from app.store import users as users_store
from app.wire import MAX_ENVELOPE_BYTES
from app.wirecbor import encode as cbor_encode
from app.wirecbor import to_json_safe

logger = logging.getLogger("relay.devcfg")

# docs/DEVICE_PLAN.md §4.3 / H6: "Book cap 10 approved + 4 requests." --
# still the cap for the legacy full `/down book` envelope (`build_book`,
# non-bpull devices, docs/PROTOCOL.md §3.7's gate).
MAX_APPROVED_CONTACTS = 10
MAX_LISTED_REQUESTS = 4
# docs/PROTOCOL.md §3.7: the §14.7 fetch response's own `c[]` cap -- "32
# instead of 10 ... the device's stored capacity", `docs/CHAT_UI_DESIGN.md`
# §1's `BOOK_MAX_CONTACTS`.
MAX_PULL_CONTACTS = 32

# docs/PROTOCOL.md §3.1: c[].n / p[].n are display names, same 16-code-point
# cap contact_req's own `name` field uses (app/ingest.py's
# `_CONTACT_NAME_MAX_CODEPOINTS`) -- truncated here rather than rejected,
# since a `users/{uid}.displayName` or a stored contact_req `name` may
# already exceed it (neither store enforces the 16-cp cap the wire does; the
# wire is enforced at ingest for `contact_req.name`, not at admin/user
# creation time for `displayName`).
_BOOK_NAME_MAX_CODEPOINTS = 16

# docs/PROTOCOL.md §14.3: a CBOR `sig` pair is always exactly 10 bytes on
# the wire (`0x0D 0x48` + an 8-byte tag) -- see app/devauth.py's
# `_CBOR_SIG_LEN` for the same constant, not imported from there (a private
# name) to avoid coupling this module to devauth's internals for one flat
# integer.
_SIGNED_CBOR_SIG_BYTES = 10
# Worst-case `n` -- docs/V02_DESIGN.md §3 widens `n` to a 52-bit counter
# (`app/wire.py`'s `N_MAX_EXCLUSIVE`); included in the size check even
# though the real value is only known at publish time (`app/store/
# device_secrets.py`'s `next_down_n`), so the assertion below never
# under-counts. `next_down_n` itself still counts by one from zero (§3: "the
# relay's own downN counts by one"), so this worst case is far more
# conservative than any downN reachable in practice -- which is the point of
# a worst-case bound.
_WORST_CASE_N = 2**53 - 1

_PENDING_BOOK_FIELD = "pendingBook"
_PENDING_CFG_FIELD = "pendingCfg"
# docs/V02_DESIGN.md §4.4: `cfg.ca` gets its own pending slot, separate from
# `cfg.lock`'s `_PENDING_CFG_FIELD` -- a pending lock and a pending CA push
# must not clobber each other (each is independently "newest unacked, only
# one at a time", but the two *kinds* of cfg coexist). Keeping the existing
# `pendingCfg` name for `lock` (rather than renaming both to a uniform
# scheme) leaves any `cfg.lock` already pending in production, before this
# change deploys, still tracked and re-publishable.
_PENDING_CFG_CA_FIELD = "pendingCfgCa"
# docs/V02_DESIGN.md §6: `cfg.sms` gets its own pending slot too, for the
# same reason `cfg.ca` did -- a pending SMS-contact push must not clobber
# (or be clobbered by) a pending `cfg.lock`/`cfg.ca`, since all three are
# independently "newest unacked, one at a time" per *kind* of cfg.
_PENDING_CFG_SMS_FIELD = "pendingCfgSms"
# docs/WIFI_DESIGN.md §4/§6, docs/WIFI_TASKS.md W7: `cfg.wifi` gets its own
# pending slot for the same reason -- independent of `cfg.lock`/`cfg.ca`/
# `cfg.sms`.
_PENDING_CFG_WIFI_FIELD = "pendingCfgWifi"
# Every field `ack()`/`republish_pending()` iterate over -- see their
# docstrings for why book/lock/ca/sms/wifi are five independent "newest
# unacked" slots rather than one.
_ALL_PENDING_FIELDS = (
    _PENDING_BOOK_FIELD,
    _PENDING_CFG_FIELD,
    _PENDING_CFG_CA_FIELD,
    _PENDING_CFG_SMS_FIELD,
    _PENDING_CFG_WIFI_FIELD,
)


def _devices():
    return get_db().collection("devices")


def get_book_version(device_id: str) -> int:
    """Raw read of `devices/{d}.bookVersion` -- see this module's docstring
    for why this bypasses `devices_store.Device` (S4.1's `app/store/
    contacts.py.bump_book_version` is the writer; not modelled there
    either, for the same reason)."""
    snap = _devices().document(device_id).get()
    if not snap.exists:
        return 0
    return int((snap.to_dict() or {}).get("bookVersion", 0) or 0)


def _default_alias(device: devices_store.Device) -> str | None:
    if device.defaultToUid is None:
        return None
    user = users_store.get_user(device.defaultToUid)
    return user.alias if user is not None else None


def _contact_type_hint(uid: str) -> str:
    """docs/PROTOCOL.md §3.1: `c[].t` is "hint for an icon (`web`/`sms`/
    `chat`)". A contact is a `users/{uid}` with zero or more backends
    (`app/store/backends.py`); every user also has an implicit `webapp`
    backend (`users_store.create_user`'s own docstring), so `web` is the
    fallback. `sms`/`gchat` take priority over the implicit `webapp` one
    when present and enabled -- those are the backends a contact was
    actually *added* through (docs/DEVICE_PLAN.md §4.3's "create" flow adds
    an `sms` backend for a phone-based contact), so they are the more
    useful icon to show than "web", which every contact technically has."""
    kinds = {b.kind for b in backends_store.list_backends(uid) if b.enabled}
    if "sms" in kinds:
        return "sms"
    if "gchat" in kinds:
        return "chat"
    return "web"


def _group_contacts(owner_uid: str) -> list[dict[str, Any]]:
    """docs/GROUP_CHAT_DESIGN.md §4: every group `owner_uid` is a member of,
    as a book contact -- `t: "grp"` (amended 23 Sep on firmware review: the
    pick screen labels a row from `book_contact_t.type` verbatim, so a group
    needs its own type distinct from `web`, docs/PROTOCOL.md §3.1's `c[].t`
    row). A group with no `alias` yet (should not happen --
    `conversations_store.create_group` always sets one in the same
    transaction as the conversation doc -- but `alias` is optional on the
    `Conversation` model because a DM conversation never has one) is
    skipped rather than emitting an unaddressable contact."""
    return [
        {"a": conv.alias, "n": (conv.name or conv.alias)[:_BOOK_NAME_MAX_CODEPOINTS], "t": "grp"}
        for conv in conversations_store.list_groups_for_member(owner_uid)
        if conv.alias is not None
    ]


def _approved_contacts(owner_uid: str) -> list[dict[str, Any]]:
    """Every contact this owner's device book may list -- allowed users
    (§3.2's `web`/`sms`/`chat` hint) then the owner's own groups (`t:"grp"`)
    -- **uncapped and unordered** (§3.7's `c[]` order is `_ordered_contacts`'
    job below, since `build_book` and `build_book_body` cap and order this
    same set differently)."""
    contacts: list[dict[str, Any]] = []
    for uid in allow_store.allowed_recipients(owner_uid):
        user = users_store.get_user(uid)
        if user is None:
            continue
        contacts.append(
            {
                "a": user.alias,
                "n": user.displayName[:_BOOK_NAME_MAX_CODEPOINTS],
                "t": _contact_type_hint(uid),
            }
        )
    contacts.extend(_group_contacts(owner_uid))
    return contacts


def _contact_sort_key(contact: dict[str, Any], default_alias: str | None) -> tuple[int, str, str]:
    """docs/PROTOCOL.md §3.7: "`d`, `c[]` and `p[]` are §3.2's, with `c[]`
    ... ordered: the entry whose alias equals `d` first, then `t:"grp"`
    entries, then the rest, each by display name (case-insensitive), ties
    by alias." `docs/CHAT_UI_DESIGN.md` §1: "Order: `d` first, then groups,
    then people, both alphabetical by display name" -- read together as one
    three-way rank (default, then group, then everyone else), each rank
    internally sorted by `displayName.casefold()` with the alias as the
    tiebreaker (the default-recipient rank never has a tie: `d` names at
    most one alias)."""
    if contact["a"] == default_alias:
        rank = 0
    elif contact["t"] == "grp":
        rank = 1
    else:
        rank = 2
    return (rank, contact["n"].casefold(), contact["a"])


def _ordered_contacts(owner_uid: str, default_alias: str | None) -> list[dict[str, Any]]:
    """`_approved_contacts(owner_uid)`, sorted into §3.7's `c[]` order.
    Shared, uncapped, by `build_book` (legacy full envelope, cap 10) and
    `build_book_body` (§14.7 fetch response, cap 32) -- both this task's Do
    2's "using the same new ordering before its cap"."""
    contacts = _approved_contacts(owner_uid)
    contacts.sort(key=lambda c: _contact_sort_key(c, default_alias))
    return contacts


def _listed_requests(device_id: str) -> list[dict[str, Any]]:
    """docs/PROTOCOL.md §3.2: `p[]` is "the device's own requests that are
    not approved" -- pending or rejected, `s` = `pend`/`no`. A device can
    accumulate more than `MAX_LISTED_REQUESTS` non-approved requests over
    its lifetime (rejections are unbounded, unlike the 5-pending cap), so
    this selects the *newest* `MAX_LISTED_REQUESTS` by `createdAt` (falling
    back to `reqId` for two requests created in the same emulator tick,
    where `SERVER_TIMESTAMP` resolution can tie) -- the most recently
    decided/asked-about requests are the ones worth a kid seeing on the
    device."""
    requests = [r for r in contacts_store.list_requests(device_id=device_id) if r.status != "approved"]
    requests.sort(key=lambda r: (r.createdAt or datetime.min.replace(tzinfo=UTC), r.reqId), reverse=True)
    return [
        {"n": r.name[:_BOOK_NAME_MAX_CODEPOINTS], "s": "pend" if r.status == "pending" else "no"}
        for r in requests[:MAX_LISTED_REQUESTS]
    ]


# docs/V02_DESIGN.md §6: `sig` is 11 base64url characters at runtime (8 raw
# bytes, `app/devauth.py`'s 64-bit truncated tag) -- `,"sig":"<11 chars>"`
# is the exact tail `sign_json` appends (see that function's docstring).
_SIGNED_JSON_SIG_SUFFIX_BYTES = len(',"sig":""') + 11


def _assert_within_envelope_limit(obj: dict[str, Any]) -> None:
    """docs/DEVICE_TASKS.md S4.2: "assert the signed CBOR is <= 640 bytes,"
    extended (docs/V02_DESIGN.md §6: "check ... in both encodings") to also
    assert the signed *JSON* size -- `cfg.sms`'s size depends on the SMS
    contact names' actual UTF-8 byte length, unlike `book`/`cfg.lock`/
    `cfg.ca`, whose worst case was always comfortably under 640 bytes in
    either encoding, so this second check was never load-bearing before now.
    `obj` is unsigned (signing is `BrokerClient.publish_down`'s job, which
    needs the device's `deviceSecrets.hmacKey` this module never touches),
    so this estimates each signed size: `n` (worst case, the full 52-bit
    counter) plus the fixed-length `sig` pair each encoding actually
    produces, both accounted for without needing real key material."""
    worst_case = {**obj, "n": _WORST_CASE_N}
    cbor_len = len(cbor_encode(worst_case)) + _SIGNED_CBOR_SIG_BYTES
    assert cbor_len <= MAX_ENVELOPE_BYTES, (
        f"devcfg {obj.get('kind')!r} payload too large: {cbor_len} bytes "
        f"signed CBOR (limit {MAX_ENVELOPE_BYTES})"
    )
    # `to_json_safe` mirrors `devauth.sign_json`'s own pre-serialisation
    # step: a raw `bytes` leaf (e.g. `cfg.ca.sha`) is not JSON-serialisable
    # at all, and the real JSON wire form is base64url text, not raw bytes
    # (docs/V02_DESIGN.md §7).
    json_len = (
        len(
            json.dumps(
                to_json_safe(worst_case), separators=(",", ":"), ensure_ascii=False
            ).encode("utf-8")
        )
        + _SIGNED_JSON_SIG_SUFFIX_BYTES
    )
    assert json_len <= MAX_ENVELOPE_BYTES, (
        f"devcfg {obj.get('kind')!r} payload too large: {json_len} bytes "
        f"signed JSON (limit {MAX_ENVELOPE_BYTES})"
    )


def build_book(device_id: str) -> dict[str, Any]:
    """docs/DEVICE_TASKS.md S4.2, ordering amended by docs/PROTOCOL.md §3.7
    (this task): the legacy full `/down book` envelope, still capped at
    `MAX_APPROVED_CONTACTS` (10) and still 640-byte-asserted -- unchanged in
    shape from before this task, only in `c[]`'s ordering (now the same
    `_ordered_contacts` rule the §14.7 fetch response uses, capped
    differently). Sent to a device that is not gated into the nudge
    (`push_book`'s job to decide which). Raises `ValueError` if `device_id`
    names no `devices/{d}` document -- a caller-error case (every real
    caller in this module checks the device exists first; see
    `push_book`)."""
    device = devices_store.get_device(device_id)
    if device is None:
        raise ValueError(f"no such device: {device_id!r}")

    obj: dict[str, Any] = {
        "v": 1,
        "id": new_message_id(),
        "ts": int(time.time()),
        "kind": "book",
        "bv": get_book_version(device_id),
    }
    default_alias = _default_alias(device)
    if default_alias is not None:
        obj["d"] = default_alias
    obj["c"] = _ordered_contacts(device.ownerUid, default_alias)[:MAX_APPROVED_CONTACTS]
    obj["p"] = _listed_requests(device_id)
    obj["ack"] = None
    _assert_within_envelope_limit(obj)
    return obj


def build_book_body(device_id: str) -> dict[str, Any]:
    """docs/PROTOCOL.md §3.7's §14.7 fetch response body, *unsigned* and
    without the two fields that only make sense on a down envelope (`id`,
    `ack` -- "There is no `id` and no `ack`: the response is not a down
    message and is never acked itself"). No 640-byte assert: this is an
    HTTPS body, not an MQTT payload, bounded instead by §3.7's 4096 bytes
    (the device refuses an oversize body itself; the ordering/32-cap below
    is what keeps a real book far under that in practice).

    `c[]` is capped at `MAX_PULL_CONTACTS` (32); `more: True` is set iff the
    real (uncapped) contact count exceeds the cap, never merely present as
    `False`, matching the wire's "reserved for chunking" optional-bool
    convention `build_book` already omits when there is nothing to say.

    Raises `ValueError` if `device_id` names no `devices/{d}` document, same
    as `build_book`."""
    device = devices_store.get_device(device_id)
    if device is None:
        raise ValueError(f"no such device: {device_id!r}")

    default_alias = _default_alias(device)
    contacts = _ordered_contacts(device.ownerUid, default_alias)
    truncated = len(contacts) > MAX_PULL_CONTACTS

    obj: dict[str, Any] = {
        "v": 1,
        "ts": int(time.time()),
        "kind": "book",
        "bv": get_book_version(device_id),
    }
    if default_alias is not None:
        obj["d"] = default_alias
    obj["c"] = contacts[:MAX_PULL_CONTACTS]
    obj["p"] = _listed_requests(device_id)
    if truncated:
        obj["more"] = True
    return obj


def build_nudge(device_id: str, url: str) -> dict[str, Any]:
    """docs/PROTOCOL.md §3.7: the `/down book` nudge -- `bv` and `url`, no
    `d`/`c`/`p`/`more`. `url` is the caller's job to build (`push_book`
    reads `settings.public_base_url`, this function does not touch
    `Settings` at all, matching `build_book`/`build_book_body`'s "this
    module's builders don't read the environment" shape). 640-byte-asserted
    like every other `/down` envelope this module builds -- the nudge is
    the one shape docs/PROTOCOL.md §3.3 gives its own worked example for
    (338 bytes at every field's maximum), so this assert is never expected
    to fire, but it is cheap insurance against `url` growing past its
    documented 200-byte cap in some future caller."""
    obj: dict[str, Any] = {
        "v": 1,
        "id": new_message_id(),
        "ts": int(time.time()),
        "kind": "book",
        "bv": get_book_version(device_id),
        "url": url,
        "ack": None,
    }
    _assert_within_envelope_limit(obj)
    return obj


def _set_pending(device_id: str, field: str, obj: dict[str, Any]) -> None:
    _devices().document(device_id).set(
        {field: {"id": obj["id"], "obj": obj, "acked": False}}, merge=True
    )


def push_book(
    device_id: str, broker: BrokerClient, *, settings: Settings | None = None
) -> bool:
    """docs/PROTOCOL.md §3.7's gate, docs/DEVICE_TASKS.md S4.2's original
    "build a fresh book, remember it as this device's one pending book
    (superseding -- not appending to -- whatever was pending before), and
    publish it."

    **The gate:** a nudge (`build_nudge`) is sent iff the device's stored
    `status.bpull == 1` *and* `authMode == "hmac"` *and*
    `settings.public_base_url` is configured (the nudge's `url` is built
    from it, `<base>/api/device/book` -- the same `.rstrip('/')` join
    `app/ca_resolve.py`'s `ca_pointer` uses for the sibling `/ca/...`
    pointer). If `bpull == 1` but no base URL is configured, this logs one
    ERROR (not raised -- a misconfigured deployment must not stop paging)
    and falls back to the legacy full book: **a nudge is never published
    without a `url`**, since a pull-capable device that received one would
    have nowhere to fetch from. Every other device (bpull absent, or
    `authMode != "hmac"`) gets the legacy full book unconditionally, per
    §3.7's own compatibility rule.

    Either way, the built envelope is stored in the existing `pendingBook`
    slot (newest only) and published exactly as before -- the ack path
    (`ack()`) and republish (`republish_pending()`) do not need to know
    which shape is pending; the device is expected to `shown`-ack a nudge
    at once if its `bv` is already stale (§3.7 device rule 1) or once the
    fetch lands (rule 2), and a full book once applied, same as today.

    Returns `False` without publishing if `device_id` is not registered
    (mirrors `BrokerClient.publish_down`'s own fail-closed style rather than
    raising, since every caller here is a webhook/admin-API handler that
    must not 500 on a device that has since been deleted)."""
    device = devices_store.get_device(device_id)
    if device is None:
        logger.warning("push_book: no such device %s", device_id)
        return False

    obj: dict[str, Any] | None = None
    if device.status.bpull == 1 and device.authMode == "hmac":
        settings = settings if settings is not None else Settings.from_env()
        base = settings.public_base_url
        if base:
            obj = build_nudge(device_id, f"{base.rstrip('/')}/api/device/book")
        else:
            logger.error(
                "push_book: device %s advertises bpull=1 but "
                "settings.public_base_url is not configured -- falling "
                "back to the legacy full book (never publishing a nudge "
                "without a url)",
                device_id,
            )
    if obj is None:
        obj = build_book(device_id)

    _set_pending(device_id, _PENDING_BOOK_FIELD, obj)
    return broker.publish_down(device_id, obj)


def renudge_if_behind(
    device_id: str, reported_bv: int, broker: BrokerClient, *, settings: Settings | None = None
) -> bool:
    """docs/PROTOCOL.md §5.3 (v0.4): "if the reported `bv` is lower than
    `bookVersion`, re-publish the pending nudge when its `bv` equals
    `bookVersion` (same `id`), otherwise build and publish a new one."

    Called by `app.ingest.Ingest.handle_status` once it already knows
    `reported_bv < devcfg.get_book_version(device_id)` -- this function does
    not re-check that itself (`reported_bv` is accepted, not re-derived,
    purely so the caller's log line and this function's decision are
    against the same value), only decides *which* republish. The
    "re-publish the pending nudge" branch fires only when there **is** a
    pending, unacked `pendingBook` whose own `bv` already equals the
    current `bookVersion` and whose `obj` carries `url` (a nudge, not a
    legacy full book, which never needs this same-id shortcut since it *is*
    the content) -- otherwise (no pending book, an already-acked one, a
    stale-`bv` one, or a legacy full book, e.g. a device whose gate flipped
    since the last push) this falls through to an ordinary `push_book`,
    which builds fresh and supersedes whatever was pending."""
    current_bv = get_book_version(device_id)
    snap = _devices().document(device_id).get()
    data = (snap.to_dict() or {}) if snap.exists else {}
    pending = data.get(_PENDING_BOOK_FIELD)
    if isinstance(pending, dict) and not pending.get("acked", False):
        obj = pending.get("obj")
        if isinstance(obj, dict) and "url" in obj and obj.get("bv") == current_bv:
            logger.info(
                "re-publishing pending unacked book nudge %s to device %s "
                "(reported bv=%s behind bv=%s, pending nudge already matches)",
                obj.get("id"),
                device_id,
                reported_bv,
                current_bv,
            )
            return broker.publish_down(device_id, obj)
    return push_book(device_id, broker, settings=settings)


def push_cfg(device_id: str, lock: dict[str, Any], broker: BrokerClient) -> bool:
    """docs/DEVICE_TASKS.md S4.2 / docs/DEVICE_PLAN.md §5.8: `lock` is the
    `{clear?: bool, auto?: int}` map `POST /api/admin/devices/{id}/cfg`
    accepts verbatim (docs/PROTOCOL.md §3.2: "unknown members of `cfg` are
    ignored", so this module does not need to know every possible `lock`
    key -- it passes whatever the caller built)."""
    if devices_store.get_device(device_id) is None:
        logger.warning("push_cfg: no such device %s", device_id)
        return False
    obj: dict[str, Any] = {
        "v": 1,
        "id": new_message_id(),
        "ts": int(time.time()),
        "kind": "cfg",
        "cfg": {"lock": lock},
        "ack": None,
    }
    _assert_within_envelope_limit(obj)
    _set_pending(device_id, _PENDING_CFG_FIELD, obj)
    return broker.publish_down(device_id, obj)


def push_ca(
    device_id: str, *, pem: str, broker: BrokerClient, settings: Settings | None = None
) -> bool:
    """docs/V02_DESIGN.md §4.4: `/down cfg.ca = {url, sha}` -- the CA-pointer
    push. `pem` is the CA PEM text to push (normally the relay's own current
    CA, `BROKER_CA_PEM`/`ca_resolve.get_broker_ca_pem()`, but this module
    does not read the environment itself, matching `push_cfg`'s "the caller
    builds the payload" shape); `ca_resolve.ca_pointer` computes the
    content-addressed URL, remembers the PEM in `cas/{sha}` so the pointer
    keeps resolving later, and raises `ca_resolve.PublicBaseUrlRequired` if
    `PUBLIC_BASE_URL` is not configured -- propagated to the caller (the
    admin route) rather than swallowed, since silently not pushing would
    look identical to a broker outage.

    Stored under its own pending slot (`_PENDING_CFG_CA_FIELD`), independent
    of a pending `cfg.lock` -- see `_ALL_PENDING_FIELDS`'s docstring."""
    if devices_store.get_device(device_id) is None:
        logger.warning("push_ca: no such device %s", device_id)
        return False
    settings = settings if settings is not None else Settings.from_env()
    url, sha = ca_resolve.ca_pointer(pem, settings)
    obj: dict[str, Any] = {
        "v": 1,
        "id": new_message_id(),
        "ts": int(time.time()),
        "kind": "cfg",
        "cfg": {"ca": {"url": url, "sha": sha}},
        "ack": None,
    }
    _assert_within_envelope_limit(obj)
    _set_pending(device_id, _PENDING_CFG_CA_FIELD, obj)
    return broker.publish_down(device_id, obj)


def unpin_ca(device_id: str, broker: BrokerClient) -> bool:
    """docs/V02_DESIGN.md §4.4: "`url = ''` means un-pin." No `sha` at all --
    there is nothing to hash-check when there is no CA."""
    if devices_store.get_device(device_id) is None:
        logger.warning("unpin_ca: no such device %s", device_id)
        return False
    obj: dict[str, Any] = {
        "v": 1,
        "id": new_message_id(),
        "ts": int(time.time()),
        "kind": "cfg",
        "cfg": {"ca": {"url": ""}},
        "ack": None,
    }
    _assert_within_envelope_limit(obj)
    _set_pending(device_id, _PENDING_CFG_CA_FIELD, obj)
    return broker.publish_down(device_id, obj)


def push_sms_contacts(
    device_id: str, contacts: list[dict[str, Any]], broker: BrokerClient
) -> bool:
    """docs/V02_DESIGN.md §6: `/down cfg.sms = [{n, p}, ...]` -- the *whole*
    SMS contact allow-list, every time (`[]` is a legal push, meaning "no
    SMS contacts"), newest-wins and acked `shown` on apply exactly like
    `cfg.lock`/`cfg.ca`. `contacts` is `[{"name": ..., "phone": ...}, ...]`
    (`app/store/devices.py`'s `SmsContact.model_dump()` shape) -- the caller
    (`app/routers/devices.py`) has already validated max-8/unique-phone/
    E.164/name-length before calling this, so this function does not
    re-validate, only maps `name`/`phone` to the wire's `n`/`p` (§7's
    `cfg.sms[]` sub-map) and stores/publishes.

    Stored under its own pending slot (`_PENDING_CFG_SMS_FIELD`), independent
    of a pending `cfg.lock`/`cfg.ca` -- see `_ALL_PENDING_FIELDS`'s
    docstring."""
    if devices_store.get_device(device_id) is None:
        logger.warning("push_sms_contacts: no such device %s", device_id)
        return False
    obj: dict[str, Any] = {
        "v": 1,
        "id": new_message_id(),
        "ts": int(time.time()),
        "kind": "cfg",
        "cfg": {"sms": [{"n": c["name"], "p": c["phone"]} for c in contacts]},
        "ack": None,
    }
    _assert_within_envelope_limit(obj)
    _set_pending(device_id, _PENDING_CFG_SMS_FIELD, obj)
    return broker.publish_down(device_id, obj)


def push_wifi(
    device_id: str, *, en: bool, nets: list[dict[str, str]] | None, broker: BrokerClient
) -> bool:
    """docs/WIFI_DESIGN.md §4/§6, docs/WIFI_TASKS.md W7: `/down cfg.wifi =
    {en, nets?}`. `nets is None` omits the `nets` key from the pushed
    envelope entirely -- the wire's own "leave the stored networks alone,
    apply `en` only" rule (§4), which is what lets the web app toggle WiFi
    without re-sending a PSK. `nets == []` pushes an explicit empty array
    (clears the device's stored networks); a non-empty list wholesale
    replaces them. `nets` entries are `{"s": ssid, "p": psk}` (`app/store/
    device_secrets.py`'s `WifiNet.model_dump()` shape) -- the caller
    (`app/routers/devices.py`) has already validated the `<=2`/byte-length
    rules and the `tls == "pinned"` guard before calling this, so this
    function does not re-check either.

    **Never logs `nets`' contents** (this module logs nothing on success, same
    as `push_sms_contacts`/`push_cfg`; the "no such device" warning below
    logs only the device id, matching every other push function here).

    Stored under its own pending slot (`_PENDING_CFG_WIFI_FIELD`), independent
    of a pending `cfg.lock`/`cfg.ca`/`cfg.sms` -- see `_ALL_PENDING_FIELDS`'s
    docstring."""
    if devices_store.get_device(device_id) is None:
        logger.warning("push_wifi: no such device %s", device_id)
        return False
    cfg_wifi: dict[str, Any] = {"en": en}
    if nets is not None:
        cfg_wifi["nets"] = nets
    obj: dict[str, Any] = {
        "v": 1,
        "id": new_message_id(),
        "ts": int(time.time()),
        "kind": "cfg",
        "cfg": {"wifi": cfg_wifi},
        "ack": None,
    }
    _assert_within_envelope_limit(obj)
    _set_pending(device_id, _PENDING_CFG_WIFI_FIELD, obj)
    return broker.publish_down(device_id, obj)


def wifi_pending(device_id: str) -> bool:
    """True iff this device has a pushed `cfg.wifi` not yet acked `shown` --
    `GET`/`PUT /api/devices/{id}/wifi`'s `pending` field (`app/routers/
    devices.py`), same shape as `sms_pending` below."""
    snap = _devices().document(device_id).get()
    if not snap.exists:
        return False
    pending = (snap.to_dict() or {}).get(_PENDING_CFG_WIFI_FIELD)
    return isinstance(pending, dict) and not pending.get("acked", False)


def sms_pending(device_id: str) -> bool:
    """True iff this device has a pushed `cfg.sms` not yet acked `shown` --
    `GET`/`PUT /api/devices/{id}/sms-contacts`'s `pending` field
    (`app/routers/devices.py`). `False` (not an error) for an unregistered
    device, matching every other read in this module's "no such device is
    just an empty/false answer" style."""
    snap = _devices().document(device_id).get()
    if not snap.exists:
        return False
    pending = (snap.to_dict() or {}).get(_PENDING_CFG_SMS_FIELD)
    return isinstance(pending, dict) and not pending.get("acked", False)


def ack(device_id: str, msg_id: str) -> bool:
    """docs/PROTOCOL.md §3.2: a book/cfg is "acked `shown` once applied,"
    through the same `{"id":..., "ack":"shown"}` `/up` machinery as any
    other down message -- but a book/cfg id never exists in `messages/{id}`
    (see this module's docstring), so `app/ingest.py`'s `_handle_ack` calls
    this *after* `messages_store.get_message` comes back empty, to check
    whether the acked id is this device's current pending book or cfg
    instead of an actually-unknown id.

    Returns `True` if `msg_id` matched (idempotently -- a repeated ack for
    an already-acked id is a no-op write skip, `docs/PROTOCOL.md` §4.1 rule
    1), so the caller knows not to log/drop it as an unknown-id security
    event; `False` if it matched neither, which *is* still an unknown id."""
    ref = _devices().document(device_id)
    snap = ref.get()
    if not snap.exists:
        return False
    data = snap.to_dict() or {}
    matched = False
    updates: dict[str, Any] = {}
    for field in _ALL_PENDING_FIELDS:
        pending = data.get(field)
        if isinstance(pending, dict) and pending.get("id") == msg_id:
            matched = True
            if not pending.get("acked"):
                updates[f"{field}.acked"] = True
    if updates:
        ref.update(updates)
    return matched


def republish_pending(device_id: str, broker: BrokerClient) -> None:
    """docs/PROTOCOL.md §5.3: "re-publish ... the newest `book` and `cfg`
    ... one each ... not counted against [the 10-message] cap." Called by
    `app/ingest.py`'s `_republish_unacked` on the same online edge as the
    `pendingDeviceIds` loop. Re-publishes the *exact* previously-built `obj`
    (same `id`, same content) for whichever of `pendingBook`/`pendingCfg`
    is still unacked; a no-op for either that is missing or already
    acked."""
    snap = _devices().document(device_id).get()
    if not snap.exists:
        return
    data = snap.to_dict() or {}
    for field in _ALL_PENDING_FIELDS:
        pending = data.get(field)
        if not isinstance(pending, dict) or pending.get("acked"):
            continue
        obj = pending.get("obj")
        if not isinstance(obj, dict):
            continue
        logger.info(
            "re-publishing unacked %s %s to device %s", obj.get("kind"), obj.get("id"), device_id
        )
        broker.publish_down(device_id, obj)
