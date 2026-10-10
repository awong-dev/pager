# Book: add anyone — execution tasks (10 Oct 2026)

Design: `docs/BOOK_ADD_ANYONE_DESIGN.md` (D-numbers below refer to its decisions). Order: **B1→B4**
first (relay contract), then **W1** and **F1** in parallel, **D1** last (after review), **A1** on the
bench. Every task: read before touching anything; `Files` is exhaustive; `Verify` is the exact
command. House rules: cite the D-number in docstrings, no unverified defensive code, numbers
redacted in logs, one INFO line per event, delete what the design retires (no A/B toggles).

Common verify (relay): `cd /Users/albert/src/pager/relay && ruff check . && ruff format --check . && pytest -q`
(needs `docker compose up -d firebase`). Web: `cd /Users/albert/src/pager/web && npm run lint && npx tsc --noEmit && npm run build`.
Firmware: host tests `cd /Users/albert/src/pager/firmware/host && make test`; build per memory
(export `IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.2_py3.11_env` before `export.sh`, log to the scratchpad).

---

## Backend (backend-dev)

### B1 Marker + listing (D4, D5, D3)
- **Read:** D3-D5; `relay/app/book.py` (`_nicks`, `set_nick`, `sms_contacts_for`, `entries_for`,
  `BookEntry`), `relay/app/devcfg.py` `_approved_contacts`, `relay/tests/test_book.py`, `relay/tests/test_device_book.py`.
- **Files:** `relay/app/book.py`, `relay/app/devcfg.py`, `relay/tests/test_book.py`, `relay/tests/test_device_book.py`.
- **Do:** `_book_docs(owner)` returns `{peer: dict}` (`_nicks` derives from it). `BookEntry.added: bool = False`.
  `entries_for` adds marked peers that are not already listed: persons (non-disabled) through the existing
  peer loop, and externals with `ownerFamilyId == owner.familyId` and a phone through the external loop
  (same `sendable`/`reason` computation). `sms_contacts_for` stays **unchanged**. `_approved_contacts` keeps `e.sendable or e.added`.
  `on_pager(owner, peer_uid) -> bool` (in `c[]` for a relay-number owner; in `sms_contacts_for[:8]` for a modem owner) for D3.
- **Verify:** common relay. New tests: `test_added_external_listed_not_sendable` (people_sms, no edge → entry
  `added=True, sendable=False`, present in `build_book_body` `c[]` with `t:"sms"`);
  `test_implied_unsendable_still_hidden` (policy `sms` sibling with no marker → absent from `c[]`);
  `test_marker_not_in_sms_contacts_for` (marker only → `sms_contacts_for` excludes it);
  `test_book_always_has_c` (empty book → `"c" in obj and "p" not in obj`).

### B2 Add at ingest (D1, D2, D6, D12, D13, D14)
- **Read:** D1-D3, D6, D12-D14; `relay/app/ingest.py` `_handle_contact_req`/`_classify_contact_req`,
  `relay/app/store/contacts.py`, `relay/app/store/externals.py` (`get_or_create`, `ContactNameTaken`, `name_key`),
  `relay/app/store/rate_limits.py`, `relay/app/book.py` `set_nick`, `relay/app/devcfg.py` `_listed_requests`, `relay/tests/test_contacts.py`, `relay/tests/test_ingest.py`.
- **Files:** `relay/app/ingest.py`, `relay/app/store/contacts.py`, `relay/app/devcfg.py`, `relay/app/book.py`,
  `relay/tests/test_contacts.py`, `relay/tests/test_ingest.py`.
- **Do:** Classify per D2/D3: outcomes `in_book|added|bad_number|blocked|no_contact`. Delete `pending_*`,
  `create_request`, `TooManyPending`, `count_pending`, `has_matching_pending_or_approved`, `_listed_requests`
  and `obj["p"]`, and stop bumping on a rejection. New `contacts_store.add_entry(device_id, owner_uid, req_id, name, peer_uid, nick) -> "added"|"full"|"dup"`
  implementing D14(d) in **one** Firestore transaction. Name fallback per D1 in a helper `_contact_for_add(fid, e164, name) -> (User, nick|None)`.
  Rate limit `book_add:{device_id}` 10/h after the dedup check. Modem path (D12): after the transaction,
  `rederive_sms_contacts`; if `policy.check(owner, contact, has_out, False)` refuses → fixable ? `alerts.approval_upsert(...)` (B3) + reply `added; needs a parent's OK to text` : reply `added; settings don't allow texting`.
  Reply body helpers next to `in_book_body`. Log `contact_req outcome=…`.
- **Verify:** common relay. Tests: `test_add_phone_creates_contact_and_marker` (row `status:"added"`, marker `added:true`, bv+1 on each device, no alert);
  `test_add_redelivery_is_noop` (same id twice → one row, one bump); `test_add_existing_number_other_name_sets_nick`;
  `test_add_name_taken_suffixes_last4` (contact `"Grandma 0100"`, nick `"Grandma"`, no exception);
  `test_add_alias_inbound_edge_marks` / `test_add_alias_foreign_no_edge_shared_reply` (body equals unknown-alias body);
  `test_add_cap_32_full`; `test_add_rate_10_per_hour`; `test_policy_hidden_sibling_not_in_book` (D3: marker written, no `already in your book`);
  `test_modem_owner_refused_add_alerts_and_replies`; `test_rejection_no_bump`.

### B3 Refused send to an added entry (D7, D8, D9)
- **Read:** D7-D9; `relay/app/ingest.py` `_handle_v2_up_message`, `_send_system_reply`; `relay/app/routing.py`
  `send`, `_policy_reject_reason`; `relay/app/alerts.py` (`contact_request`, `create`); `relay/app/store/alerts.py`; `relay/tests/test_routing.py`, `relay/tests/test_alerts.py`.
- **Files:** `relay/app/ingest.py`, `relay/app/alerts.py`, `relay/app/store/alerts.py`, `relay/tests/test_ingest.py`, `relay/tests/test_alerts.py`.
- **Do:** `routing.py` does **not** change. In ingest, when `result.rejected[0].reason in {"not_allowed","policy_out","policy_in"}`
  and `uid` has an `added` marker under the sender: compute `fixable` per D8; if fixable, call `alerts.approval_upsert(owner, peer)` →
  `"created"|"open"|"reopened"|"declined"` (store: `families/{fid}/alerts/cr_{owner}_{peer}` read+write in one transaction; push only on created/reopened);
  pick the body (`needs a parent's OK; resend once approved` / `not approved` / `not allowed`) with `<n>` = the entry label. Otherwise keep today's body.
  Delete `alerts.contact_request`'s `contactRequestKey` path.
- **Verify:** common relay. Tests: `test_refused_added_fixable_alerts_once` (two sends → one alert doc, one FCM push, two replies with distinct ids);
  `test_refused_added_unfixable_no_alert` (policy `people`, phone → `not allowed`, no alert);
  `test_refused_not_added_keeps_unknown_recipient`; `test_dismissed_within_24h_replies_not_approved`; `test_dismissed_25h_reopens`;
  `test_refused_message_not_stored` (no `messages` doc for the wire id); `test_marker_never_permits` (marker + `people_sms` + no edge → rejected).

### B4 Approve + Remove (D10, D15 API)
- **Read:** D10, D15; `relay/app/routers/family.py` `_approve_contact_request`, `_approve_new_conversation`; `relay/app/routers/book.py`; `relay/tests/test_family_router.py`.
- **Files:** `relay/app/routers/family.py`, `relay/app/routers/book.py`, `relay/app/book.py`, `relay/tests/test_family_router.py`, `relay/tests/test_book.py`.
- **Do:** Replace `_approve_contact_request` with D10 (409 `superseded; add again from the pager` when `contactRequestKey` is set).
  `GET /api/book` entries carry `added`. `DELETE /api/book/{owner}/added/{peer}`: same auth and limiter as the nickname
  routes; one transaction clears `added` (deleting the doc if no `nick` is left) and bumps; push afterwards.
- **Verify:** common relay. Tests: `test_approve_added_writes_edge_and_bumps` (then a resend delivers);
  `test_approve_keeps_existing_locate`; `test_approve_legacy_409`; `test_remove_added_keeps_nick`; `test_remove_added_other_family_404`.
  `relay/tests/test_rules.py`: assert that a client write to `users/{u}/book/{p}` with `added` is denied.

## Web (web-dev)

### W1 Cards and lists (D15)
- **Read:** D15; `web/components/AlertCard.tsx` (`contact_request` branch), `web/lib/types.ts`, `web/app/settings/book/*`, `web/app/family/contacts/*`.
- **Files:** those four paths only.
- **Do:** Card copy "@kid wants to text <name> (<phone>) · added from the pager", or "@kid wants to message @peer (<name>)"; Approve / Block (phone only) / Dismiss.
  `/settings/book`: an "Added from pager" chip, the existing reason text, and a Remove button (confirm) → the DELETE route.
  Contacts: an "added by @alias" chip per marker holder (from `GET /api/book` per member, or a field the contacts API returns; take the cheaper one).
- **Verify:** common web, plus a manual check of an emulator-seeded `added` entry and an alert.

## Firmware (firmware-dev)

### F1 Copy and pending rows (D16)
- **Read:** D16; `firmware/main/scr_book.c` (`add_submit` ~226, hint ~293, request rows ~472-486),
  `firmware/main/scr_pick.c` (~78, ~126, ~192-205), `firmware/main/book.h` (~316-333), `firmware/main/book.c` (`BK_P`, `is_nudge`).
- **Files:** `firmware/main/scr_book.c`, `firmware/main/scr_pick.c`, `firmware/main/book.h`.
- **Do:** toast `"added"`, hint `"enter add"`; delete the request-row branches and the `n_requests` terms from the row counts
  in both screens. Leave `book.c` parsing alone. Rewrite the `book_request()` comment: digits are a phone, the relay adds the entry, rate limits are relay-side (10/h, 32 added).
- **Verify:** host tests pass; firmware build succeeds; `grep -n "pending approval\|not approved\|sent for approval" firmware/main/scr_*.c` returns nothing.

## Docs (docs-writer, after review)

### D1 PROTOCOL + cross-references
- **Read:** the design's "Proposed PROTOCOL.md text"; PROTOCOL §3.1 (`p` row), §3.2, §4.2; CONTACT_REQ_DESIGN decisions 1-2; ADDRESS_BOOK_DESIGN decisions 1-3; FAMILIES_DESIGN deviation 2.
- **Files:** `docs/PROTOCOL.md`, `docs/CONTACT_REQ_DESIGN.md`, `docs/ADDRESS_BOOK_DESIGN.md`, `docs/FAMILIES_DESIGN.md`.
- **Do:** paste the proposed text verbatim (additive only). Add a one-line `*(10 Oct 2026: superseded by docs/BOOK_ADD_ANYONE_DESIGN.md Dn)*` to each amended decision.
- **Verify:** `git diff --stat docs/PROTOCOL.md` shows only insertions; `git diff docs/PROTOCOL.md | grep '^-[^-]'` is empty.

## Acceptance (bench, one owner; rc1 + the Pixel bridge paired to rc1's owner, policy `people_sms`)
1. Pager Add: name `GV`, number = the GV number (not yet a family contact). Expect: relay log `contact_req outcome=added`; within seconds the pager's book shows `GV` (`book fetch done applied`).
2. Send "hi" to GV. Expect: no `messages` doc; system reply `GV: needs a parent's OK; resend once approved`; one `cr_…` alert; a web push.
3. Send again → same reply, still one alert, no second push.
4. Parent clicks Approve on the web. Expect: edge `allow/{kid}_{x…}.message=true`, bv bump.
5. Kid resends → delivered via the bridge (the GV phone receives it; delivery chip "sent").
Pass = all five steps; quote times in PDT.
