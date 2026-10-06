# Contact requests, SMS contacts per family, family rename (7 Oct 2026)

*(prod: two pager "Add contact" requests became family-less PERSON users @aphone1/@aphone2 with no
SMS route — a bare-digit `ph` read as an alias, a card that hid what was asked, "Create new person"
as the default. Brief: `build/bench-logs/DESIGN_contact_req_brief.md`; decisions 5-7 are owner
follow-ups the same night.)*

**Firmware does not change.** The pager sends `ph` as typed (`scr_book.c` `add_submit`: digits,
`+digits`, or an alias without `@`), renders `p[]` `s:"no"` as "not approved" (`scr_book.c` ~482),
and renders any `from:"system"` message in a thread. Family names never reach the pager.

## Decisions
**1. Ingest classifies, validates and answers.** `ContactReqEnvelope._check_ph` only requires 1-16
chars, no control chars (a bad `ph` now gets a reply, not a silent malformed-drop). A pure
`classify_ph(ph)`: `^\d{10}$` → `+1`+d; `^1\d{10}$` → `+`+d; `^\+\d+$` must pass `_PHONE_E164_RE`
and `phonenumbers.is_possible_number`; any other digit string → bad *(digits are never an alias)*;
else `ALIAS_RE` → alias; else bad. Outcomes, first match wins (R = row `status:"rejected"`,
`reason` code, `decidedBy:"relay"`, no alert, then bump + push the book so `p[]` shows it):

| case | outcome | `system` body |
|---|---|---|
| bad | R `bad_number` | `<name>: number must be 10 digits or start with +` |
| phone in the family's `blockedNumbers` | R `blocked` | `<name>: number not allowed` |
| phone = a same-family person's verified number, or the family's SMS contact (decision 7) the owner already has an edge to | no row | `<name>: already in your book` |
| alias = owner, same-family person, or an existing owner→peer `message` edge | no row | same |
| alias of a non-disabled person outside the family with a `message` edge **to** the owner | pending **link** + alert | — |
| any other alias (unknown, external, group, disabled, no inbound edge) | R `no_contact` | `<name>: no contact @<alias>` |
| valid phone otherwise | pending **SMS** + alert | — |
| 5 pending already | unchanged | `too many pending requests` |

One body for every alias failure, so a pager cannot probe which aliases exist. Replies go through
`_send_system_reply` (id derived from the request id: a redelivery cannot alert twice). An admin's
Block/Dismiss is already visible as `p[]` `s:"no"`; a reason is only sent when the user can fix it.
**Dedup is transactional:** `create_request` writes with `DocumentReference.create()`;
`AlreadyExists` → return the stored row, and only the winner raises the alert.

**2. Approval never creates a person.** For `contact_request`, `mode`/`alias` are ignored and
`mode:"create"` → 400 `new people are added under Family > People`. The logic moves into
`family.py` (`admin._approve_contact_impl`, `ContactApproveRequest`, `_resolve_link_uid`,
`_slugify_name` deleted). At approve time the target is resolved again:
- **SMS:** a verified person number (`backends.get_by_phone`) → link rule. Otherwise
  `externals.get_or_create(owner.familyId, phone, request.name)` (decision 7), owner→contact
  `message`, `rederive_sms_contacts(owner)`, push `cfg.sms`.
- **Link:** peer still a non-disabled person, same family or with `message` edge peer→owner, else
  409 `@x no longer has an edge to @owner` (request stays pending). Write **only** the missing
  owner→peer edge (`message`, no `locate`); never rewrite the peer's edge *(today's code
  overwrites it with `locate:false`, silently revoking location)*.
- Then `contacts_store.approve`, `book.bump_and_push({owner})`.
Alerts gain `peerName`. SMS alert: `peerPhone`=E.164, plus `peerUid/peerAlias/peerName` if it is a
person's verified number. Link alert: `peerUid/peerAlias/peerName`. `preview` = typed name. Card:
SMS "SMS contact for @kid · Grandma · +1 206 555 0100" ("already known as …" when resolved),
Approve/Block/Dismiss; link "Link @kid to @gma (Grandma Jo) · asked as Grandma", Approve/Dismiss.
One-click Approve; the dialog, radio, alias field and the "Also allow location requests" box go
*(the box was a lie: `ApproveAlertRequest` has no `locate`)*.

**3. Orphans.** Invariant: `kind:"person"` ⇒ `familyId` names an existing family; `kind:"external"`
⇒ `familyId` null, `ownerFamilyId` non-null (decision 7), no Auth account. Enforced in
`users_store.create_user` (`ValueError`), `POST /api/admin/users` → 400 if no family resolves.
Cleanup `relay/scripts/cleanup_orphan_persons.py`: dry run by default; `--apply --uid U…`
**deletes** each orphan: edges both ways (recompute `locatableBy`), `backends`/`book`
subcollections, `aliases`, `users`, Auth user, the approved `contactRequests` rows and their
`contact_request` alerts; bumps former edge holders' books. Messages stay as history. Delete, not
attach to `default`: nobody can sign in as them, and the intent (an SMS contact) is one new request.

**4. Delivery chips say where it went.** pager: waiting for pager / sent to pager / on pager / read
on pager. webapp: waiting / sent to app / shown in app / read in app. sms: SMS queued / sent by
SMS. gchat: sent to Google Chat / read in Google Chat. failed → "<where> failed"; expired →
"expired"; fulfilled → "location received". Time suffix unchanged.

**5. A person's phone is a sign-in number, never an SMS route.** `users.phone` is the Firebase Auth
`phone_number` `/login` uses. Option (a) would turn a credential into a paid texting route with no
consent step; a person who wants SMS has the self-verified `sms` backend. People
(`web/app/family/people`) and Users (`web/app/admin/users`) label it "Sign-in phone", helper
"For signing in only. To text a number from a pager, add it under Contacts." (link);
`POST /api/family/members` and `POST /api/admin/users` normalise it (`normalize_phone`, 400 on
failure). @albertphone is a valid member; the script leaves it alone.

**6. Families can be renamed by their admin.** `PATCH /api/family` `{name}` (`FamilyScope`: own
family's admin, or super with `?family=`), name stripped, 1-40 code points, no control chars (the
same check added to super's `PATCH /api/admin/families/{fid}`). The id never changes. The name
appears only on the web: the super switcher (`AppShell`) and the admin Users/Devices tables read
`families` through a live listener and refresh by themselves; the new "Family: <name> · Rename"
header on `/family/people` re-fetches `GET /api/family` after saving. Nothing else holds the name:
there is no book bump, no `cfg`, nothing in `/status`, and no pager impact.

**7. An SMS contact belongs to one family; the number is not a key.** An external is
`users/{uid}`, `kind:"external"`, `familyId: null` (rules, `sameFam`, `locate` checks unchanged),
**`ownerFamilyId: fid`**, `phone: e164`, `displayName` = that family's name for it. Identity is
`(fid, e164)`: `uid = "x_" + h[:16]`, `alias = "x" + h[:11]`, `h = sha256(f"{fid}|{e164}").hex()`.
Deterministic ids make `get_or_create(fid, phone, name)` idempotent through `create_user`'s own
transaction, and lookups (`get_family_contact(fid, e164)`) need no index. The reverse index
`phoneIndex/{e164}.ext = {fid: uid}` (merge write) exists only for the shared-number webhook.
A verified person keeps `{uid, bid}` on the same doc: `set_phone_index` becomes `merge=True`, and
`clear_phone_index` deletes only `uid`/`bid`, so the two shapes cannot clobber each other.
`get_by_phone` returns persons only. Alias collision (40-bit hash) → `AliasTaken` → 400.
- **Inbound Twilio:** person match → as today. Else family = `resolve_family_for_to(To)`, or the
  `@alias` target's family; that family's contact exists → route from it. Otherwise the existing
  `_handle_unknown_sms` path for that family only (its `any`-policy branch creates the family's
  contact). Shared number with no `@alias`: if exactly one family in `phoneIndex.ext` → use it, else drop.
- **Device-direct `sms_log`:** the device's family attributes it. The row gains `peerUid` = that
  family's contact (or null). An `in` row from a number with no contact raises an **open**
  `sms_unknown` alert for that family (subject = owner, nothing held, at most one open per
  number). Approve creates the contact and edge, then re-derives the owner's SMS contacts.
- **`devices.smsContacts`** = one helper, `rederive_sms_contacts(owner)`: the owner's `message`
  edges to externals with `ownerFamilyId == owner.familyId`, `{name: displayName, phone}`, by
  name, cap 8, then `set_sms_contacts` + `push_sms_contacts` for each device. It is called from
  `put_approved`, SMS approval (decision 2), `sms_unknown` approval, contact rename and delete.
  Contacts page = externals with `ownerFamilyId == fid`; rename and delete are refused for any other family.
- **Callers changed:** `routers/family.py` (put_approved, create/patch contact, sms_unknown approve),
  `routers/conversations.py` (start by number → sender's family), `routers/webhooks.py`, `alerts.py`
  (`sms_unknown` peer lookup), `ingest._handle_sms_log`.
- **Migration:** none. Prod has zero externals and zero `phoneIndex` entries, so this is a clean
  cut, and the old global `get_or_create(phone, name)` is deleted. **Rules:** no change; `phoneIndex`
  stays relay-only (pinned in `test_rules.py`) and the `users` read rule is untouched because
  `familyId` stays null. **Indexes:** none; contact lookups are by doc id, and the Contacts list
  filters `list_users()` in Python, as today.

## Failure modes
- A crash after a row write is recovered by the redelivered webhook: it hits `AlreadyExists` and
  re-runs bump/push/reply, which are idempotent.
- A crash mid-approve leaves the request pending; a retry repeats the same idempotent writes.
- A foreign 10-digit number gets a reply telling the user to type the `+`.
- Two families holding the same number keep separate names and edges, and an inbound text is
  attributed only through the receiving family.

## Measure
- One INFO line `contact_req outcome=<pending_sms|pending_link|bad_number|blocked|no_contact|in_book|cap>`.
- Persons with `familyId==null`: 0 (the script's dry run).
- Externals without `ownerFamilyId`: 0.
- Auth users created by approvals: 0.

## Proposed PROTOCOL.md text (additive; owner applies)

**Applied to PROTOCOL.md §3.2 and §4.2 on 6 Oct 2026, verbatim.**

§3.2, appended to the `kind:"contact_req"` paragraph:
> A `ph` of digits only is a phone number, never an alias: 10 digits are read as `+1` and the
> digits, 11 digits starting with `1` as `+` and the digits, any other digit string is rejected. A
> request rejected at ingest (bad number, blocked number, or an alias neither in the owner's book
> nor a person with a `message` edge to the owner) is listed in `p[]` with `s:"no"` and answered by
> one `system` down message naming the request's `name` and the reason; a request the book
> already satisfies is answered `<name>: already in your book` and not recorded. *(the pager sends
> what was typed; a bare number read as an alias made family-less users in prod, 7 Oct 2026.)*

§4.2, after "Case 3's `system` reply is the single exception…":
> *(also §3.2's `contact_req` rejection and `too many pending requests` replies — same shape, one per offending `id`.)*

§3.3 budget: the longest body is a 48-byte name plus 44 bytes = 92 bytes (108 if the name is fully
escaped). The signed envelope is about 230 bytes, under 640.
