# Multi-family tasks

Companion to `docs/FAMILIES_DESIGN.md`, which holds the reasoning and the decisions; this file holds
only what an implementing agent needs. Each task names its agent, the files to read first, the files
it may touch, what to do, and how to verify. Section numbers (§) refer to `FAMILIES_DESIGN.md`
unless another document is named.

Relay: `cd relay && .venv/bin/pytest`. Rules (emulator-backed): `cd relay && docker compose up -d
emulator && .venv/bin/pytest tests/test_rules.py`. Web: `cd web && npm run lint && npm run build &&
npx tsc --noEmit`. The web has no test runner; every web task verifies by a manual checklist against
the emulators (`web/README.md:146-258` describes the setup: `relay/docker-compose.yml`, `python -m
app.bootstrap`, `npm run dev` with `NEXT_PUBLIC_USE_EMULATORS=1`).

Order: 1.1 → 1.2 → 1.3 → 1.4 → 1.5 → 1.6 → 1.7 → 1.8 → 1.9 → 1.10 → 2.1 → 2.2 → 2.3 → 2.4 → 2.5
→ 2.6 → 3.1 → 3.2 → 3.3 → 3.4 → 3.5 → 3.6 → 4.1 → 4.2 → 4.3 → 4.4 → 4.5 → 4.6 → 5.1 → 5.2 → 5.3
→ 5.4.

Dependencies: within a phase, relay tasks come before web tasks. 1.2/1.3/1.4 can run in parallel
once 1.1 lands; 1.7/1.8/1.9 in parallel once 1.6 lands; 1.10 any time after 1.6. 2.1 and 2.2 in
parallel; 2.4/2.5/2.6 in parallel once 2.3 lands. 3.1 may start as soon as 1.5 lands (it does not
need phase 2); 3.4/3.5/3.6 need 2.3. 4.1/4.2 in parallel after 3.3; 4.5/4.6 after 4.4. Phase 5
last.

**No migration.** Owner, 30 Sep: production holds disposable test data and will be wiped, and
pushing to `main` is allowed. §7 of the design (migration job, dual-accepting rules, schema gate)
is therefore not built: rules go straight to the final claim-only predicates in 1.4, and the
bootstrap (1.6) creates the default family. Each phase ends committed and pushed to `main`.

---

## 1. Tenancy core

### 1.1 Families store, user/device fields, claims — backend-dev

**Read:** §1 decisions 1-3, §3 data model; `relay/app/store/users.py:23-39,82-107`
(model, alias regex, `create_user`), `relay/app/store/devices.py:180-222` (`Device` model),
`relay/app/routers/admin.py:153-154,193,218` (`_set_admin_claim`), `relay/app/bootstrap.py:26-50`,
`relay/tests/test_users.py`, `relay/tests/test_admin.py`.
**Files:** new `relay/app/store/families.py`, `relay/app/store/users.py`, `relay/app/store/devices.py`,
`relay/app/routers/admin.py`, `relay/tests/test_users.py`, new `relay/tests/test_families_store.py`.
**Do:** add `families/{fid}` `{name, smsNumber|null, blockedNumbers: [], createdAt, createdBy}` with
`create_family`, `get_family`, `list_families`, `update_family` (name, smsNumber). Extend `User`
with `familyId: str|None`, `role: Literal['super','admin','member']`, `kind:
Literal['person','external'] = 'person'`, `policy: {out: str, in: str}` and `notify: {alerts:
bool} = {alerts: True}`. `create_user` takes `family_id`, `role`, `kind`, defaults `policy` by
role per §2 (`member` → `people`/`people`; `admin`/`super` → `open`/`any`). Extend `Device` with
`familyId`, set from the owner's `familyId` in device creation (`admin.py:461-561`) and add the
missing `bookVersion` and `pending*` fields to the model so they stop being dropped on read.
Replace `_set_admin_claim` with `set_claims(uid, role, family_id)` writing custom claims
`{role, fam}` exactly (no `admin` key); call it wherever role or family changes.
**Verify:** `pytest tests/test_users.py tests/test_families_store.py tests/test_admin.py` green with
new cases: create family; create user in family gets default policy by role; claims written as
`{role, fam}`; device creation copies `familyId`.

### 1.2 Principal and auth dependencies — backend-dev

**Read:** §4 first paragraph; `relay/app/auth.py:72-77` (the role-or-claim OR), `relay/app/routers/
devices.py:54-55` (the duplicated OR), `relay/app/routers/me.py:119-121`, `relay/tests/test_auth.py`.
**Files:** `relay/app/auth.py`, `relay/app/routers/me.py`, `relay/app/routers/devices.py`,
`relay/tests/test_auth.py`, `relay/tests/test_me.py`.
**Do:** add `Principal(uid, role, family_id)` built from the verified token's `role` and `fam` claims
only; a token without a `role` claim is treated as `member` with no family. Dependencies:
`require_user` (registered, returns `Principal`), `require_family_admin` (role `admin` on own family,
or `super` with the family chosen by `?family=` or `X-Family`; returns `(Principal, family_id)`;
403 when an admin names another family), `require_super`. Keep `require_admin` as an alias of
`require_super` until 5.1 removes it. `GET /api/me` returns `familyId`, `kind`, `policy`, `notify`,
and `claimsStale: true` when the token's `role`/`fam` differ from the user doc (and re-issues the
claims server-side so the next forced refresh picks them up). The owner-or-admin check in
`devices.py` becomes owner, or family admin of the device's `familyId`, or super.
**Verify:** `pytest tests/test_auth.py tests/test_me.py` green with: member token → 403 on
`require_family_admin`; admin of family A with `?family=B` → 403; super with `?family=B` → ok;
`/api/me` reports `claimsStale` when the doc role changed; a bare `admin: true` claim grants nothing.

### 1.3 Family router and superadmin family routes — backend-dev

**Read:** §4 tables (`/api/family/*` and `/api/admin/*`); `relay/app/routers/admin.py` (users
:157-218, devices :461-561, group create in `routers/conversations.py:207`), `relay/app/routers/
conversations.py:191-204,240-265`, `relay/tests/test_admin.py`, `relay/tests/test_conversations.py`.
**Files:** new `relay/app/routers/family.py`, `relay/app/routers/admin.py`, `relay/app/routers/
conversations.py`, `relay/app/main.py`, new `relay/tests/test_family_router.py`, `relay/tests/
test_admin.py`.
**Do:** `family.py` mounted at `/api/family`, every route under `require_family_admin`:
`GET /` (family doc plus member/device counts); `GET /members`, `POST /members` (`alias,
displayName, email|phone, role: admin|member` → the existing create-user path with `familyId` =
scope and default policy); `PATCH /members/{uid}` (`displayName`, `role` admin↔member, `disabled`;
`policy` is added in 3.1; 404 if the uid is not in the family); `GET /devices`, `POST /devices`,
`POST /devices/{id}/rotate-credentials`, `/revoke`, `/cfg`, `/ca`, `DELETE /devices/{id}` by
calling the existing admin handler functions after an in-family check (owner alias must resolve to
a member of the scope family); `POST /groups` = today's group create restricted to members whose
`familyId` is the scope or who hold a `message` edge with the creator. Group join (`POST
/api/conversations/{alias}/members`) now requires the caller to be a family admin of the creator's
family and no longer writes `locate` edges (only `message`, both ways). Admin router: `GET/POST
/api/admin/families`, `PATCH /api/admin/families/{fid}` (`name`, `smsNumber`); `PATCH
/api/admin/users/{uid}` accepts `familyId` and `role: super` (re-issues claims); `GET
/api/admin/users` and `/devices` accept `?family=`. Route-level rate limits as in
`require_admin_write_rate_limit`.
**Verify:** `pytest tests/test_family_router.py tests/test_admin.py tests/test_conversations.py`
green: family admin lists only own members/devices; creating a member sets `familyId` and claims;
device create for an out-of-family owner → 403; group create with an out-of-family non-peer → 403;
join by a non-admin → 403; join writes no `locate` edge; super creates a family and moves a user.

### 1.4 Rules v2 — server-architect then backend-dev

**Read:** §3 rules sketch; `relay/firestore.rules` (whole file), `relay/tests/test_rules.py`,
`relay/tests/firebase_test_utils.py`.
**Files:** `relay/firestore.rules`, `relay/tests/test_rules.py`.
**Do:** add the helpers `role()`, `fam()`, `isSuper()`, `isFamAdmin(f)`, `sameFam(f)` from §3,
claim-only: `isSuper()` is `role() == 'super'` and nothing else; the old `admin` claim is not
consulted anywhere. Add `match /families/{f}` (read: super or
`sameFam(f)`), `match /families/{f}/alerts/{a}` (read: super or `isFamAdmin(f)`). Change
`users/{u}` to super, self, or `sameFam(resource.data.familyId)`; `users/{u}/backends/{b}` stays
self or super (no family copy on backends). `devices/{d}` → super, or `sameFam(familyId)` and
(owner, in `locatableBy`, or `role() == 'admin'`); `devices/{d}/locations/{l}` → super, or the
parent predicate via `get()`; `devices/{d}/smsLog/{l}` → super, owner, or `isFamAdmin(parent
.familyId)`; `allow/{e}` → super, a party, or `role()=='admin' && fam() in resource.data.familyIds`;
`contactRequests/{r}` → super, `ownerUid`, or `isFamAdmin(resource.data.familyId)`. `conversations`
and `messages` are unchanged in this task (2.2 adds the admin clause). Writes stay denied.
**Verify:** `pytest tests/test_rules.py` green with new cases per match block: same-family member
reads a family member's `users` doc, other-family member denied; family admin reads family device
and its locations, other-family admin denied; a bare `admin: true` claim is denied everywhere a
member would be; super reads a family doc; member denied on `alerts`.

### 1.5 Directory endpoint — backend-dev

**Read:** §4 (`GET /api/directory`), §5.1 `useDirectory()`; `web/lib/directory.tsx:5-35` (the
documented gap), `relay/app/store/allow.py`, `relay/app/store/messages.py:66-123`.
**Files:** `relay/app/routers/me.py`, `relay/tests/test_me.py`.
**Do:** `GET /api/directory` (`require_user`) returns `{entries: [{uid, alias, displayName, kind,
familyId, role}]}` = union of: users in the caller's family; both ends of every `allow` edge the
caller is a party to; every uid in the caller's conversations' `uids`. Externals included with
`kind: 'external'` and their number in `phone`. Cap 500 entries.
**Verify:** `pytest tests/test_me.py` green: a member sees family members, an edge peer in another
family, an external they have a conversation with, and not an unrelated user.

### 1.6 Bootstrap creates the default family — backend-dev

**Read:** `relay/app/bootstrap.py:26-50`, `relay/app/store/settings.py` (`settings/meta`),
`relay/app/store/families.py` (1.1), `relay/tests/test_bootstrap.py`.
**Files:** `relay/app/bootstrap.py`, `relay/tests/test_bootstrap.py`.
**Do:** `python -m app.bootstrap --admin-email …` on an empty database creates `families/default`
with `name: "Home"`, creates the bootstrap user with `role: 'super'`, `familyId: 'default'`,
`kind: 'person'`, the admin default policy, sets claims `{role: 'super', fam: 'default'}` (no
`admin` key), and writes `settings/meta.schemaVersion = 2`. Idempotent: re-running against a
database that already has the family and the user changes nothing. There is no migration and no
startup gate on `schemaVersion`.
**Verify:** `pytest tests/test_bootstrap.py` green: empty database → family, super user, claims and
`schemaVersion 2` exactly as above; second run is a no-op; `git grep -n "migrate-families"` empty.

### 1.7 Web: auth context, family scope, navigation — web-dev

**Read:** §5.1 (all); `web/lib/auth-context.tsx` (`Me` :31-36, `isAdmin` :98), `web/components/
RequireAuth.tsx:22-44`, `web/components/AppShell.tsx:25-36,86-108`, `web/components/Providers.tsx:
14-24`, `web/lib/api.ts`.
**Files:** `web/lib/auth-context.tsx`, `web/components/RequireAuth.tsx`, `web/components/
AppShell.tsx`, `web/components/Providers.tsx`, new `web/lib/family-context.tsx`, new
`web/components/FamilySwitcher.tsx`, `web/lib/api.ts`, `web/lib/types.ts`.
**Do:** `Me` gains `familyId`, `kind`, `policy`, `notify`; `isSuper = role === 'super'`,
`isFamilyAdmin = role === 'admin' || isSuper`; when `/api/me` returns `claimsStale`, call
`getIdToken(true)` and refetch once. `FamilyProvider`/`useFamily()` returns `{familyId, family,
setFamilyId}`: fixed to `me.familyId` for non-super; for super from `?family=` then
`sessionStorage['pager.family']`, defaulting to `me.familyId`. `api.ts` appends `?family=` to every
`/api/family/*` call when the caller is super. `RequireAuth` gains `requireRole: 'admin' | 'super'`
(`admin` accepts super) and redirects to `/chat` otherwise. `AppShell`: nav per the §5.1 table
(Family menu with People, Devices, Contacts, Alerts for admins; Admin menu with Families, All users,
All devices, Allow-list, Retention for super; the `FamilySwitcher` chip in the bar for super listing
`families` from Firestore). `UserDoc`/`DeviceDoc` types gain the §3 fields; add `FamilyDoc`.
**Verify:** lint/build/tsc clean. Manual: sign in as super → switcher visible, changing it updates
`?family=`; as family admin → Family menu, no Admin menu, no switcher; as member → neither; a
role change on the server followed by reload lands on the new nav after the forced token refresh.

### 1.8 Web: superadmin Families page and family filters — web-dev (parallel with 1.7 after agreeing on `useFamily`)

**Read:** §5.5; `web/app/admin/users/page.tsx`, `web/app/admin/devices/page.tsx:131-142`,
`web/app/admin/allowlist/page.tsx:50-56,115`.
**Files:** new `web/app/admin/families/page.tsx`, `web/app/admin/users/page.tsx`, `web/app/admin/
devices/page.tsx`, `web/app/admin/allowlist/page.tsx`.
**Do:** `/admin/families`: table from `families` (name, `smsNumber`, admins, member count, device
count, created); Create dialog → `POST /api/admin/families {name}` then optionally `POST
/api/family/members?family=` for the first admin; row detail: rename and SMS number via `PATCH
/api/admin/families/{fid}`, move a user in via `PATCH /api/admin/users/{uid} {familyId}`,
promote/demote via `{role}`, "Open as family" sets `useFamily().setFamilyId` and routes to
`/family/people`. `/admin/users` and `/admin/devices`: Family column, family filter (default = the
switcher family, "All" option), create dialog takes a family, role picker includes `super`.
`/admin/allowlist`: matrix rows/columns filtered to the switcher family with a "Show all families"
switch; Locate checkbox disabled and tooltipped when the two users' `familyId` differ. All pages
`requireRole: 'super'`.
**Verify:** lint/build/tsc clean. Manual against the emulator: create a second family, move a
member into it, promote them to admin, sign in as them → they see only their family on
`/family/people` (1.9); allow-list shows cross-family Locate disabled.

### 1.9 Web: family People (profile only) and Devices pages, directory rewrite — web-dev

**Read:** §5.1 `useDirectory()`, §5.4 People (profile section only) and Devices; `web/lib/
directory.tsx` (whole, esp. :144,174-190), `web/app/admin/devices/page.tsx` and
`SetupCodePanel.tsx`, `web/app/admin/users/page.tsx`.
**Files:** `web/lib/directory.tsx`, new `web/app/family/people/page.tsx`, new `web/components/
MemberDrawer.tsx`, new `web/app/family/devices/page.tsx`, `web/app/settings/devices/page.tsx`.
**Do:** `useDirectory()`: listen to `users where familyId == useFamily().familyId`, merge `GET
/api/directory` (fetched once per sign-in and on family switch), expose `byUid`, `byAlias`,
`contacts` (family + peers), `externals`; delete the `localStorage` store and `learn()`; keep
`groupByAlias`. `/family/people` (`requireRole: 'admin'`): table per §5.4 minus the policy chips
(3.4 adds them); Add person dialog → `POST /api/family/members`; row → `MemberDrawer` with the
Profile section (`PATCH /api/family/members/{uid}`); Policy and Approved sections are placeholders
until 3.4. `/family/devices`: copy of `/admin/devices` reading `devices where familyId == fam`,
owner picker from the directory's family members, calls rerouted to `/api/family/devices*`,
`SetupCodePanel` reused as is. `/settings/devices` unchanged except it reads the new `familyId`
type.
**Verify:** lint/build/tsc clean. Manual: as a family admin, People lists only the family; adding a
person appears live; creating a device shows the setup code and the device appears in the table
with the right owner; as a member, `/chat` peers resolve by alias without any `localStorage` key.

### 1.10 Runbook: wipe and re-bootstrap — infra-dev (any time after 1.6)

**Read:** `infra/README.md` (runbook), `relay/README.md` (bootstrap section), `relay/app/
bootstrap.py` (1.6).
**Files:** `infra/README.md`.
**Do:** add one runbook step for the multi-family deploy: "wipe Firestore and Firebase Auth in the
project, then re-run `python -m app.bootstrap --admin-email …`" with the exact `firebase`/`gcloud`
commands to delete all Firestore documents and all Auth users, and the note that this is a
one-time cutover because there is no migration from the single-household schema. Deploy order
after the wipe: rules and indexes, relay, web.
**Verify:** the runbook step reads top to bottom against a fresh emulator: wipe, bootstrap, sign in
as the bootstrap user, `/admin/families` shows "Home".

---

## 2. Visibility

### 2.1 `familyIds` and `participants` on conversations and messages — backend-dev

**Read:** §1 decisions 3-4, §3 (`conversations`, `messages`, index list); `relay/app/store/
messages.py:97-147,177-204,303-311`, `relay/app/store/conversations.py:50-100`, `relay/app/
routing.py:203-213,256-323`, `relay/app/location.py:618-656` (loc messages), `CHAT_UI_DESIGN.md`
§1 (the displayName trigger list), `relay/tests/test_messages.py`, `relay/tests/
test_conversations_store.py`.
**Files:** `relay/app/store/messages.py`, `relay/app/store/conversations.py`, `relay/app/routing.py`,
`relay/app/location.py`, `relay/app/routers/admin.py` (displayName change), `relay/firestore.
indexes.json`, `relay/tests/test_messages.py`, `relay/tests/test_conversations_store.py`,
`relay/tests/test_routing.py`.
**Do:** `create_message` and the loc-message writer set `familyIds` = sorted unique non-null
`familyId` of every uid in `uids`. Conversation creation (DM lazy create and group create) writes
`familyIds` the same way over the member list and `participants: {uid: {alias, displayName, kind}}`;
group join/leave and `displayName` changes rewrite both. Add indexes `conversations(familyIds
CONTAINS, lastMessageAt DESC)` and `messages(familyIds CONTAINS, convKey ASC, seq DESC)`, plus
`users(familyId, alias)`, `devices(familyId, label)`, `contactRequests(familyId, status,
createdAt)`.
**Verify:** `pytest tests/test_messages.py tests/test_conversations_store.py tests/test_routing.py`
green: a DM between families A and B carries `familyIds [A, B]`; a DM with an external carries only
the member's family; a group of three families carries all three; join adds the fourth; a rename
updates `participants`.

### 2.2 Rules: admin read of conversations and messages — backend-dev (parallel with 2.1)

**Read:** §3 rules sketch (`conversations`, `messages`); `relay/firestore.rules:49-55`,
`relay/tests/test_rules.py`.
**Files:** `relay/firestore.rules`, `relay/tests/test_rules.py`.
**Do:** predicate for both: `registered() && (isSuper() || request.auth.uid in resource.data.uids
|| (role() == 'admin' && fam() in resource.data.familyIds))`. Note a query must carry either `uids
array-contains me` or `familyIds array-contains fam` so the rule can be evaluated per document.
**Verify:** `pytest tests/test_rules.py` green: family admin reads a member's DM with an outside
family; the outside family's admin also reads it (§1.4 is symmetric); an admin of a third family is
denied; a member who is not a party is denied; an SMS conversation (`familyIds` = one family) is
denied to any other family's admin; an admin query with `familyIds array-contains` own family is
allowed and with another family denied.

### 2.3 Location scoping and in-family `locate` edges — backend-dev

**Read:** §1 decision 5, §5.3; `relay/app/store/allow.py:108-145`, `relay/app/routers/admin.py:
329-361` (allowlist PUT), `relay/app/routers/conversations.py:312-358` (locate), `relay/tests/
test_admin.py`, `relay/tests/test_conversations.py` (the 8 locate tests).
**Files:** `relay/app/store/allow.py`, `relay/app/routers/admin.py`, `relay/app/routers/
conversations.py`, `relay/tests/test_admin.py`, `relay/tests/test_conversations.py`.
**Do:** `allow` edges gain `familyIds`; any write of an edge with `locate: true` whose ends have
different (or null) `familyId` is refused with 400 `locate_cross_family`. `POST /api/conversations/
{alias}/locate` succeeds for: self, the target's owner, a `locate` edge holder, or a family admin
of the target's family; super gets it only through an edge. `GET /api/devices` for a family admin
returns every device of the family.
**Verify:** tests green: cross-family locate edge refused, same-family accepted; family admin can
`/locate` a family member's device and not another family's; super without an edge → 403; the
replace-all PUT rewrites `locatableBy` with in-family uids only.

### 2.4 Web: Family tab and read-only monitor thread — web-dev

**Read:** §5.2 (Family tab, `/chat/view/[key]`); `web/app/chat/page.tsx:70,201-247`,
`web/app/chat/[alias]/ThreadPageClient.tsx:231-291,365-378,553-568`, `web/firebase.json:22-29` and
`web/next.config.ts:39-42` (rewrites for dynamic routes), `web/lib/types.ts:173-186`.
**Files:** `web/app/chat/page.tsx`, new `web/app/chat/view/[key]/page.tsx` and
`MonitorThreadClient.tsx`, `web/app/chat/[alias]/ThreadPageClient.tsx` (factor the bubble list into
a shared `web/components/MessageList.tsx`), `web/lib/types.ts`, `web/firebase.json`,
`web/next.config.ts`.
**Do:** `ConversationDoc` gains `familyIds`, `participants`. `/chat`: name DM peers from
`participants` (drop the disabled `uid:xxxx` rows), phone icon plus number under the name when a
participant has `kind: 'external'`; for admins add a "Family" tab listing `conversations where
familyIds array-contains fam` minus those containing me, each row "@kid ↔ @peer" or the group name,
an eye icon, last preview, opening `/chat/view/{convKey}`. `/chat/view/[key]`: static shell with
`generateStaticParams → "_"` and the hosting rewrite `/chat/view/**` → `/chat/view/_.html` (before
the `/chat/**` rule), `MonitorThreadClient` reads the key from `usePathname()`, banner "You're
viewing @kid's conversation with @peer as a family admin", messages from `messages where convKey ==
key and familyIds array-contains fam orderBy seq desc limit N` with "Load older", no composer,
no mark-read, no locate. `RequireAuth requireRole: 'admin'`.
**Verify:** lint/build/tsc clean. Manual: as family admin, Family tab shows a member's DM with a
user in another family and the member's SMS conversation; opening one shows messages read-only;
as the other family's admin the same DM appears and the SMS one does not; as a member, no Family
tab; DM rows never show `uid:xxxx`.

### 2.5 Web: group-handling bug fixes — web-dev (parallel with 2.4)

**Read:** the three bugs: `web/components/NotificationWatcher.tsx:52` (group announced as a DM),
`web/app/chat/[alias]/ThreadPageClient.tsx:568` ("Load older" gated on `peerUid`),
`web/components/NewGroupDialog.tsx` (member list only filled for admins).
**Files:** `web/components/NotificationWatcher.tsx`, `web/app/chat/[alias]/ThreadPageClient.tsx`,
`web/components/NewGroupDialog.tsx`, `web/app/chat/page.tsx:183-187`.
**Do:** `NotificationWatcher`: for `kind === 'group'` announce "New message in {name}" and link to
`/chat/{conversation.alias}`; otherwise name the peer from `participants`. Thread: gate "Load
older" on having a resolved `convKey`, not on `peerUid`. `NewGroupDialog`: members from
`useDirectory().contacts` (now populated for admins through 1.9) and submit to `POST
/api/family/groups`; the "New group" button on `/chat` shows for `isFamilyAdmin`.
**Verify:** lint/build/tsc clean. Manual: a group message while on another page notifies with the
group name and opens the group; a group with more than one page of messages offers Load older; a
family admin creates a group from family members.

### 2.6 Web: location page scoping — web-dev (parallel with 2.4)

**Read:** §5.3; `web/lib/locatableDevices.ts:61-67`, `web/app/location/page.tsx:159-204`,
`web/components/AppShell.tsx:100-104`.
**Files:** `web/lib/locatableDevices.ts`, `web/app/location/page.tsx`, `web/components/AppShell.tsx`.
**Do:** `useLocatableDevices()` takes the family scope: own devices, `locatableBy` devices, and for
admins `devices where familyId == fam`; every query adds `where('familyId','==', fam)`. "Locate
now" enabled for owner, edge holder, family admin; hidden for super without an edge. Location nav
item shows for admins always.
**Verify:** lint/build/tsc clean. Manual: family admin sees all family devices on the map and can
Locate now; the other family's devices never appear, including for super until the switcher is
changed; a member sees only own and granted devices.

---

## 3. Policies and numbers

### 3.1 Policy field and routing gate — backend-dev (may start after 1.5)

**Read:** §2 (both tables and the semantics paragraph), §1 decision 7; `relay/app/routing.py:
180-213,245-252,295-305`, `relay/app/store/allow.py:24-42`, `relay/app/backends/resolve.py:28-54`,
`relay/app/routers/conversations.py:63-99`, `relay/tests/test_routing.py`, `relay/tests/
test_conversations.py`.
**Files:** new `relay/app/policy.py`, `relay/app/routing.py`, `relay/app/routers/family.py`,
`relay/app/routers/conversations.py`, new `relay/tests/test_policy.py`, `relay/tests/
test_routing.py`, `relay/tests/test_conversations.py`.
**Do:** `policy.py`: `OUT = {open, people, people_sms, sms, any_sms}`, `IN = {any, people,
people_sms, sms, any_sms}`, `rule(policy_code, peer_kind) -> 'any'|'approved'|'none'` per the §2
tables (`peer_kind` is `person` or `external`), and `check(sender, recipient, has_edge_out,
has_edge_in) -> None | 'policy_out' | 'policy_in'`. `routing.send`: for every (sender, recipient)
pair in a DM or group, evaluate the sender's `policy.out` against the recipient's `kind` and the
recipient's `policy.in` against the sender's `kind`; `approved` requires the existing edge check,
`any` skips it, `none` refuses; externals have no policy (skip their side). Keep the `not_allowed`
reason for an edge missing under `approved`. `PATCH /api/family/members/{uid}` accepts `policy
{out, in}` validated against the sets. 403 bodies from the message route carry `{reason, message}`
with the texts: `policy_out` → "Your family admin has limited who you can message.", `policy_in` →
"@x is not accepting messages from you.", `not_allowed` → "You are not on each other's approved
lists.".
**Verify:** `pytest tests/test_policy.py` covers the full 5×2×2 matrix as a table test; routing and
conversation tests: member `people` without edge → 403 `policy_out`; admin `open` to anyone → 201;
recipient `people` rejects a sender without inbound edge → `policy_in`; `sms` outbound to a person
→ `policy_out`; group message evaluates every pair.

### 3.2 Externals by number and approved-numbers PUT — backend-dev

**Read:** §1 decision 6 and 11, §4 (`/approved`, `/contacts`, E.164 on the message route);
`relay/app/routers/admin.py:752-778` (contact approve `create`), `relay/app/routers/me.py:199-234`
(`phoneIndex` write), `relay/app/store/devices.py:29-52,142-177` (`smsContacts`), `relay/app/
routers/devices.py:94-150` (sms-contacts PUT and `cfg.sms` push), `relay/app/devcfg.py:197-233`
(book), `relay/tests/test_contacts.py`, `relay/tests/test_sms.py`, `relay/tests/test_devcfg.py`.
**Files:** new `relay/app/store/externals.py`, `relay/app/routers/family.py`, `relay/app/routers/
conversations.py`, `relay/app/routers/devices.py`, `relay/app/routers/admin.py`, `relay/app/store/
allow.py`, `relay/tests/test_family_router.py`, `relay/tests/test_sms.py`, `relay/tests/
test_devcfg.py`, `relay/tests/test_contacts.py`.
**Do:** `externals.get_or_create(phone_e164, display_name)` → a `users/{uid}` with `kind:
'external'`, `familyId: null`, alias = E.164 digits, one `sms` backend `{config.phone}` marked
`adminVerified`, and the `phoneIndex` entry; idempotent on the number. Contact approve `create`
mode uses it instead of `auth.create_user`. `PUT /api/family/members/{uid}/approved {people:
[{alias, message, locate}], numbers: [{phone, name}]}` rewrites the member's outgoing edges: people
edges as given (locate refused cross-family per 2.3), one `message` edge per number (creating the
external), edges to externals not listed removed; then `devices.smsContacts` for each of the
member's devices is re-derived as the first 8 approved numbers by name and `cfg.sms` pushed
(existing code), and the book bumped and nudged. `PUT /api/devices/{id}/sms-contacts` returns 405
with a message pointing at the People page; GET stays. `GET/POST/PATCH /api/family/contacts`:
externals that hold an edge with any member of the family, `{uid, alias, phone, displayName,
approvedFor: [uid…]}`; POST creates one without edges; PATCH renames. `POST /api/conversations/
{alias}/messages`: an `alias` that parses as a phone number (`+` or digits, normalised with
`phonenumbers` as already used for SMS backends) resolves to the external alias; the external is
created on the fly only when the sender's outbound rule for externals is `any`, otherwise 404
`unknown_alias`.
**Verify:** tests green: approved PUT creates the external and edge, derives `smsContacts`
(capped at 8) and pushes `cfg.sms`; removing a number removes the edge and the device contact;
message to `+1 555 123 4567` from an `any_sms` member creates the external and routes it by SMS;
same from a `people_sms` member without an edge → 404; `/contacts` lists only the family's
externals; the book carries `t:"sms"` entries for approved numbers.

### 3.3 Twilio webhook: family resolution, hold or deliver — backend-dev

**Read:** §2 last paragraph (unrecognised X), §4 Webhooks, §1 decision 10; `relay/app/routers/
webhooks.py:215-272`, `relay/app/backends/resolve.py:28-54`, `relay/app/notify/sms.py:79-113`,
`relay/app/backends/sms_twilio.py:118-133`, `relay/tests/test_sms_twilio.py`.
**Files:** `relay/app/routers/webhooks.py`, `relay/app/backends/resolve.py`, `relay/app/backends/
sms_twilio.py`, `relay/app/config.py`, `relay/tests/test_sms_twilio.py`.
**Do:** resolve the family from the webhook's `To`: the family whose `smsNumber` matches, else the
deployment number → no family. Outbound SMS to an external from a member uses the member's family
`smsNumber` when set, else `TWILIO_FROM_NUMBER`. Known `From` (in `phoneIndex`): unchanged, except
the recipient chosen by `resolve_reply` is now gated by the recipient's inbound policy for externals
(`approved` → edge required; `any` → deliver). Unknown `From`: if the family's `blockedNumbers`
contains it → drop; determine the target member: the `@alias` prefix if present, else the single
family member whose inbound rule for externals is `any`, else none; with a target whose rule is
`any` → create the external (3.2), route, and record an alert of kind `sms_unknown` with `status
handled` (4.1 adds the alert writer; in this task write the alert doc directly with the §3 fields);
with a target whose rule is `approved`, or no target but a family → write an `sms_unknown` alert
with `heldBody` and `status open`, deliver nothing, reply nothing; no family and no `@alias` → log
and drop as today (`webhooks.py:236-244`).
**Verify:** `pytest tests/test_sms_twilio.py` green: `To` matching family A's number resolves A;
unknown sender to a family with one `any_sms` member is delivered and an alert with `heldBody null`
is written; unknown sender to a family with only `people` members writes an open alert with the
body held and sends nothing; blocked number dropped; known sender to a `people_sms` recipient
without an edge is refused with the hint SMS.

### 3.4 Web: policy and approved editors in the member drawer — web-dev

**Read:** §2 tables (labels and one-line descriptions), §5.4 People sections 2-3; `web/components/
MemberDrawer.tsx` (from 1.9), `web/app/devices/[id]/DevicePageClient.tsx:148,238` (sms-contacts
editor), `web/lib/smsContacts.ts` (validators to reuse for phone input).
**Files:** `web/components/MemberDrawer.tsx`, new `web/components/PolicyPicker.tsx`, new
`web/components/ApprovedEditor.tsx`, `web/app/family/people/page.tsx`, `web/app/devices/[id]/
DevicePageClient.tsx`, `web/lib/types.ts`, new `web/lib/policy.ts`.
**Do:** `policy.ts`: the two code lists with `label` and `description` strings from §2 (pure module,
testable later). `PolicyPicker`: two MUI `RadioGroup`s (Outbound / Inbound) → `PATCH
/api/family/members/{uid} {policy}`. `ApprovedEditor`: People = checkbox rows for family members and
directory peers with a Locate switch disabled (tooltip "Location stays within a family") when
`familyId` differs; Numbers = rows of `{name, phone}` with add (E.164 validation) and remove, note
"The pager itself lists at most 8 numbers." Save → `PUT /api/family/members/{uid}/approved`.
People table gains policy chips "Out: {label} · In: {label}". `/devices/[id]`: the SMS-contacts
editor becomes a read-only list with the text "Managed from People → @{alias} → Approved numbers".
**Verify:** lint/build/tsc clean. Manual: change a member to People + Numbers, add a number, save;
`devices/{id}.smsContacts` shows it in the emulator UI and the pager receives `cfg.sms` in the
relay log (or the Python test pager); cross-family Locate switch is disabled; the device page
shows the list read-only.

### 3.5 Web: New chat dialog — web-dev (parallel with 3.4)

**Read:** §5.2 "New chat"; `web/app/chat/page.tsx:171-188` (the free-text alias box),
`web/app/chat/[alias]/ThreadPageClient.tsx:478-494,553-557`, `web/lib/directory.tsx` (1.9).
**Files:** new `web/components/NewChatDialog.tsx`, `web/app/chat/page.tsx`, `web/app/chat/[alias]/
ThreadPageClient.tsx`, `web/lib/api.ts` (surface `reason`/`message` from 403 bodies).
**Do:** dialog with one `Autocomplete` field "Name, @alias or phone number"; options from
`useDirectory()` grouped Family / People / Numbers; a free value that parses as a phone number
shows the option "Text +1 555 123 4567" and navigates to `/chat/{digits}`; an alias navigates to
`/chat/{alias}`. Replace the free-text box with a "New chat" button. Thread: a 403 on send renders
an inline `Alert` with the server's `message`, and the empty-thread text becomes "No conversation
with @x yet" with the composer still shown.
**Verify:** lint/build/tsc clean. Manual: member on `people` types an unrelated alias, sends, sees
"Your family admin has limited who you can message."; admin types a phone number, sends, the
relay log shows a Twilio send (or queued without `TWILIO_BASE_URL`); the SMS conversation then
appears on `/chat` with the phone icon.

### 3.6 Web: family Contacts page — web-dev (parallel with 3.4)

**Read:** §5.4 Contacts; `web/app/family/people/page.tsx` (1.9) for table conventions.
**Files:** new `web/app/family/contacts/page.tsx`.
**Do:** table from `GET /api/family/contacts` (name, number, approved for which members as chips,
last message from the conversation list where available); rename dialog → `PATCH
/api/family/contacts/{uid}`; add dialog → `POST /api/family/contacts {phone, name}`; a second
read-only section "Linked families" from `allow where familyIds array-contains fam` whose ends
differ, with the text "Ask your superadmin to link a person from another family."
`requireRole: 'admin'`.
**Verify:** lint/build/tsc clean. Manual: an external created through 3.4 appears; renaming it
changes the name shown on `/chat`; a cross-family edge created by super on `/admin/allowlist`
appears under Linked families.

---

## 4. Alerts

### 4.1 Alerts store, writers, retention — backend-dev

**Read:** §6 "Alert creation", §3 (`alerts` fields), §9 item 6; `relay/app/store/contacts.py:
55-70,170-185` (contactRequests write), `relay/app/routing.py` (DM conversation lazy create),
`relay/app/jobs.py` (sweep), `relay/tests/test_contacts.py`, `relay/tests/test_routing.py`.
**Files:** new `relay/app/alerts.py`, new `relay/app/store/alerts.py`, `relay/app/routing.py`,
`relay/app/store/contacts.py`, `relay/app/routers/webhooks.py` (replace 3.3's direct writes),
`relay/app/jobs.py`, `relay/app/routers/family.py`, new `relay/tests/test_alerts.py`.
**Do:** `store/alerts.py`: `create`, `list(family_id, status)`, `decide(id, status, by)`.
`alerts.py`: `new_conversation(sender, recipient, conv_key)` called from `routing.send` when it
creates a DM `conversations` doc, the sender's `policy.out == 'open'` and no `message` edge
sender→recipient exists; `sms_unknown(family_id, phone, target_uid|None, body, held: bool)` used by
3.3; `contact_request(request)` called where `contactRequests` are stored, with `contactRequestKey`.
Family router: `GET /api/family/alerts?status=open|all`; `POST /api/family/alerts/{id}/approve`
(`sms_unknown`: `{name, forAlias}` → external + edge via 3.2's path, then `routing.send` the
`heldBody` from the external; `new_conversation`: add the edge; `contact_request`: today's approve
body), `/block` (`families.blockedNumbers` add, status handled), `/dismiss`. Retention sweep deletes
handled/dismissed alerts older than 90 days (`settings/retention.alertsDays`, default 90).
**Verify:** `pytest tests/test_alerts.py tests/test_routing.py tests/test_contacts.py` green: an
`open` member's first DM to a stranger writes one `new_conversation` alert and a second message
none; approving an `sms_unknown` creates the external, the edge and delivers the held body;
block adds the number and a later SMS from it is dropped; a `contact_req` writes an alert;
sweep removes old handled alerts only.

### 4.2 Alert push — backend-dev (parallel with 4.1)

**Read:** §6 "Push"; `relay/app/backends/webapp.py:67-135`, `relay/app/backends/fcm.py`,
`relay/app/store/push_tokens.py`, `relay/tests/test_webapp.py`.
**Files:** `relay/app/backends/webapp.py`, `relay/app/alerts.py`, `relay/app/routers/me.py`,
`relay/tests/test_webapp.py`.
**Do:** `push_alert(family_id, alert)`: for every user with `familyId == family_id and role ==
'admin'` (and every `super`? no: super is not pushed) whose `notify.alerts` is not `False`, send a
data message `{kind: "alert", alertKind, id, title, body, url: "/family/alerts"}` to each of their
`pushTokens`; titles: "New chat: @kid ↔ @peer", "Text from an unknown number for @kid", "Contact
request from @kid's pager"; body ≤120 characters. `alerts.py` calls it after each `create`. `PATCH
/api/me {notify: {alerts}}` added.
**Verify:** `pytest tests/test_webapp.py` green: fake FCM asserts the exact data map for each kind
and that an admin with `notify.alerts false` gets nothing; a member gets nothing.

### 4.3 Service worker alert arm — web-dev

**Read:** §6 "Push"; `web/sw/firebase-messaging-sw.template.js:33-96`.
**Files:** `web/sw/firebase-messaging-sw.template.js`.
**Do:** in `onBackgroundMessage` add the `alert` arm: `showNotification(data.title, {body, tag:
"pager-alert-" + data.id, data: {url: data.url}})`; `notificationclick` unchanged (opens `url`).
**Verify:** `npm run build` regenerates `public/firebase-messaging-sw.js`; manual with
`PUSH_BACKEND=fcm` and a real token: a held SMS in the emulator produces a notification that
opens `/family/alerts`.

### 4.4 Web: Alerts page and badge — web-dev

**Read:** §5.4 Alerts, §5.2 `NotificationWatcher` alerts; `web/app/admin/contacts/page.tsx`
(the approve/reject dialog to move), `web/components/NotificationWatcher.tsx`,
`web/components/AppShell.tsx`.
**Files:** new `web/app/family/alerts/page.tsx`, new `web/components/AlertCard.tsx`,
`web/components/NotificationWatcher.tsx`, `web/components/AppShell.tsx`, `web/lib/types.ts`.
**Do:** `/family/alerts` (`requireRole: 'admin'`): listen to `families/{fam}/alerts where status ==
'open' orderBy ts desc` and, under a "Show handled" toggle, all statuses; `AlertCard` per kind with
the actions from §5.4 calling `/api/family/alerts/{id}/{approve|block|dismiss}`; the
`contact_request` card embeds the approve dialog moved from `/admin/contacts`. `AppShell`: badge on
the Family menu and the Alerts item with the open count. `NotificationWatcher`: for admins, subscribe
to the same open-alerts query and show a browser notification for a newly added doc when the tab is
hidden or the path is not `/family/alerts`.
**Verify:** lint/build/tsc clean. Manual: send an SMS from an unknown number via the Twilio
webhook test client to a family with `people` members → badge increments, card shows the held text,
Approve with a name creates the contact and the message appears in the member's thread; Block then
a second SMS produces nothing; a `contact_req` from the Python test pager shows a card whose Approve
writes the edges.

### 4.5 Web: notifications setting — web-dev (parallel with 4.4)

**Read:** §5.6; `web/app/settings/notifications/page.tsx`.
**Files:** `web/app/settings/notifications/page.tsx`.
**Do:** for `isFamilyAdmin`, a "Family alerts" switch bound to `me.notify.alerts` via `PATCH
/api/me {notify: {alerts}}`.
**Verify:** lint/build/tsc clean. Manual: toggling off then triggering an alert sends no push
(relay log shows the skip).

### 4.6 Per-family SMS number in the UI — web-dev (parallel with 4.4)

**Read:** §1 decision 10, §5.5 Families; `web/app/admin/families/page.tsx` (1.8).
**Files:** `web/app/admin/families/page.tsx`.
**Do:** the family detail shows `smsNumber` with an edit field (E.164) and the help text "Inbound
texts to this number are routed to this family. Leave empty to use the shared number."
**Verify:** lint/build/tsc clean. Manual: set a number, the Twilio webhook test with that `To`
resolves the family in the relay log.

---

## 5. Cleanup

### 5.1 Remove legacy admin paths — backend-dev

**Read:** §8 phase 5; `relay/app/auth.py` (the `require_admin` alias from 1.2), `relay/app/routers/
admin.py` (contacts routes :722-820), `relay/app/main.py`.
**Files:** `relay/app/auth.py`, `relay/app/routers/admin.py`, `relay/app/routers/devices.py`,
`relay/tests/test_admin.py`, `relay/tests/test_contacts.py`.
**Do:** delete `require_admin` and every remaining caller (use `require_super`); delete
`/api/admin/contacts/*` (tests move to the family alerts routes); `_set_admin_claim` must no
longer exist.
**Verify:** `grep -rn "require_admin\b\|_set_admin_claim\|token.get(\"admin\"" relay/app` empty;
full `pytest` green.

### 5.2 Rules and indexes deployed — infra-dev

**Read:** `relay/firestore.rules` (1.4/2.2), `relay/firestore.indexes.json` (2.1),
`infra/README.md` (1.10), `.github/workflows/deploy.yml`.
**Files:** `infra/README.md` if a command differs from 1.10.
**Do:** confirm the wipe from 1.10 has been done in the project; `firebase deploy --only
firestore:rules,firestore:indexes` dry-run; confirm CI deploys rules and indexes on push to `main`.
**Verify:** dry-run clean; the `familyIds` queries run with no "index required" error against the
deployed project.

### 5.3 Delete `/admin/contacts` and the web checklist — web-dev

**Read:** `web/app/admin/contacts/page.tsx`, `web/README.md:146-258`.
**Files:** delete `web/app/admin/contacts/page.tsx`; `web/README.md`.
**Do:** remove the page; extend the manual checklist with the family-admin, super and member
scenarios used in tasks 1.7-1.9, 2.4-2.6, 3.4-3.6, 4.4 (one line each).
**Verify:** lint/build/tsc clean; `out/admin/contacts` no longer exists.

### 5.4 Docs — docs-writer

**Read:** `docs/SERVER_PLAN.md` §1 (line 43), §3, §5.1, §5.3-5.5, §7.2-7.6; `docs/ROADMAP.md`
(Web and Relay sections); `docs/README.md`; `docs/FAMILIES_DESIGN.md`.
**Files:** `docs/SERVER_PLAN.md`, `docs/ROADMAP.md`, `docs/README.md`, `docs/FAMILIES_DESIGN.md`
(status line only).
**Do:** update §1's one-household statement, add the §3 fields and collections from
`FAMILIES_DESIGN.md` §3, the routes from §4 to §5.1, the roles to §5.3, the policy layer to §5.4,
the family-admin surface to §5.5, and the routes/nav of §5 to §7.2; add `FAMILIES_DESIGN.md` to
`docs/README.md`'s reference table; move the §9 flagged items that stay open to `ROADMAP.md`
"Decisions waiting on the owner"; add a `**Status:**` line at the top of `FAMILIES_DESIGN.md`.
**Verify:** links resolve; no section numbers in `SERVER_PLAN.md` were renumbered.

---

## Definition of done

- Manual checklist (5.3) passed once in each role: super, family admin, member, across two
  families in the emulator, covering: nav per role; People/Devices/Contacts/Alerts scoped; a
  member's DM with another family visible to both families' admins and to no third family; an SMS
  conversation visible only to the member's family; location never crossing a family; every policy
  preset refusing and admitting as §2 says; a chat started by phone number; an unrecognised SMS
  held, approved and delivered; alert push received.
- Project wiped and re-bootstrapped per 1.10; `/admin/families` shows "Home".
- `GROUP_CHAT_DESIGN.md` join semantics superseded (documented in 5.4); `/admin/contacts` and
  `require_admin` gone.
- The pager's book and `cfg.sms` unchanged on the wire; `docs/PROTOCOL.md` untouched.
- All relay tests green (`cd relay && .venv/bin/pytest`), rules tests green against the emulator,
  web lint/tsc/build clean, docs updated, committed and pushed to `main`.
