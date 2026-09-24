# Group chat — basic design

**Status:** G1-G8 implemented and committed (`d849e61` conversations store/routing/pager/push,
`ec8749f` firmware `sndr` parse, `ff25132` web UI). G9 (end-to-end acceptance) was passed on the
wire on 24 Sep 2026 (`build/bench-logs/group-acceptance.md`); the real pager's author-line
rendering is still pending the owner's own glass check. Companion to `docs/SERVER_PLAN.md` (§3 data
model, §5.2 routing) and subordinate to `docs/PROTOCOL.md` for anything the device sees.

## Decisions (22 Sep 2026) — settled, do not re-open

1. **The author of a group message travels in its own optional field, `sndr`, not as a `body`
   prefix.** The body keeps all 160 code points. This makes v0.3 a device-facing protocol change:
   `sndr` is now specified in `docs/PROTOCOL.md` §3.1 (field table), §3.3 (byte budget) and §10
   (CBOR key 51). See §4 below.
2. **Group creation is admin-only, and creating or joining a group auto-creates `allow` edges in
   both directions between every member pair**, so the send-time allow-list gate never
   partial-delivers. What happens when an admin later deletes an edge stays open (§8.6).
3. **No history on join; per-copy visibility stands.** A member reads only the copies they were
   party to.

**Scope line (owner's words: "design a basic group chat feature"):** a conversation with more than
two members, every message fanned out to every other member, sender identified by alias, per-member
unread. **Out of scope, deliberately: admin roles inside a group, invite links, member-removal
notifications, read receipts beyond today's per-delivery `read`, typing indicators, threads,
reactions.** Group `/locate` is also out of scope (it stays a DM/pager-owner operation).

---

## 0. Today's model, established from the code

- **A conversation is a UID *pair*, computed, not stored.** `conv_key(a, b)` is
  `"_".join(sorted([a, b]))` (`relay/app/store/messages.py:107-108`); `create_message` derives both
  `convKey` and `uids` from `(sender_uid, recipient_uid)` (`:146-147`) and nothing lets a caller
  supply them. `docs/SERVER_PLAN.md:190`: "`convKey` = the two UIDs sorted and joined with `_`".
  There is no `kind`, no membership list, no name (`relay/app/store/messages.py:97-105`,
  `web/lib/types.ts:143-149`).
- **Who may talk to whom is not pager-vs-web, it is the allow-list.** `routing.send()` resolves an
  alias to a single uid (`relay/app/routing.py:180-192`) and gates on
  `allow/{from}_{to}.message` (`:152`, `docs/SERVER_PLAN.md:517-520`). So *two web users can
  already DM each other*, and *one web user can DM several pagers* — each such pair is simply its
  own conversation. Nothing today is pair-restricted except the shape of `convKey`.
- **One message document per (sender, recipient) pair**, with `uids` = that pair and a `deliveries`
  map keyed by the recipient's backend doc id (`relay/app/store/messages.py:185-204`,
  `docs/SERVER_PLAN.md:175-184`). A device up-message with no `to` already fans out into N
  documents sharing one `wireId` (`docs/SERVER_PLAN.md:196-199`). **Fan-out therefore already
  exists**; what does not exist is a conversation that several such documents belong to.
- **`seq` is global, not per conversation**: `settings/meta.seqCounter`, read-then-incremented
  inside the message transaction (`relay/app/store/messages.py:177-183`). Ordering is by `seq`
  within a `convKey`.
- **Reaching the pager:** `PagerBackend.deliver()` builds the §3.2 down envelope with
  `from` = the **sender's alias** (`relay/app/backends/pager.py:88-94,113-116`) and `body` = the
  message body; `id` is the Firestore message id. `body` caps are 160 code points / 320 UTF-8 bytes
  (`docs/PROTOCOL.md:129`, `relay/app/wire.py:26-27`); a down text message is ~99 bytes typical,
  420 bytes at the §3.3 per-field ceiling, 449 signed — the 640-byte limit has ≥166 bytes of
  headroom (`docs/PROTOCOL.md:248-308`). (The "193" figure in the brief is `docs/DEVICE_PLAN.md:186`'s
  `/loc` row, not a `/down` budget.)
- **A pager reply is addressed by alias.** `/up` may carry `to` (`docs/PROTOCOL.md:131`); ingest
  passes it straight through as `recipient_alias` (`relay/app/ingest.py:522`), and an unknown or
  disallowed `to` produces the §4.2 `system` reply (`:530-535`). With no `to`, the relay uses
  `devices/{d}.defaultToUid`, else broadcasts to every allowed recipient
  (`relay/app/routing.py:194-201`).
- **The device's model of "who am I talking to" is a flat alias list.** A down message carries only
  `from`, never `to` (`firmware/main/msg.h:46-49`); an up message carries `to` (`:72,113-114`,
  empty = default). The address book is a projection of the owner's allow-list
  (`relay/app/devcfg.py:197-211`), ≤10 contacts, each `{a: alias, n: name, t: web|sms|chat}`
  (`firmware/main/book.h:65-77`). The chat screen is still **one merged thread**; the pick screen
  navigates but does not yet set `to` (`firmware/main/scr_pick.c:22-32`) — a `to`-aware per-peer
  view exists in `msg.c` but is not exposed.
- **Unread** is `conversations/{convKey}.unread[uid]`, incremented per created message
  (`relay/app/store/messages.py:206-217`) and zeroed only by
  `POST /api/conversations/{alias}/messages/{id}/read` (`relay/app/routers/conversations.py:87-121`,
  which checks `msg.convKey == conv_key(me, aliasUid)` at `:106-108`).
- **Client reads:** thread = `messages` where `convKey == k` **and** `uids array-contains me`,
  `orderBy(seq desc)` (`web/app/chat/[alias]/ThreadPageClient.tsx:212,221-243`); list =
  `conversations` where `uids array-contains me` (`web/app/chat/page.tsx:60-67`, which derives
  `peerUid` by "the uid that isn't me"). Rules mirror exactly that
  (`relay/firestore.rules:49-55`), and the two indexes already exist
  (`relay/firestore.indexes.json`: `messages(convKey, seq)` and
  `messages(uids CONTAINS, convKey, seq DESC)`).

---

## 1. Goal and non-goals

Goal: a named conversation with 3+ members where every member (web or pager) receives every
message, sees who sent it, and has their own unread count. Non-goals: the list in the scope line
above.

## 2. Data model (minimal change)

**`conversations/{convKey}`** — add three fields, change nothing existing:
`kind: 'dm' | 'group'` (**absent MUST be read as `dm`**), `name: string` (display name, ≤48 bytes),
`alias: string` (the group's wire/URL alias, same regex as a user alias), `createdBy: uid`.
`uids` **becomes the member list** (length ≥ 2) — that is the whole point of choosing this field:
`firestore.rules:53-55` and both client queries already key off `uids array-contains me`, so
membership read-scoping comes for free. For a group, `convKey` is a minted id (`g_` + 8 hex,
`app/ids.py`), **not** derived from uids; DMs keep the derived pair key. A `kind` flag alone is not
enough (a pair key cannot express 3 members) and a 3-uid sorted key is worse (it changes on every
membership edit, orphaning history).

**The group alias lives in the one flat alias namespace**: `aliases/{alias} = {convKey}` (no `uid`
field), created with `transaction.create()` for uniqueness exactly as users do
(`relay/app/store/users.py:73-90`). This is load-bearing: the device's `to` field is a flat alias
namespace, so a group alias must be unable to collide with a user alias. Free consequence:
`users_store.get_uid_for_alias()` returns `None` for a group (`:121-127` reads the `uid` field), so
`POST /api/conversations/{alias}/locate` 404s on a group without any new check.

**`messages/{id}`** — two added fields, `uids` and `recipientUid` unchanged:
- `groupMsgId: string | null` — one id shared by every copy of one logical group message. The web
  client dedupes on it (the sender holds N-1 copies of their own message); push uses it as a
  collapse key.
- `senderAlias: string | null` — denormalised. Required, not optional: the pager page and the push
  payload both need it, and both are built in code paths that would otherwise do a `users/{uid}`
  read per delivery (`relay/app/backends/pager.py:113-116`), and the push contract in
  `docs/V03_PLAN.md` §3a names `senderAlias` explicitly. Keep `senderUid` as the authority.
- `uids` stays the **pair** `[senderUid, recipientUid]`. This is the decision that makes the
  feature cheap: **no index change, no rule change**, and a non-member can never read any copy.
- **`seq` is allocated once per logical group message** and written identically to every copy, so
  every member's `orderBy(seq)` agrees. Per-copy allocation would let two concurrent senders
  interleave (A's copies 101/103, B's 102/104) and different members would see different orders.

**Indexes:** none added. **Rules:** none changed; `relay/firestore.rules:49-55` already says
"party to this document" / "member of this conversation". `test_rules.py` gains the negative cases.

**Web types** (`web/lib/types.ts`): `ConversationDoc.uids: [string, string]` → `string[]`, plus
`kind?`, `name?`, `alias?`; `MessageDoc` gains `groupMsgId` and `senderAlias`;
`MessageDoc.uids` stays a 2-tuple.

## 3. Relay routing / fan-out

New store module `relay/app/store/conversations.py` (group CRUD); `routing.send()` gains one branch
before recipient resolution: if `recipient_alias` resolves to a **group** conversation, recipients =
`members − sender`, and the send carries `conv_key`/`group_msg_id`/`seq` down into
`create_message()`, which gains those three keyword arguments (defaults preserve today's derived
behaviour exactly).

Data flow for one group message:
1. `POST /api/conversations/{alias}/messages` (web) or `/up` with `to: <group alias>` (pager) —
   **unchanged endpoints and wire**.
2. Resolve alias → group; assert sender ∈ members (403 otherwise).
3. **T1** — `allocate_seq()`: one transaction on `settings/meta.seqCounter`. Also mint
   `groupMsgId`; for a web-originated send set `wire_id = groupMsgId` so the fan-out is idempotent
   under retry (device sends already carry a `wireId`).
4. **T2…Tn** — per recipient, today's `create_message` transaction unchanged in shape:
   `create(wireIds/{wireId}_{recipientUid})` (dedup, per recipient — so a partial fan-out re-run
   creates only the missing copies), write the message copy, increment
   `conversations/{convKey}.unread[recipientUid]`, set `lastPreview`/`lastMessageAt`.
5. Inline delivery per copy through the existing backend registry — pager → broker REST publish,
   webapp → Firestore listener + FCM. Allow-list gate (`allow_store.is_message_allowed`) stays on
   every recipient: **membership is not a substitute for an allow edge.** To keep that from
   silently partial-delivering, group creation/join refuses unless an edge exists and (admin path)
   creates the missing edges both ways; a missing edge at send time is a `SECURITY` log + that one
   recipient dropped, as today.

*The likely real case — one pager, several web users (family + kid's pager):* the kid's copy is an
ordinary pager delivery, so unacked-page republish on an online edge, `/internal/tick` retries,
`pendingDeviceIds`, the 24 h expiry and the `smsLog` audit path all work untouched
(`relay/app/routing.py:203-251`). *Two pagers:* two copies, two QoS-1 pages, two independent ack
streams; neither pager receives an echo of its own message (the sender is excluded from the
recipient set, and `_create_and_deliver`'s self-loop guard at `:281-294` stays a no-op).

**Group admin API** (all `require_user`, creation admin-only in v1):
`POST /api/conversations` `{name, alias, memberUids}` → 201 `{convKey, alias}` (409 on alias
collision); `POST /api/conversations/{alias}/members` `{uid}`; `DELETE
/api/conversations/{alias}/members/me` (leave). `mark_read` gains a group branch at
`relay/app/routers/conversations.py:106-108` (alias → group convKey, caller ∈ members).

**Failure modes / recovery:** crash between T1 and the last copy → some members miss the message;
re-POST (web, same `groupMsgId`) or QoS-1 redelivery (pager) repairs it, creating only missing
copies. Pager publish failure → delivery stays `queued`, `tick` retries, 5 attempts → `failed`
(existing). Alias collision → 409, nothing written. Leave-while-sending → the leaver may get one
last copy; harmless. Nothing here needs a long-lived process: every step is request- or
tick-driven.

**What to measure:** copies written per logical message; pages/day per pager against
`docs/PROTOCOL.md` §7.3's data budget; p95 latency of `POST /api/conversations/{alias}/messages` as
a function of member count (fan-out is sequential and in-request — this is why group size is
capped); `queued`→`sent` conversion for pager deliveries in groups; rule-denial counts;
`unread` drift (a non-zero count with no unread copy).

## 4. Device-facing protocol impact

**Recommendation: the pager needs a conversation name and a sender alias per message, and nothing
else — no member list, ever.** It also never fans out: it publishes one `/up` with
`to: <group alias>` and the relay does the rest.

**The field (decision 1).** `sndr`, a text string, same regex and ≤16-char rule as `from`; it is the
**author's alias** and it appears **only** on a `/down` `msg` belonging to a group conversation.
`from` on that page is **the group's alias** — the thread the device shows and the alias its reply
addresses. `sndr` is absent on every DM page, so today's one-to-one pages stay byte-identical, and
absent means "`from` is the author", i.e. today's rule. It is never sent on `/up`: the device replies
to a conversation and the relay already knows which member's device published. CBOR key **51**
(`docs/PROTOCOL.md` §10, the next free integer after `link=50`); in JSON the name `sndr`. Now
specified in `docs/PROTOCOL.md` §3.1, §3.3 and §10, with the alias-namespace rule kept as §12 item 9.

**Byte budget, at §3.3's per-field ceiling.** `"sndr":"<=16>",` = 7 + 18 + 1 = **26 bytes** in JSON:
the v1 down `msg` line goes 420 → **446** (text fields only) and the signed down `msg` 449 → **475**;
the largest *achievable* signed JSON group page goes ≈479 → **≈505**, leaving **≥135 bytes** under
the 640-byte limit. In CBOR the cost is +19 bytes at a 16-character alias (down `msg` ≈75 → ≈94) and
+6 at a typical 3-character alias, because the key is a two-byte integer head, not a quoted name.
The 640-byte limit does not move and no other payload grows.

**What the pager renders.** `from` = the group alias is the thread identity: the address-book row and
the chat header. `sndr` is the per-message author line inside the thread. The device needs nothing
else — **no member list, ever** — and it never fans out: one `/up` with `to: <group alias>`, relay
does the rest. The group appears in the book as a contact `{a: <group alias>, n: <group name>,
t: "grp"}` (amended 23 Sep, firmware review: the pick screen labels a row from `book_contact_t.type`
verbatim, so a group needs its own type rather than reusing `web` — `t` gains a fourth value, `grp`,
docs/PROTOCOL.md §3.1's `c[].t` row and §10's `c[]` key list) (`relay/app/devcfg.py:197-211` gains
groups; the ≤10 cap and `_assert_within_envelope_limit` at `:240-272` already protect the envelope).

**What today's firmware does with a page carrying `sndr`: ignores it, verified.** The `/down` CBOR
parse loop's `default:` arm calls `cbor_r_skip()` on any key it does not know
(`firmware/main/msg.c:1492-1498`), and `cbor_r_skip()` handles a `tstr` by consuming its length
correctly, with bounds checks and position restore on failure (`firmware/main/cbor.c:418-469`).
Unknown keys are counted against the map's declared pair count and skipped, not rejected, so an
un-updated pager renders a group page as a message from the group alias with no author line, acks it
normally, and replies to the right conversation. HMAC verification is unaffected: the tag covers the
serialised bytes with the `sig` pair trimmed (§14.3), so an extra pair is simply signed with the
rest. **Consequence for rollout: emitting `sndr` is safe before the firmware is updated** — it is a
graceful-degradation change, not a flag-day one (see §8.7 for the ordering that follows).

**Firmware work (G7, firmware-architect reviews before firmware-dev builds):** parse key 51 into a
new `sndr[17]` field on the stored message, render it as the author line in `scr_chat.c`, and label
the group row in `scr_pick.c`. Host tests for a page with and without `sndr`. Not designed here —
no pixel layout in this document.

## 5. Web UI

- **Create group** (admin): dialog on `/chat` — name, alias, checkbox list of existing contacts
  from the directory; `POST /api/conversations`.
- **List page** (`web/app/chat/page.tsx`): the `peerUid`-derivation at `:66` must branch on
  `kind === 'group'` → render `name` and route to `/chat/{alias}`; DM rows unchanged.
- **Thread view** (`ThreadPageClient.tsx`): resolve `alias` → conversation from the existing
  `conversations` listener (no new query, no new index), use its doc id as `convKey`, keep the
  existing `where('convKey','==',k) + where('uids','array-contains',me)` pair, then **dedupe by
  `groupMsgId`** and render a sender-alias label above each bubble when
  `kind === 'group'` (DMs keep today's look). An auto-scroll change is landing in this file in
  parallel: confine group edits to the `convKey` resolution, the dedupe step and the bubble header
  — do not touch the scroll effect or the listener's ordering.
- **Leave group**: menu item → `DELETE /api/conversations/{alias}/members/me`.

## 6. Push

Maps onto `docs/V03_PLAN.md` §3a's contract without extending it:
`kind: "message"`, `convKey`, `id`, `senderUid`, `senderAlias`, `title` = **group name** (DM: sender
display name), `body` = preview, `url` = `/chat/{group alias}`. `WebappBackend.deliver()`
(`relay/app/backends/webapp.py:66-82`) currently sends only `convKey`/`id`/`senderUid`/`body`;
§3a's task already has to add `kind`/`senderAlias`/`title`/`url`, and groups only change how
`title` and `url` are computed. One push per member copy, collapse key = `groupMsgId`.

## 7. Migration

Nothing to backfill. Existing DM conversation and message documents are valid as-is: `kind` absent
is read as `dm`, `uids` is already the member list for a pair, `groupMsgId`/`senderAlias` absent
means "not a group copy" / "look up the sender". `conv_key()` and the derived-key path stay the
default in `create_message`, so every DM code path is byte-identical. The only forced edit is
widening `ConversationDoc.uids` to `string[]` in `web/lib/types.ts` (a type-level change; the DM
call sites already only ever `.find()` over it).

## 8. Risks and open questions

1. **Page size for group pages.** `sndr` costs 26 bytes in JSON (19 in CBOR at a maximum-length
   alias), taking the largest achievable signed JSON group page to ≈505 of 640 bytes — ≥135 bytes of
   headroom, but the tightest shape this contract has. Keep group aliases short; a future additive
   `/down` field has that much less room. `body` itself is untouched: all 160 code points survive.
2. **Several conversations paging one pager.** The device shows one merged thread
   (`scr_pick.c:22-32`); a group plus two DMs will interleave on screen, and the composer's
   `to`-targeting is still the `@alias` word. This is the weakest part of the device UX and it is a
   pre-existing gap, not one groups create — but groups make it visible.
3. **Fan-out cost.** N-1 QoS-1 pages and N-1 message documents per message; a group with two pagers
   doubles both against `docs/PROTOCOL.md` §7.3's per-device budget. Mitigation: cap membership
   (proposal: 8 members, at most 2 pager members) and cap groups per pager so the 10-contact book
   never overflows.
4. **Per-copy visibility is a feature *and* a limitation.** A member cannot read copies addressed to
   others — so a new member gets no history, and a "who else has read it" view is impossible without
   a schema change. Confirm that is acceptable for "basic".
5. **Rules audit.** With `uids` = members on the conversation and `uids` = pair on the message, no
   non-member read path exists that I can find (`firestore.rules:49-55`); the one thing to pin in
   tests is that a **former** member keeps read access to copies they were party to (intended) and
   loses the conversation summary (which will freeze their unread badge — cosmetic).
6. **Allow-list vs membership** double gate (**still open**, per decision 2): edges are auto-created
   both ways at creation/join, but if an admin later deletes one, the group silently stops delivering
   to that member. Surface it as a `degraded` flag on the conversation, or refuse the edge deletion
   while a shared group exists? Not resolved now.
7. **Rollout ordering.** `sndr` is safe to emit before any firmware carries it — the parse loop skips
   unknown keys (`firmware/main/msg.c:1492-1498`), so an un-updated pager shows the group thread with
   no author line and still replies correctly. CBOR is the only down path this firmware has — JSON
   down-ingest was removed and cJSON dropped from the build (`firmware/main/msg.h:322-327`) — so the
   verification above covers every page a real device receives. (If `devices/{d}.wire` ever said
   `json`, that device would already drop *all* pages; pre-existing, unrelated to groups.) No
   firmware-version gate is therefore needed, and none exists today (`/status.fw` is stored but
   nothing routes on it) — worth revisiting only if the fleet grows past a couple of devices.

---

## 9. Tasks

**Build order: G1 → G2 → G3 → G5 → G6 → G7 → G4 → G8 → G9.** G3 (the relay emitting `sndr`) may
precede G7 (the firmware rendering it) because an un-updated pager skips the unknown key
(§4, §8.7) — the group is usable from the web and the pager the day G3 lands, and G7 only adds the
author line. G5 (rules tests) goes before G6 so the web work is not the thing that discovers a rule
gap. G4 waits on `docs/V03_PLAN.md` §3a landing, not on anything here. G7 needs
firmware-architect sign-off on the `sndr` parse/render shape **before** firmware-dev starts.

### G1 Conversations store + group model — backend-dev
**Read:** `relay/app/store/messages.py` (all), `relay/app/store/users.py:59-113`,
`docs/SERVER_PLAN.md:175-200`, this document §2.
**Files:** new `relay/app/store/conversations.py`, `relay/app/store/messages.py`,
new `relay/tests/test_conversations_store.py`.
**Do:** `create_group(name, alias, member_uids, created_by)` (one transaction: conversation doc +
`aliases/{alias} = {convKey}` via `transaction.create`), `get_by_alias`, `add_member`,
`remove_member`, `is_member`. Add `allocate_seq()` and the `conv_key`/`uids`/`seq`/`group_msg_id`/
`sender_alias` keyword arguments to `create_message` with defaults that preserve today's behaviour.
**Verify:** new tests: alias collision → `AlreadyExists`; members read back sorted; DM
`create_message` output unchanged field-for-field; two copies sharing one `seq` and `groupMsgId`;
existing `relay/.venv/bin/pytest` suite green.

### G2 Routing fan-out + API — backend-dev
**Read:** `relay/app/routing.py` (docstring + `_candidate_recipients`, `_create_and_deliver`),
`relay/app/routers/conversations.py`, `relay/app/ingest.py:500-545`, this document §3.
**Files:** `relay/app/routing.py`, `relay/app/routers/conversations.py`,
`relay/tests/test_routing.py`, `relay/tests/test_conversations.py`.
**Do:** group branch in recipient resolution (members − sender, allow gate per recipient, one
`allocate_seq` + shared `groupMsgId`, `wire_id = groupMsgId` for web-originated sends);
`POST /api/conversations` (admin-only, creates missing allow edges both ways),
`POST /api/conversations/{alias}/members`, `DELETE …/members/me`; group branch in `mark_read`.
**Verify:** tests for a 3-member group (2 copies, one `seq`, per-member unread), a group with one
pager (a page is published) and with two pagers (two pages, no self-echo), a non-member sender →
403, a re-POST with the same `groupMsgId` creating no duplicates, and a missing allow edge →
`SECURITY` log + that recipient dropped.

### G3 Pager page + book — backend-dev
**Read:** `relay/app/backends/pager.py`, `relay/app/devcfg.py:197-296`, `docs/PROTOCOL.md:121-308`,
this document §4.
**Files:** `relay/app/backends/pager.py`, `relay/app/wire.py` (`build_down_payload`, `:454-480`),
`relay/app/wirecbor.py` (keymap, `:101` — add `"sndr": 51`), `relay/app/devcfg.py`,
`relay/tests/test_wire.py`, `relay/tests/test_routing.py`, `relay/tests/test_devcfg.py`.
**Do:** for a group copy, `from` = the group alias and `sndr` = `msg.senderAlias`; **`body` is
untouched and never prefixed or truncated**. Add `sndr` to the CBOR keymap as key 51 and to the JSON
builder's field order (after `to`, before `ack`). Omit `sndr` entirely on DM pages. List the owner's
groups as book contacts (`t: "grp"`, name = group name) inside the existing ≤10 cap.
**Verify:** DM pages byte-identical to today (assert the exact bytes for one); a group page carries
`sndr` in both encodings; a worst-case group page (16-char `id`, 16-char `from`, 16-char `sndr`,
160-cp/320-byte `body`, signed) asserts ≤640 bytes and matches §3.3's ≈505 figure;
`_assert_within_envelope_limit` still passes with 10 contacts including groups.

### G4 Push payload for groups — backend-dev (after §3a lands)
**Read:** `relay/app/backends/webapp.py`, `docs/V03_PLAN.md` §3a.
**Files:** `relay/app/backends/webapp.py`, `relay/tests/test_routing.py`.
**Do:** `title` = group name and `url` = `/chat/{group alias}` for group copies; `senderAlias`
from the message document; collapse key `groupMsgId`.
**Verify:** fake FCM client asserts the exact data map for a group and a DM.

### G5 Rules tests — backend-dev
**Read:** `relay/firestore.rules`, `relay/tests/test_rules.py`.
**Files:** `relay/tests/test_rules.py` only (no rule change expected — if one turns out to be
needed, stop and report).
**Do:** emulator cases: member reads group conversation + own copies; non-member denied both;
`convKey`-only query denied; former member keeps old copies, loses the summary.
**Verify:** `pytest relay/tests/test_rules.py` green.

### G6 Web: create, list, thread, leave — web-dev
**Read:** `web/lib/types.ts`, `web/app/chat/page.tsx`, `web/app/chat/[alias]/ThreadPageClient.tsx`
(and the in-flight auto-scroll diff), `web/lib/directory.tsx`, this document §5.
**Files:** `web/lib/types.ts`, `web/app/chat/page.tsx`,
`web/app/chat/[alias]/ThreadPageClient.tsx`, new `web/components/NewGroupDialog.tsx`,
`web/lib/directory.tsx`.
**Do:** widen `uids`, add `kind`/`name`/`alias`; group rows on `/chat`; group-aware `convKey`
resolution, `groupMsgId` dedupe and sender-alias bubble header in the thread; create dialog; leave
action. Do not touch the auto-scroll effect.
**Verify:** `cd web && npm run lint && npm run build`; by hand against the emulator: a 3-member
group shows one bubble per message with the right alias, and the sender sees no duplicates.

### G7 Device: parse and render `sndr` — firmware-dev, **firmware-architect reviews the shape first**
**Read:** `docs/PROTOCOL.md` §3.1 (`sndr` row), §3.3, §10 (key 51); `firmware/main/msg.c:1340-1500`
(the down-parse loop and its `default: cbor_r_skip()` arm), `firmware/main/msg.h:100-130,254-262`
(`msg_unread_t`/thread entry fields and the "no `to` on a down message" note),
`firmware/main/cbor.c:418-469`, `firmware/main/scr_chat.c` (row building), `firmware/main/scr_pick.c`,
this document §4.
**Files:** `firmware/main/msg.c`, `firmware/main/msg.h`, `firmware/main/scr_chat.c`,
`firmware/main/scr_pick.c`, `firmware/host/test_cbor.c`, `firmware/host/test_msg.c`,
`firmware/host/Makefile` if a source is added.
**Do:** add `MK_SNDR = 51` and a `case` in the down-parse loop storing it into a new
`sndr[MSG_FROM_MAX]` field (validate as an alias, same length rule as `from`; empty when absent —
absent is normal and MUST NOT be malformed). Carry it through the NVS/RTC record the same way `from`
is carried, minding §9's storage budget (17 bytes per entry — state the new total in the report).
Render it as the author line per message in the chat thread when non-empty; when empty, render
exactly as today. Label a group row in the pick screen from the book entry. No new wire field, no
`/up` change, no ack change.
**Verify:** `make -C firmware/host test` green, including new cases in `test_cbor.c` (an unknown
high-numbered key is skipped, a `tstr` value included) and `test_msg.c` (a page **with** `sndr`
stores and renders the author; a page **without** `sndr` is byte-for-byte the behaviour of today,
including acks; an over-long or non-alias `sndr` is ignored rather than making the page malformed;
a signed page with `sndr` still verifies). `PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build`
clean. No `/dev/cu.*` access in this task; bench check rides along with G9.

### G8 Deploy indexes/rules — infra-dev
**Read:** `relay/firestore.indexes.json`, `relay/firestore.rules`, `infra/README.md`.
**Files:** none expected.
**Do:** confirm no new composite index is required (the `(uids CONTAINS, convKey, seq DESC)` index
already covers the group thread query) and deploy rules if G5 changed them.
**Verify:** `firebase deploy --only firestore:rules` dry-run clean; the group thread query runs
with no "index required" error against the emulator and staging.

### G9 Manual acceptance — owner + backend-dev
Group "family" = owner's web user + a second web user + `test-pager`'s user.
**Do/Verify, in order:** create the group in the web UI → both web users see it on `/chat` and the
pager's book shows `family`; owner sends → the second web user sees the bubble labelled with the
owner's alias, the pager shows the page under the `family` thread with the owner's alias as the
author line (**before G7 lands: same page, no author line — check that too, it is the degradation
path §8.7 relies on**); the pager replies (`wake`, `key \n`, type,
`enter`) with `to: family` → both web users receive it, each with their own unread count, and the
pager gets no echo; open the thread in one web tab → only that user's unread clears; power-cycle
the pager mid-send → the page re-publishes on the online edge and does not double-render.
