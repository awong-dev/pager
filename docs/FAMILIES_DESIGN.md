# Multi-family web UI and tenancy design (30 Sep 2026)

> **7 Oct 2026 (owner decision):** the relay has no SMS backend. Twilio, per-family
> `smsNumber`, `phoneIndex`, held/delivered inbound SMS and "text any number" are gone; an external is reached only by
> the pager's own SMS (`cfg.sms`). Members whose `policy.out` is `open`/`any_sms` get every family contact on their
> pager automatically. See `build/bench-logs/DESIGN_no_relay_sms.md` and CONTACT_REQ_DESIGN.md decision 7.
> 
> **8 Oct 2026 (owner decision):** the relay SMS backend returns for members who hold a per-user Twilio number (`users.smsNumber`). Inbound SMS from unknown numbers are held with an alert to family admins; known and approved senders are delivered. Outbound SMS goes through the relay by phone number to externals with the sender's number. See docs/RELAY_SMS_DESIGN.md.

**Status:** implemented overnight 30 Sep → 1 Oct 2026 per `docs/FAMILIES_TASKS.md`; see §10 for where
the build deviated from this text.

Owner request, 30 Sep: one deployment serves **many families**, each with its own parents (family
admins) and children (members, usually pager owners). A **superadmin** sees everything. A family
admin sees and controls only their family's people, devices, locations and the conversations their
members take part in. Conversations can be started by alias **or by phone number**; SMS
participants are outside the system, so their conversations are visible only to the member and
that member's family admins. Family admins set a per-member **conversation policy** (outbound and
inbound) and are notified of new conversations and of SMS from unrecognised numbers.

Companion to `docs/SERVER_PLAN.md` (§3 data model, §5 API, §7 web) and `docs/GROUP_CHAT_DESIGN.md`.
Where this document differs from them, this document wins. Nothing here changes
`docs/PROTOCOL.md`: the pager sees the same book, `cfg.sms` and envelopes as today.
`docs/FAMILIES_TASKS.md` is the execution plan.

## 0. Today, established from the code

- **One deployment = one household.** `SERVER_PLAN.md:43` says so. There is no family, tenant or
  owner-of-user field anywhere; scoping comes only from `devices.ownerUid`, `allow` edges, the
  `uids` arrays on `conversations`/`messages`, and one global `role: 'admin'|'member'`
  (`relay/app/store/users.py:29-39`).
- **Admin is decided three different ways**: the relay accepts the `admin` custom claim *or*
  `users.role == "admin"` (`relay/app/auth.py:72-77`); the rules accept only the claim
  (`relay/firestore.rules:17-30`); the web accepts only `role` from `GET /api/me`
  (`web/lib/auth-context.tsx:98`). `SERVER_PLAN.md` §5.3's "refreshed on `/api/me`" is not
  implemented (`relay/app/routers/me.py:119-121`).
- **Every admin page listens to a whole collection** (`users`, `allow`, `devices`,
  `contactRequests`; see `web/app/admin/*/page.tsx`). Members cannot read any `users/{uid}` but
  their own, so a member's chat list shows peers as `uid:xxxx` and `web/lib/directory.tsx` papers
  over it with `localStorage`.
- **Navigation is one MUI AppBar** (`web/components/AppShell.tsx`): Chat, Location (only if
  locatable devices exist), a Settings menu, an Admin menu (Users, Allow-list, Devices, Retention;
  `/admin/contacts` exists but is not linked).
- **An SMS-only person is already a full `users/{uid}`** with an `sms` backend and a
  `phoneIndex/{e164}` entry (`relay/app/routers/webhooks.py:215-272`, `routers/me.py:199-234`).
  Contact approval in `create` mode manufactures such a user from a pager `contact_req`
  (`routers/admin.py:752-768`). Inbound SMS from an unknown number is logged and dropped
  (`webhooks.py:236-244`). One deployment-wide Twilio number (`TWILIO_FROM_NUMBER`).
- **The pager's own modem SMS** is a separate transport: `devices.smsContacts` (max 8) is pushed
  as `cfg.sms`, the device shows only listed numbers, and every text is audited to
  `devices/{d}/smsLog` (`V02_DESIGN.md` §6).
- **Conversations** are `conversations/{convKey}` summaries plus top-level `messages/{id}` docs,
  one per (sender, recipient) pair, readable when `auth.uid in uids`
  (`relay/firestore.rules:49-55`). Group creation is admin-only; join is open to any member and
  silently writes mutual `locate` edges (`routers/conversations.py:191-204, 240-265`).
- **Push** exists only for a routed message to its recipient (`relay/app/backends/webapp.py`).
  Nothing notifies anyone of a new conversation, a contact request, or a blocked SMS.
- **The web has no test runner** (`ROADMAP.md`, Web). Relay tests are extensive, including
  emulator-backed rules tests (`relay/tests/test_rules.py`, 36 cases).

## 1. Decisions

1. **A family is a first-class document; every user and device belongs to exactly one.**
   `users.familyId`, `devices.familyId`. One family per user is enough for the stated goal
   (parents monitoring their children); shared custody across two families is out of scope.
2. **Three roles, one field, two claims.** `users.role ∈ {super, admin, member}` (`admin` now
   means *family* admin). Firebase custom claims `role` and `fam` are the **only** source of truth
   for the relay dependency and the rules; `GET /api/me` compares the token's claims with the user
   doc and returns `claimsStale: true` so the web forces `getIdToken(true)`. The legacy `admin`
   claim and the role-or-claim OR in `auth.py` go away.
3. **Visibility is denormalised onto the documents the rules guard.** `familyIds` on
   `conversations` and `messages` (union of the participants' families; externals contribute
   none), `familyId` on `devices`, `contactRequests`, `allow` edges. Rules stay lookup-free.
4. **Conversation read = participants ∪ family admins of any participant ∪ super.** Exactly the
   owner's rule. Because an SMS participant has no family, an SMS conversation's `familyIds` is
   just the member's family, so it is visible to the member and their admins and nobody else,
   with no special case.
5. **Location and device data never cross a family.** `locate` edges are refused unless both ends
   share a family; `devices.locatableBy` is therefore always in-family. Family admins may read and
   request location for every device in their family. Super may **read** stored fixes everywhere
   but does not get `/locate` by role (keeps the spirit of the 27 Sep decision; flagged in §9).
6. **An SMS number is an "external" user.** `users/{uid}` with `kind: 'external'`, `familyId`,
   no Firebase Auth account, alias = `uid`, `displayName` = the name an admin gave it, `phone` on the user doc,
   no backend row. It reaches the pager only through `cfg.sms` *(7 Oct 2026: no relay SMS)*. *(8 Oct 2026: externals now carry an `sms` backend row (`kind:"sms", verifiedAt: now, config:{phone}`) for relay members with a number; see docs/RELAY_SMS_DESIGN.md decision 3.)*
7. **Conversation policy lives on the member, is enforced in `routing.send`, and layers on top of
   the allow-list.** "Approved people" and "approved numbers" *are* the member's outgoing `allow`
   edges (to users and to externals respectively). A policy that says *any* bypasses the edge
   check for that participant type; a policy that says *approved* requires it. A DM from S to R
   passes only if S's **outbound** policy admits R and R's **inbound** policy admits S. Adults
   default to open/any, children to approved/approved. Externals have no policy.
8. **Family admins get their own admin surface; the global matrix stays for super.** A family
   admin manages members, policies, approved people and numbers, devices, contacts and alerts of
   their family through `/family/*` pages backed by a new `/api/family/*` router. Cross-family
   `message` edges (cousins in two families) are created by super in v1; a friend-request flow is
   a follow-up.
9. **Admin alerts are documents plus push.** `families/{fid}/alerts/{id}` for *new conversation*
   (a member on the `open` outbound policy started talking to someone they have no edge to) and
   *unrecognised SMS* (held, with approve/block actions). Pager `contactRequests` are shown in the
   same inbox. Every family admin's push tokens receive a `kind: "alert"` FCM message.
10. **Per-family Twilio number superseded 7 Oct 2026; per-user number restored 8 Oct 2026.** Each member may hold a relay SMS number (`users.smsNumber`, docs/RELAY_SMS_DESIGN.md decision 1) to send and receive SMS through the relay; see §9 for alerts and decision 6 for externals' backend rows.
11. **The pager's modem SMS path is a projection, not a second policy.** `devices.smsContacts`
    becomes derived (the member's approved numbers, capped at 8, editable only through the member's
    approved-numbers list). `any_sms` cannot be honoured on the device today because the firmware
    shows only listed numbers; the relay-side audit log still records blocked texts. Firmware
    follow-up, not in this plan. *(7 Oct 2026: plus every family contact when the member's numbers rule is `any`, minus explicit
    denies — CONTACT_REQ decision 7.)*

## 2. Policies

Two pickers per member, five presets each. Machine codes are stored; labels are what the UI
shows; the description is the one line under the radio button.

**Outbound** (`users.policy.out`, "who @kid can start or continue a chat with"):

| code | label | people (users) | numbers (SMS) | notes |
|---|---|---|---|---|
| `open` | **Open** | any | any | admins alerted when a new conversation starts |
| `people` | **People** | approved | none | default for members |
| `people_sms` | **People + Numbers** | approved | approved | |
| `sms` | **Numbers** | none | approved | |
| `any_sms` | **Any number** | none | any | literal reading of the request; see §9 |

**Inbound** (`users.policy.in`, "who may reach @kid"):

| code | label | people | numbers | notes |
|---|---|---|---|---|
| `any` | **Anyone** | any | any | default for admins |
| `people` | **People** | approved | none | default for members |
| `people_sms` | **People + Numbers** | approved | approved | |
| `sms` | **Numbers** | none | approved | |
| `any_sms` | **Any number** | none | any | unrecognised SMS is delivered *and* alerted |

Semantics in `routing.send` for a DM from S to R (each side evaluated with its own policy and the
other's `kind`): `people`-type rule → `allow/{S}_{R}.message` (outbound) or `allow/{R}_{S}.message`
(inbound) must exist; `any` → no edge needed; `none` → refused (`policy_out` / `policy_in` reasons,
mapped to 403 with a human message). Groups: every (sender, member) pair is checked the same way,
as today. Externals: no policy of their own; an inbound SMS from external X to member R checks only
R's inbound rule for numbers; an unrecognised X under `people`/`people_sms`/`sms` is **held** as an
alert, under `any`/`any_sms` it is **delivered and alerted**. *(8 Oct 2026: `any` on the inbound picker no longer delivers unknown numbers; all unknown SMS are held with an alert to admins. `any` now means the family's *contacts* reach this member without an edge — see docs/RELAY_SMS_DESIGN.md.)*  Location requests keep today's
`locate`-edge rule; policies do not affect them.

Defaults are applied at user creation from role (`member` → `people`/`people`, `admin`/`super` →
`open`/`any`) and by the migration for existing users.

*(7 Oct 2026: the numbers column now only decides whether every family contact is implied on the member's pager (`any`) or only approved ones; the relay carries no SMS, so inbound "held/delivered" no longer applies.)* *(8 Oct 2026: inbound SMS are held/delivered again, but with a per-user SMS number and held-to-approval flow, not per-family nor auto-delivery under `any` — see docs/RELAY_SMS_DESIGN.md.)*

## 3. Data model changes

```
families/{fid}                 {name, createdAt, createdBy}
families/{fid}/alerts/{id}     {kind: 'new_conversation'|'sms_unknown'|'contact_request',
                                status: 'open'|'handled'|'dismissed', ts, subjectUid, subjectAlias,
                                peerUid|null, peerAlias|null, peerPhone|null, preview (≤120),
                                heldBody|null, convKey|null, contactRequestKey|null,
                                decidedAt, decidedBy}
users/{uid}                    + familyId (null for externals), role: 'super'|'admin'|'member',
                                 kind: 'person'|'external', policy: {out, in}
devices/{d}                    + familyId
allow/{from}_{to}              + familyIds [fromFam?, toFam?]       (for family-admin reads)
conversations/{k}              + familyIds [..], participants: {uid: {alias, displayName, kind}}
messages/{id}                  + familyIds [..]
contactRequests/{key}          + familyId
contactNames/{fid}_{h16}       {uid, familyId} — unique contact name per family (7 Oct 2026)
```

`participants` is written when the conversation doc is created and refreshed on group join/leave
and on `displayName` change (same trigger list as the book push in `CHAT_UI_DESIGN.md` §1). It
retires the `localStorage` directory hack and the `uid:xxxx` rows.

**Indexes** (add to `relay/firestore.indexes.json`): `conversations(familyIds CONTAINS,
lastMessageAt DESC)`; `messages(familyIds CONTAINS, convKey, seq DESC)`; `users(familyId, alias)`;
`devices(familyId, label)`; `alerts` is per-family so single-field `ts` suffices;
`contactRequests(familyId, status, createdAt)`.

**Rules** (sketch; the real file is `relay/firestore.rules`):

```
function role()      { return request.auth.token.get('role', ''); }
function fam()       { return request.auth.token.get('fam', ''); }
function isSuper()   { return role() == 'super'; }
function isFamAdmin(f) { return role() == 'admin' && fam() == f; }
function sameFam(f)  { return fam() != '' && fam() == f; }

match /families/{f}            { allow read: if isSuper() || sameFam(f); }
match /families/{f}/alerts/{a} { allow read: if isSuper() || isFamAdmin(f); }
match /users/{u}               { allow read: if isSuper() || request.auth.uid == u
                                              || sameFam(resource.data.familyId); }
match /users/{u}/backends/{b}  { allow read: if isSuper() || request.auth.uid == u
                                              || isFamAdmin(resource.data.familyId); }   // needs familyId copied onto backends, or keep self/super only (see tasks)
match /conversations/{k}       { allow read: if registered() && (isSuper()
                                              || request.auth.uid in resource.data.uids
                                              || (role() == 'admin' && fam() in resource.data.familyIds)); }
match /messages/{m}            { same predicate on resource.data }
match /devices/{d}             { allow read: if isSuper() || (sameFam(resource.data.familyId)
                                              && (resource.data.ownerUid == request.auth.uid
                                                  || request.auth.uid in resource.data.locatableBy
                                                  || role() == 'admin')); }
match /devices/{d}/locations/{l} { allow read: if isSuper() || <parent predicate via get()>; }
match /devices/{d}/smsLog/{l}  { allow read: if isSuper() || owner || isFamAdmin(parent.familyId); }
match /allow/{e}               { allow read: if isSuper() || uid in [fromUid,toUid]
                                              || (role()=='admin' && fam() in resource.data.familyIds); }
match /contactRequests/{r}     { allow read: if isSuper() || uid == ownerUid || isFamAdmin(resource.data.familyId); }
match /settings/{s}            { allow read: if registered(); }
match /{document=**}           { allow write: if false; }
```

Externals have `familyId: null`, so `sameFam(null)` is false: a member can read an external's
`users` doc only through the conversation `participants` map, which is what the chat needs.

## 4. API changes

Auth (`relay/app/auth.py`): `Principal {uid, role, familyId}` from claims only.
`require_user`, `require_family_admin` (role `admin` on own family, or `super` with
`?family=` / `X-Family` selecting any family), `require_super`. `require_admin` is deleted; every
current `/api/admin/*` route is re-homed under one of the two below.

**`/api/me`**: adds `familyId`, `kind`, `policy`, `claimsStale`. **`GET /api/directory`**: the
aliases the caller may resolve (own family, edge peers, participants of own conversations, own
externals), for members whose rules-visible set is narrower than what they talk to.

**`/api/family/*`** (family admin; super with `?family=`):

| route | does |
|---|---|
| `GET /api/family` | the family doc plus counts |
| `GET/POST /api/family/members` | list; create person (alias, displayName, email\|phone, role admin\|member) with default policy |
| `PATCH /api/family/members/{uid}` | `displayName`, `role` (admin↔member), `disabled`, `policy{out,in}` |
| `PUT /api/family/members/{uid}/approved` | `{people:[{alias, message, locate}], numbers:[phone…]}` → rewrites that member's outgoing edges; refuses `locate` to another family; unknown number creates an external; re-derives `devices.smsContacts` and pushes `cfg.sms` + the book |
| `GET/POST/PATCH /api/family/contacts` | externals used by this family (`{phone, name}`); PATCH renames |
| `GET/POST /api/family/devices`, `/{id}/rotate-credentials`, `/revoke`, `/cfg`, `/ca`, `DELETE` | today's admin device handlers with an in-family check |
| `POST /api/family/groups` | today's group create, members restricted to own family and edge peers |
| `GET /api/family/alerts`, `POST /api/family/alerts/{id}/{approve\|block\|dismiss}` | approve = create external if needed + edges; block = mark and add to `families.blockedNumbers` |
| `GET /api/family/conversations` | optional; the web reads Firestore directly by `familyIds` |

**`/api/admin/*`** (super only): `GET/POST /api/admin/families`, `PATCH /api/admin/families/{fid}`
(`name`), `PATCH /api/admin/users/{uid}` gains `familyId` and `role: super`,
`GET /api/admin/users|devices` gain `?family=`, `PUT /api/admin/allowlist` stays replace-all but
accepts `?family=` to replace only edges touching that family, and is the only writer of
cross-family edges. `/api/admin/contacts/*` moves to `/api/family/alerts/*`.

**Conversations**: An E.164 number resolves only to the sender's family contact, which is refused (`sms_contact`, 403) — the relay sends no SMS (7 Oct 2026). *(8 Oct 2026: the relay SMS backend returns via `PATCH /api/family/members {smsNumber}`, but the routing decision is unchanged: E.164 in a DM attempts relay delivery only if the sender has a number; see docs/RELAY_SMS_DESIGN.md decision 2.)*  403 bodies carry `reason ∈ {policy_out, policy_in,
not_allowed, not_member, sms_contact, no_sms_number}` and a message the UI shows verbatim. `POST /api/conversations` (group
create) moves to `/api/family/groups`; join (`POST …/members`) requires the joiner to be a family
admin of the group creator's family and no longer writes `locate` edges.

**Webhooks**: none for SMS (removed 7 Oct 2026). *(8 Oct 2026: restored for inbound SMS only — `POST /webhooks/twilio/sms` per-IP limited, signature-verified, holds unknown senders for admin approval; see docs/RELAY_SMS_DESIGN.md decision 4 and §4 of this doc.)*

## 5. Web UI

### 5.1 Shell and identity

- `AuthProvider` exposes `me: {uid, alias, displayName, role, familyId, kind, policy}`,
  `isSuper`, `isFamilyAdmin`; on `claimsStale` it calls `getIdToken(true)` and refetches.
- `FamilyProvider` / `useFamily()`: the family in scope. For `admin`/`member` it is `me.familyId`.
  For `super` it comes from `?family=` (persisted in `sessionStorage`), and the AppBar shows a
  **family switcher** chip ("Family: Wong ▾") that lists all families. Every `/family/*` and
  `/location` query uses this scope; `/api/family/*` calls carry it as `?family=`.
- `RequireAuth` gains `requireRole: 'admin' | 'super'` (`admin` accepts super).
- **Navigation** by role:

| role | items |
|---|---|
| member | Chats · Location (if any visible device) · Settings ▾ (Backends, Notifications, My devices) |
| admin | Chats · Location · **Family ▾** (People, Devices, Contacts, Alerts *badge*) · Settings ▾ |
| super | as admin, plus **Admin ▾** (Families, All users, All devices, Allow-list, Retention) and the family switcher |

- `useDirectory()` is rewritten: listens to `users where familyId == fam` (rules now allow it),
  merges `GET /api/directory`, and reads `conversations.participants`; `localStorage` and
  `learn()` are removed.

### 5.2 Chats (`/chat`, `/chat/[alias]`, new `/chat/view/[key]`)

- **My chats**: as today, with peers named from `participants`; group rows unchanged; an SMS
  conversation (a participant with `kind: 'external'`) shows a phone icon and the number under the
  name.
- **New chat** button (all roles) → `NewChatDialog`: one text field "Name, @alias or phone
  number"; suggestions from the directory grouped *Family / People / Numbers*; a value that parses
  as a phone number is normalised to E.164 and shown as "Text +1 555 123 4567"; Enter navigates to
  `/chat/{alias}`. The thread's first send surfaces a policy 403 as an inline alert ("Your family
  admin has limited who you can message. Ask them to add @x."). Replaces the free-text alias box.
- **New group** button for family admins (and super in a family scope) → `NewGroupDialog`, member
  list from the directory instead of the admin `users` listener.
- **Family tab** (admins only): a second tab on `/chat`, "Family", listing every conversation
  whose `familyIds` contains the family and that I am **not** party to, from
  `conversations where familyIds array-contains fam`. Each row: member avatar + "@kid ↔ @peer" (or
  the group name), phone icon for SMS peers, last preview, an eye icon meaning *monitoring*. Rows
  open `/chat/view/{convKey}`.
- **`/chat/view/[key]`**: read-only thread for a conversation I am not in. Banner: "You're viewing
  @kid's conversation with @peer as a family admin." No composer, no mark-read, no locate. Query:
  `messages where convKey == key and familyIds array-contains fam orderBy seq desc`. "Load older"
  works for DMs and groups (fixes the group gating bug). The regular `/chat/[alias]` route is
  unchanged for conversations I am party to; when an admin opens `/chat/{alias}` for a member they
  are not talking to, it renders that member's own conversation list filtered view instead.
- `NotificationWatcher`: fixes the group-as-DM bug (use the conversation's alias, not `uids[1]`);
  also raises admin alerts (`families/{fam}/alerts where status == 'open'`) as foreground
  notifications and feeds the Alerts badge.

### 5.3 Location (`/location`)

Devices visible = own devices ∪ `locatableBy` devices ∪ (admin) all family devices, all read with
a `familyId == fam` filter so the rules and the query agree. "Locate now" is enabled for the owner,
edge holders and family admins. Super sees the family switcher and read-only fixes; "Locate now"
hidden for super unless they hold an edge (§1.5, flagged §9).

### 5.4 Family admin pages (`/family/*`, `requireRole: 'admin'`)

- **People** (`/family/people`): table of members: name, `@alias`, role chip, sign-in
  (email/phone), devices (chips linking to `/devices/{id}`), policy chips ("Out: People · In:
  People"), enabled switch. **Add person** dialog: alias, name, email or phone, role. Row click →
  **Member drawer** with three sections:
  1. *Profile*: display name, role (admin/member), disabled.
  2. *Policy*: two radio groups (Outbound / Inbound) with the five labels and one-line descriptions
     from §2. Saving PATCHes `policy`.
  3. *Approved*: "People" (checkbox list of family members and of linked people from other
     families, with a Locate toggle that is disabled for out-of-family rows and explains why) and
     "Numbers" (list of externals with name and number, add by number+name, remove). Saving PUTs
     `/approved`. A note under Numbers says the pager itself lists at most 8 numbers.
- **Devices** (`/family/devices`): today's `/admin/devices` table scoped to the family (owner
  picker = family members), `SetupCodePanel` unchanged, plus link to `/devices/{id}`. On
  `/devices/[id]` the SMS-contacts editor becomes read-only ("managed from People → @kid →
  Approved numbers"), the log and Wi-Fi panel stay.
- **Contacts** (`/family/contacts`): the family's externals (name, number, approved for which
  members, last message), rename, add; and a read-only "Linked families" list of cross-family
  edges with "ask your superadmin to link a person from another family".
- **Alerts** (`/family/alerts`): inbox of `alerts` newest first, three kinds with distinct icons.
  `sms_unknown`: number, held text preview, "to @kid", actions **Approve** (name field, approve
  for @kid, releases the held text), **Block**, **Dismiss**. `new_conversation`: "@kid started a
  chat with @peer", **View** (→ `/chat/view/{key}`), **Approve** (adds the edge), **Dismiss**.
  `contact_request`: today's `/admin/contacts` approve/reject dialog, moved here. Handled items
  collapse under a "Show handled" toggle.

### 5.5 Superadmin pages (`/admin/*`, `requireRole: 'super'`)

- **Families** (`/admin/families`, new): table (name, SMS number, admins, members, devices,
  created), **Create family** (name, first admin: existing user or new person), row → family
  detail: rename, set SMS number, move a user in/out (PATCH user `familyId`), promote/demote
  admins, "Open as family" (sets the switcher and goes to `/family/people`).
- **All users** (`/admin/users`): today's page plus a Family column and filter; create takes a
  family; role picker includes super.
- **All devices** (`/admin/devices`): today's page plus Family column and filter.
- **Allow-list** (`/admin/allowlist`): today's matrix, filtered to one family by default with a
  "Show all families" switch for cross-family edges; Locate checkboxes disabled across families.
- **Retention** unchanged.
- `/admin/contacts` is deleted (superseded by `/family/alerts` under the switcher).

### 5.6 Settings (`/settings/*`)

Unchanged, except `/settings/notifications` gains a per-user toggle "Family alerts" for admins
(stored on `users/{uid}.notify.alerts`, honoured by the alert push).

## 6. Notifications

- **Alert creation** (`relay/app/alerts.py`, new): `new_conversation` when `routing.send` creates
  a `conversations` doc for a DM whose sender has `out: open` and no `message` edge to the
  recipient; `sms_unknown` from a device `sms_log` of an unlisted number; `contact_request` when a pager
  `contact_req` is stored (wraps today's `contactRequests` write).
- **Push**: `backends/webapp.py` gains `push_alert(family_id, alert)` sending `{kind: "alert",
  alertKind, id, title, body, url: "/family/alerts"}` to every token of every admin of that
  family whose `notify.alerts` is not false. The service worker's `onBackgroundMessage` already
  switches on `kind`; add the `alert` arm and a `pager-alert-{id}` tag.
- **Foreground**: `NotificationWatcher` listens to open alerts (admins only) and shows a browser
  notification for new ones when the tab is hidden or not on `/family/alerts`.

## 7. Bootstrap (no migration)

Owner, 30 Sep: everything in production is test data and may be destroyed. There is no backfill
job and no dual-accepting rules step. The project's Firestore and Auth data are wiped and
`python -m app.bootstrap --admin-email …` is re-run; it now creates `families/default` (name
"Home"), the bootstrap user as `role: super` in that family, sets the `{role, fam}` claims, and
writes `settings/meta.schemaVersion = 2`. The runbook line lives in `infra/README.md`. Rules go
straight to the claim-only predicates in §3.

## 8. Phases

| phase | delivers | user-visible |
|---|---|---|
| **1 Tenancy core** | families, roles/claims, `familyId` everywhere, rules v2, bootstrap, `/api/family/*`, `/api/admin/families`, `GET /api/directory`; web: `FamilyProvider`, role-aware nav, switcher, `/admin/families`, `/family/people` (profile only), `/family/devices`, directory rewrite | admins get scoped pages; no messaging behaviour changes |
| **2 Visibility** | `familyIds` on conversations/messages, `participants`, rules for admin reads, Family tab, `/chat/view/[key]`, location scoping, `locate` edge in-family check | admins can monitor; `uid:xxxx` gone; cross-family location closed |
| **3 Policies and numbers** | `policy` field + routing gate, approved people/numbers editor, externals by digits alias, `NewChatDialog` with phone input, `smsContacts` derived, 403 reasons in the thread | policies work end to end; start a chat by number |
| **4 Alerts** | alerts collection + push + SW arm, `/family/alerts` with contact requests merged, `new_conversation` alert | admins are notified and can act |
| **5 Cleanup** | delete `/admin/contacts` and `require_admin`, `SERVER_PLAN.md` §3/§5.1/§7 and `web/README.md` checklist updated, `ROADMAP.md` entries | none |

Phases 1 and 2 are sequential. Phase 3's relay work can start after phase 1; its web work after
phase 2. Phase 4 needs 3. Group-handling bug fixes from the web map (notification watcher, load
older, member list) ride along with phase 2.

## 9. Flagged for the owner

1. **Moot 7 Oct 2026 (no relay SMS).**
2. **`any_sms` outbound is read literally** as "numbers only, no people". If the intent was
   "approved people plus any number", the code is `people_anysms` and the table in §2 gains a row.
3. **Moot (CONTACT_REQ decision 2; no sms backend).**
4. **Cross-family edges are super-only in v1.** A family-admin-to-family-admin request/accept flow
   is the natural follow-up.
5. **Externals are global**, one per number, with one display name. A per-family nickname is a
   small addition if two families know the same number by different names.
6. **Held SMS bodies** *(8 Oct 2026: now live in `heldSms`, swept with messages using the same TTL as the message retention window)* are stored in the alert until handled; *(7 Oct 2026: retention sweep should include `alerts` (default 90 days))*.
7. **The pager cannot honour `any_sms` inbound on its modem** (decision 11); texts from unlisted
   numbers are still blocked on-device and only audited. A firmware `cfg.sms` mode flag would be
   needed; not in this plan.
8. **A child with a web login sees their own SMS conversations**, as any participant does. The
   request's "only viewable by the family admin" is read as "not by anyone outside the family".
9. **Group join** loses its silent mutual-`locate` side effect and becomes admin-only; any member
   relying on self-service join is affected.

## 10. Implementation notes (1 Oct 2026)

Where the overnight build deviated from the sections above; the code is the reference now.

1. **`users/{uid}/backends` rules** stay self-or-super (§3's `isFamAdmin(resource.data.familyId)`
   needs a `familyId` copy on backends that does not exist). The People page reads sign-in
   details from the `users` doc, not from backends.
2. **External creation on the message route** happens only when the sender's outbound rule for
   numbers is `any` (`open`, `any_sms`); under `people_sms`/`sms` an unknown number is 404
   `unknown_alias` (the admin adds it through Approved numbers first).
3. **Contact approval in `create` mode** still creates a Firebase Auth user with an `sms` backend
   (today's path) rather than an `external`; switching it to `externals.get_or_create` would rename
   the alias to digits. Both shapes route identically.
4. **`LocateCrossFamily` is enforced at every API writer of `locate` edges** (`PUT
   /api/admin/allowlist`, `PUT /api/family/members/{uid}/approved`) rather than inside
   `store/allow.set_edge`; group create/join and contact approval never write `locate`.
5. **Super is not pushed alerts** (no single family); family admins only, honouring
   `notify.alerts`.
6. **The policy gate is mutual**: a DM between two `people`/`people` members needs an edge in each
   direction. The allow-list matrix and the Approved editor write both directions; tests that set
   one edge were updated.
7. **A `devices` query without `where('familyId','==', fam)` is denied by the rules** for every
   non-super user, including the owner; every client query carries the filter.
8. **Alerts `contact_request` cards** use Block/Dismiss in place of the old Reject-with-reason.
9. **Migration replaced by wipe + bootstrap** (§7); `--migrate-families` was never built.
