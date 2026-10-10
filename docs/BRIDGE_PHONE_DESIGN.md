# Bridge phone: Google Chat, Google Voice, WhatsApp and SIM SMS through a headless Android phone (8 Oct 2026; WhatsApp 9 Oct 2026)

*(owner, 8 Oct 2026 evening: "I want to work on bridges to personal Google Chat and possibly
Google Voice. Both do not have an API that will work." … "android is not a bad option. Can we
build a side loaded app?" … "the phone will never be touched. It's a headless relay. In fact, we'd
put a SIM with SMS capabilities to handle the Twilio case directly and avoid all the A2P
nonsense." … "If a thread has been inactive for a while, I want the pager to be able to message
it. Say send a message to grandma." … "We also need a way to project Google users in the group
chat into the address book." … "This sounds good. Please orchestrate an implementation of this.
Also implement SMS via the phone's native SMS messaging and SMS via Google voice this way.")*

Facts that shaped this (checked 8 Oct 2026): the Google Chat API, including user-authenticated
calls, is Workspace-only (`docs/SERVER_PLAN.md` §10 D4 is now answered: **no** for a personal
account); Google Voice has no API; Android's `NotificationListenerService` sees the Chat, Voice and WhatsApp apps'
`MessagingStyle` notifications with full per-message text, and their reply actions carry a
`RemoteInput` the listener can fire; a sideloaded app may be the default SMS app. Tasks:
`docs/BRIDGE_PHONE_TASKS.md`. **Firmware does not change** (§PROTOCOL below).

## What exists (from the code)
- **Backends.** `app/backends/base.py` `Backend` protocol (`deliver/start_link/complete_link/
  render_state`); `registry.py` maps one `kind` to one implementation: `pager`, `webapp`,
  `gchat` (the Workspace Chat-app adapter, `app/backends/gchat.py`, never usable here), `sms`
  (`app/backends/sms.py` `SmsBackend`, bridge outbox only; Twilio removed 9 Oct 2026, last at 05ec3ed). `Routing._deliver_one` applies every `DeliverResult` through
  `messages_store.record_delivery_attempt` (attempts+1; `ok=False` at 5 → `failed`; `ok=True`
  with state `queued` only clears `error`). `jobs.tick` retries `queued` `sms` deliveries via
  `messages_store.list_recent_queued_by_kind("sms")` + `Routing.redeliver` (24 h window); the Twilio retry path is removed 9 Oct 2026 (last at 05ec3ed), and bridge `ob_` items are skipped.
- **SMS model (docs/RELAY_SMS_DESIGN.md).** `users.smsNumber` + `smsNumbers/{e164}` index
  (`users_store.set_sms_number`); an external contact (`users/{x_…}` `kind:"external"`,
  `ownerFamilyId`, `phone`, `externals_store.get_or_create`) carries one `sms` backend row
  (`ensure_sms_backend`, fixed id `sms`, `config.phone`). The `sms` backend's `deliver` (9 Oct 2026: bridge-only; it was
  `SmsTwilioBackend`, which read `smsConsent` and called `sms_client.send_sms`, removed at 05ec3ed) hands the send to the sender's bridge. Inbound: `app/inbound_text.py` `handle_text` (from `/bridge/events`; the Twilio webhook is removed) resolves the target person, runs blocked, duplicate, known-contact-delivers, else holds
  (`held_sms_store.create`, `alerts.sms_held_upsert`). Approve: `routers/family.py`
  `_approve_sms_unknown` (contact, edge, `rederive_family_sms_contacts`, backlog via
  `routing.send(... wire_id=row.id)`). `Routing._sms_route_reject` refuses an external peer unless
  the person side has `smsNumber`; `_send_group` skips every external member.
- **Policy.** `policy.check(sender, recipient, has_edge_out, has_edge_in)` reads the *numbers*
  column for a `kind:"external"` peer and the *people* column for a person (`rule()`).
- **Book.** `book.entries_for(owner)` lists persons, `sms_contacts_for(owner)` externals (with
  `sendable` = `smsNumber` set and policy clear) and `list_groups_for_member` groups; `devcfg.
  _approved_contacts` emits `t:"grp"` for groups, `t:"sms"` for externals, `_contact_type_hint`
  (`chat`/`web`) for persons. `firmware/main/book.h` reads `t` as an icon hint
  (`"web" | "sms" | "chat"`, plus `grp`); unknown types are copied verbatim.
- **Groups (docs/GROUP_CHAT_DESIGN.md).** `conversations_store.create_group` (≥2 members, minted
  `g_` key, alias in the flat `aliases/` namespace); `Routing._send_group` fans one `seq` /
  `groupMsgId` to every member but the sender and passes `sender_alias` into `create_message`;
  `PagerBackend._from_and_sndr` sets `from` = the group alias and `sndr` = `msg.senderAlias`
  (`docs/PROTOCOL.md` §3.1: `sndr` has the alias regex, ≤16). `firmware/main/msg.c` stores `sndr`
  as a string; `scr_pick.c` labels rows from `type` verbatim.
- **Alerts.** `app/alerts.py` (`create` = store + `push_alert` FCM to every admin and super of the family; `_ALERT_TITLES`
  in `backends/webapp.py` raises on an unknown kind); `store/alerts.py` `AlertKind` literal,
  `find_open(family, kind, subject_uid, peer_phone)`; web `AlertCard.tsx` switches on kind.
- **Setup codes.** `devsetup.issue()` + `store/setup_codes.py` (server-only collection, short
  expiry, `jobs` sweeps expired) — the pairing pattern to copy, minus the HKDF secret derivation.
- **FCM.** `backends/webapp.py` `FCMClient.send_data(tokens, data)`; `alerts_module.get_fcm_client()`
  holds the one configured client; `main.py` builds `FirebaseFCMClient` when credentials exist.
- **Web.** `app/family/devices/page.tsx` (table + `SetupCodePanel`), `components/MemberDrawer.tsx`
  (profile, SMS number, `ApprovedEditor`), `app/family/contacts/page.tsx`, `app/family/alerts/
  page.tsx` + `AlertCard.tsx`, `app/settings/book/page.tsx` (member picker over `GET /api/book`),
  `lib/api.ts` (`api.get/post/patch/put/del`), `lib/types.ts` (`AlertKind`, `BackendKind`).
  Static export (`output: 'export'`): no dynamic route segments, member pages use `?uid=`.
- **Dev/test.** `relay/docker-compose.yml` services `emqx`, `firebase`, `relay`, `bridge-sim`
  (removed 9 Oct 2026, last at 05ec3ed); `tools/e2e_v2.py` scenarios (`relay_sms` removed the same day); `relay/tests/conftest.py` emulator fixtures; `test_rules.py` through the emulator REST.
- **Toolchain on the Mac (8 Oct 2026).** `adb` and OpenJDK 25 present; **no** Android SDK,
  Android Studio or Gradle. AGP 8.x needs JDK 17.

## Decisions
1. **A bridge is a person's phone.** `bridges/{bridgeId}` (`b_` + 8 hex, `app/ids.new_id`) =
   `{ownerUid, familyId, label, tokenHash (sha256 of the bearer secret), pairedAt, fcmToken,
   lastSeenAt, simNumber (the *accepted* SIM number, decision 3), status: {battery, listenerBound,
   smsDefault, accessibility, accounts: [email…], simNumber (as reported), voiceNumber, tier2Count,
   version, error}, caps: {sms, gchat, gvoice}, createdAt, createdBy}`.
   Server-only reads and writes (no rules block; the web reads `GET /api/family/bridges`, which
   never returns `tokenHash` or `fcmToken`; `test_rules.py` pins default-deny) *(review 9 Oct
   2026: a client-readable doc would hand `tokenHash`, `fcmToken` and account emails to every
   family admin's browser for no consumer; W2 polls the API).* One bridge has one owner; a person may own several bridges only if
   each has a different SIM (assignment refuses a second bridge whose `simNumber` is set when the
   owner already has `smsNumber` from another bridge). *(the owner's SMS model: a number belongs to
   a person, docs/RELAY_SMS_DESIGN.md decision 1; a phone signed into the child's own Google account
   posts in Chat as the child, so the bridge for @kid is signed into @kid's account.)*
2. **Pairing copies the setup-code shape.** `POST /api/family/bridges {ownerUid, label}` (family
   admin or super; owner must be a non-disabled person of the family) creates the bridge row and
   `bridgePairCodes/{code}` = `{bridgeId, expiresAt: now+10 min}` (server-only, 8 digits,
   `secrets.randbelow`, swept with setup codes) and returns `{bridgeId, code, expiresAt}`. The app
   calls `POST /bridge/pair {code, version, accounts, simNumber?, voiceNumber?, caps}` (per-IP
   limiter from `webhooks.py` `_check_webhook_ip_rate_limit`, bucket `bridge`, **plus** one global
   bucket `rate_limits_store.check_and_increment("bridge_pair:global", limit=10, window_s=60)` →
   429 *(review 9 Oct 2026: `_client_ip` trusts the first `X-Forwarded-For` entry, so the per-IP
   cap alone does not bound guesses against an 8-digit code; pairing is rare, a global cap costs
   nothing)*): the code doc is
   consumed with a transaction (`delete` after read; a second use is 404), a 32-byte secret is
   minted, `tokenHash` stored, and `{bridgeId, token: "<bridgeId>.<secret>"}` is returned **once**.
   Every other `/bridge/*` route authenticates `Authorization: Bearer <bridgeId>.<secret>` with
   `hmac.compare_digest` against `tokenHash` (`app/bridgeauth.py` `require_bridge` dependency);
   a missing/invalid token (or a row whose `tokenHash` is null) is a uniform 401. Unpair
   (`DELETE /api/family/bridges/{id}`) clears `tokenHash` and `fcmToken`, clears the owner's
   `smsNumber` if it equals the bridge's `simNumber` (then `book.rederive_sms_contacts`), acks
   every `pending` outbox row `failed` `unpaired` and marks its delivery `failed`, keeps the row
   (`status.unpaired: true`) and keeps every contact and conversation *(review 9 Oct 2026: a
   pending row left behind would otherwise be retried by the tick through Twilio if the member
   later gets a relay number, or sit `queued` with no phone to ack it)*.
3. **The SIM number is the owner's `smsNumber`.** On **pair** (and on an admin's reassign or
   "Accept SIM number", B6) the relay sets the bridge's top-level `simNumber` and calls
   `users_store.set_sms_number(owner, e164)` when it differs (`SmsNumberTaken` → logged `bridge
   sim taken`, `status.error` set, nothing else changes), then `book.rederive_sms_contacts(owner,
   broker)` (the re-derive/bump of RELAY_SMS_DESIGN decision 7 lives in the admin PATCH path, not
   in `set_sms_number`, so the bridge path calls it itself), so the owner's externals enter the
   pager's `c[]` with `t:"sms"`. A heartbeat only records `status.simNumber`; a mismatch with the
   accepted `simNumber` sets `status.error = "SIM changed"` and changes nothing else *(review 9 Oct
   2026: a heartbeat that rewrites `smsNumber` lets a stolen token re-point the member's number,
   and would stomp a number an admin set on purpose within five minutes)*. `voiceNumber` is **not** an `smsNumbers` index entry *(inbound Voice texts arrive from the
   bridge with the owner known, decision 5; nothing routes inbound by the Voice number)* but it does
   become the member's `smsNumber` when there is no SIM (O1, revised 9 Oct 2026).
4. **One `sms` kind, one transport (void 9 Oct 2026: the Twilio branch is removed; the bridge is the only path).** Originally: `SmsTwilioBackend.deliver` gains a first step: `bridges_store.
   get_by_sms_number(sender.smsNumber)` (query on the accepted top-level `simNumber` **or** `voiceNumber`, O1 revised) — when it
   names a paired bridge with `caps.sms` or `caps.gvoice`, the send is handed to `bridge_outbox.enqueue_send(bridge,
   msg, bid, source=<the channel this member last used with this external, decision 5; `gvoice`
   only when `caps.gvoice`, else `sms`>, to={phone, conversationId?}, text=_render_body(msg))`,
   the delivery's `externalId` is set to the outbox id, and it returns `DeliverResult(ok=True,
   state="queued", external_id=<outbox id>)`; **no** consent gate, no `relay_body` wrapper, no disclosure, no
   `defang` *(10DLC obligations are Twilio's; a text from a personal SIM is person-to-person)*.
   Otherwise `failed` with `no_bridge` (9 Oct 2026: Twilio removed, last at 05ec3ed; members without a
   bridge have no SMS). The owner's earlier "may delete it later" flag is closed. The outbox id is
   deterministic, `ob_<msgId>_<bid>`, written with `create()`; on `AlreadyExists` the existing
   row's state is applied (`pending` → `queued`, `sent` → `mark_delivery_sent_if_queued`, `failed`
   → `failed`), so a redeliver also repairs an ack whose delivery write was lost. `jobs.tick`
   skips a queued `sms` delivery whose `externalId` starts with `ob_` *(review 9 Oct 2026: the
   tick scans the 50 oldest queued `sms` deliveries and dispatches at most 10; an offline phone's
   backlog would otherwise starve every Twilio retry for up to 24 h)*. `jobs.tick` additionally
   fails every `pending` outbox row older than 24 h on every bridge, paired or not (`reason:
   bridge_offline`) and marks its
   delivery `failed` (`mark_delivery_failed_if_queued`), so a dead phone cannot hold deliveries
   forever. `render_state` for an `sms` delivery is unchanged (it returns the raw state, `queued`; the web's
   chip text is W1's).
5. **Inbound events.** `POST /bridge/events {events: [{id, source: "sms"|"gchat"|"gvoice"|"whatsapp",
   kind?: "message"|"inspect", conversation: {id, title?, isGroup, link?}, sender: {name,
   phone?}, text, ts, attachments?: [{kind: "image"|"video"|"audio"|"file"}], people?: [name…]}]}`
   → `{results: [{id, outcome}]}`; every event is processed (the phone retries the batch on a
   non-2xx, so handled cases are 200). Bounds (422 on violation): ≤50 events per batch, `id`
   `^[A-Za-z0-9_-]{1,64}$`, `text` ≤1600 cp, `title`/`sender.name` ≤200 cp, `conversation.id`
   ≤512 B, `people` ≤64. Per-bridge limiter `bridge_events:{bridgeId}` 120 events/min (counted per
   event) → 429; at most 50 new `bridgeConversations` rows per bridge per day, beyond that
   `dropped_conv_cap` *(review 9 Oct 2026: invented conversation ids would otherwise mint
   unbounded rows, alerts and admin pushes)*. Idempotent on `(bridgeId, id)`: `wireId` =
   `br_<bridgeId>_<id>` through `messages_store.wire_id_exists`; the `heldSms` and `heldChat` row
   ids are the **same string**, so a backlog release (`wire_id=row.id`) and a retried live
   delivery dedup against each other. The target
   person is **always `bridge.ownerUid`** (no `smsNumbers` lookup; the SIM and the Google account
   are the owner's). Per source:
   - `sms` and `gvoice`: `conversation.isGroup` → `dropped_group` (group MMS / Voice group texts
     are out of scope for v1); `sender.phone` normalized (`externals_store.normalize_phone`; bad
     or absent → `dropped_bad_from`); then the step table of RELAY_SMS_DESIGN decision 4 from the *blocked* step on,
     factored out of `_handle_inbound_sms` into `app/inbound_text.py` `handle_text(target, from_
     number, raw_body, sid, routing, reply)` (no keywords; the Twilio keyword step was removed 9 Oct 2026). `reply(text)` for the bridge enqueues an outbox send
     on the same source (the `too_long` hint). Empty text with attachments → `[photo]` for one
     image, else `[attachment]`. A delivered text records the channel **per member** on the
     external's `sms` backend row: `config.via.<ownerUid> = "sms"|"gvoice"` and, for `gvoice`,
     `config.voiceConv.<ownerUid> = conversation.id` (`backends_store.update_backend`), which
     decision 4 uses as the outbound `source` and `to.conversationId` *(reply on the channel they
     last used; a peer reachable by both is one contact, keyed by phone, docs/RELAY_SMS_DESIGN.md's
     `get_or_create`. Review 9 Oct 2026: the contact is family-wide, so one sibling's Voice thread
     must not switch another sibling's replies to Voice; and a Voice reply needs the Voice
     conversation, not just the number, for tier 1)*.
   - `gchat`: look up `bridgeConversations/{bridgeId}_{ref}` (decision 6). `subscribed`
     → deliver (decision 7); `ignored` or `paused` → `dropped_ignored`/`dropped_paused` (nothing
     stored); `seen` or new → hold (`heldChat/br_{bridgeId}_{eventId}` = `{bridgeId, conversationId,
     familyId, toUid, senderName, body (≤1600 cp), receivedAt, status: held|delivered|too_long|
     dismissed}`, cap 25 per conversation → `held_cap`) and upsert the row plus ONE open
     `chat_unknown` alert per `(bridge, conversation)` (decision 8). An event of `kind:"inspect"`
     only upserts the row (`title`, `isGroup`, `people`, `link`, `inspectedAt`), never an alert.
   One INFO line per event: `bridge in bridge=<id> src=<source> conv=<id|-> from=<…1234|name>
   outcome=<…>`, numbers redacted with `sms_client.redact_phone`.
6. **Conversations the bridge has seen.** `bridgeConversations/{bridgeId}_{ref}` with `ref =
   sha256(conversationId)[:16]` *(review 9 Oct 2026: Android's `shortcutId`/`sbn.key` may contain
   `/` and `|`, which break a doc id and a URL segment; every `/api/family/bridges/{b}/
   conversations/{…}` route below takes `ref`)* =
   `{bridgeId, familyId, ownerUid, source: "gchat"|"gvoice", conversationId, title, isGroup, link,
   people: [name…] (speakers seen, ≤64, insertion order), lastPreview, lastAt, firstSeenAt,
   heldCount, status: "seen"|"subscribed"|"ignored"|"paused", uid (the external, decision 7),
   convKey (the group doc, groups only), pagerName, customName: bool, alertId}`; server-only,
   read through the API. This row is the web's "Seen, not subscribed" list and the source of the
   roster's "people who have spoken". A later title change from the phone updates `title` and,
   unless `customName`, the group's `name`/the external's `displayName` (+ book bump).
7. **A subscribed conversation is an external user; a group also gets a group doc.**
   `POST /api/family/bridges/{bridgeId}/conversations/{ref}/subscribe {pagerName,
   canReply = true, roster: [{name, nick}]}`:
   - Validation: `pagerName` via `book.validate_nick` bounds (1–16 cp, ≤48 B) and unique among the
     owner's `entries_for` labels (409); each `nick` must pass `wire.is_valid_alias` (lowercase
     alias shape, ≤16) and be unique within the roster *(it travels as `sndr`, which has the alias
     regex on the wire, docs/PROTOCOL.md §3.1; "Grandma Jo" therefore becomes `grandma-jo`; the
     web derives the default with `_slug(first name)` + `-2`, `-3` on collision)*; `rule(owner.
     policy.in_, "person") == "none"` → 409 `@kid's inbound policy allows nobody`.
   - The external: `externals_store.get_or_create_chat(family_id, bridge_id, conversation_id,
     display_name=pagerName)` → with `h = sha256(f"{bridgeId}|{conversationId}")`, uid `x_c` +
     `h[:16]`, alias `c` + `h[:11]` *(review 9 Oct 2026: the earlier `c_<bridgeId>_<hash8>` alias is
     21 characters and fails `ALIAS_RE`'s 16)*, `kind:"external"`, `ownerFamilyId`, `phone: null`,
     new field `chat: {bridgeId, conversationId, source, link, isGroup, title, canReply}`; name reservation as for SMS contacts (`ContactNameTaken` → 409); backend
     row `kind:"bridge"` (fixed id `bridge`, `config: {bridgeId, source, conversationId, link}`),
     `backends_store` treats `bridge` like `sms` (valid only on an external; returned by `list_
     backends`). `BackendKind` gains `"bridge"`; `registry` maps it to `BridgeBackend` (`app/
     backends/bridge.py`): `deliver` = `enqueue_send(... source, to={conversationId, link}, text=
     msg.body)` → `ok=True, state="queued"`; `render_state`: queued → "waiting for the phone",
     sent → "sent on Google Chat"/"sent on Google Voice".
   - DM (`isGroup: false`): edge `allow owner→external message:true` **always**;
     `canReply:false` is `chat.canReply = false`, enforced by decision 9(c), not by edges *(review 9
     Oct 2026: `policy.check` reads the owner's own `allow/{owner}_{external}` for both directions,
     so dropping it to make the kid read-only also drops every inbound text under the default
     `people` policy)*; `book.bump_and_push({owner}, reason="chat_subscribe")`. The
     external is listed in the book like an SMS contact, `t:"chat"`, `phone: null`,
     `chat: {source}` on the `BookEntry`.
   - Group (`isGroup: true`): additionally `conversations_store.create_bridge_group(name=
     pagerName, owner_uid, external_uid, bridge_id, conversation_id, roster)` → `conversations/
     {g_c<h[:14]>}` with alias `bc` + `h[:8]` (same `h`), conv doc and alias written with `create()`
     in one transaction, `AlreadyExists` → return the existing doc *(review 9 Oct 2026: a random key
     made a retry after a crash mint a second group)*, `kind:"group"`, `uids: [owner, external]`, plus new fields
     `bridge: {bridgeId, conversationId, source, link}` and `roster: {nick: name}`; the alias is
     relay-minted *(the admin names the pager entry, never an alias; a minted alias cannot collide
     with a typed one)*. The group enters the owner's book through `list_groups_for_member` as
     today (`t:"grp"`), `onPager` past 32 as for any entry. A group with `bridge` set is managed
     only here: `POST /api/conversations/{alias}/members` and `DELETE …/members/me` return 409
     `managed under Google Chat` *(review 9 Oct 2026: an added sibling would get edges to the
     external from `_create_missing_message_edges` and post into the Chat thread as the owner)*.
   - Mark the row `subscribed` with `uid` and `convKey` **before** the backlog, so an event that
     lands mid-subscribe is delivered live rather than held behind a decided alert; then release
     the backlog: every `heldChat` row `held` for the conversation in `receivedAt` order →
     `routing.send(sender_uid=external.uid, recipient_alias=<group alias | owner alias>, kind=
     "text", body, origin_backend_kind="bridge", origin_backend_id="bridge", wire_id=row.id,
     ts=row.receivedAt, sender_alias=<roster nick>)` (decision 9) → `delivered`; > 160 cp →
     `too_long`, kept for the parents. Then `alerts_store.decide(handled)`, row `pagerName`,
     `customName = pagerName != title[:16]`.
   - `POST …/ignore` → row `ignored`, held rows `dismissed`, alert dismissed. `PATCH …
     {pagerName?, canReply?, paused?, roster?}` → rename (book bump), re-edge, `paused` (both
     directions dropped: inbound `dropped_paused`, outbound `BridgeBackend.deliver` → `failed`
     `paused`), roster edits. `DELETE …` (unsubscribe) → edges deleted, group doc and external
     deleted (`externals_store.delete`), row back to `seen` with `uid/convKey` cleared; messages
     keep their history (per-copy visibility, GROUP_CHAT_DESIGN decision 3).
   - **Promote a participant** (`POST …/conversations/{dmRef}/subscribe` with the `ref` of the DM
     the phone reported, or decision 10's link) — the group roster's "Add as contact" is the
     same subscribe call on a *DM* conversation; the web enables it only when a `seen` DM row
     with that person's name exists or a link is pasted. *(participants of a group are names until
     promoted — a group subscription must not grant six strangers a DM.)*
8. **One `chat_unknown` alert per conversation.** `AlertKind` gains `"chat_unknown"`; `Alert`
   gains `bridgeId`, `conversationId`, `convRef` (decision 6's `ref`, what the web's Subscribe
   calls with), `convTitle`, `isGroup`, `people: [name…]`, `source`;
   `find_open` gains `bridge_conv: tuple[str, str] | None` to match on. `alerts.chat_held_upsert
   (family_id, target, row, body, sender_name)` creates it (`preview` = newest text, `heldCount`,
   `pushBody` = `"<title> (<sender>) → @kid: <text>"`) or updates `preview/heldCount/updatedAt/
   people` and pushes again. `_ALERT_TITLES["chat_unknown"]` = `"Google Chat for @kid: <title>"`.
   Approve on this kind is **not** `POST /alerts/{id}/approve` (which needs `name`): the web's
   **Subscribe** opens the dialog and calls decision 7's endpoint, which decides the alert;
   `block` is 400 for this kind; `dismiss` dismisses the alert and marks held rows `dismissed`
   (the row stays `seen`, so a later text raises a fresh alert).
9. **Routing changes, all additive.** (a) `Routing.send` gains `sender_alias: str | None = None`,
   passed through to `_send_group` and `_create_and_deliver` (DM copies get `senderAlias` too; the
   pager ignores it on a DM since `groupMsgId` is `None`). (b) `_send_group` skips an external
   member only when it has no `bridge` backend (`backends_store.has_kind(uid, "bridge")`); a
   bridge external may be the sender (inbound) or a recipient (outbound). (c) `_sms_route_reject`
   is renamed in spirit only: for an external peer with a `bridge` backend the person side must be
   that bridge's owner and the bridge must be paired, else `no_bridge`; person → external with
   `chat.canReply == false` → `not_allowed`; the SMS branch is unchanged. `RejectReason` gains
   `"no_bridge"`. `_send_group` runs this same check for every pair in which either side is an
   external (before the policy gate), so a bridge group can only carry owner ↔ external *(review 9
   Oct 2026: the group loop never called `_sms_route_reject`, so the relaxed skip would let any
   member of the group reach the bridge)*. `routers/ingest`'s §4.2 case-3 reply for `no_bridge`: `bridge not set up; ask your
   admin`. (d) `policy.check`: a `kind:"external"` with `chat` set is read as a **person** peer
   (`_peer_kind(user)`), so the subscribe edge is what approves it *(the owner: "a subscribed
   conversation is an approved edge for its owner only; nothing in the people/numbers columns
   changes"; the numbers column stays about phone numbers)*. The web's `PolicyPicker` text is
   unchanged.
10. **Add by link.** `POST /api/family/bridges/{bridgeId}/inspect {link}`: `link` must be an
    `https://chat.google.com/…` or `https://mail.google.com/chat/…` or `https://voice.google.com/…`
    URL (422 otherwise); enqueues an outbox item `{kind: "inspect", link}`; 202 `{outboxId}`. The
    phone's tier 2 opens it, reports an `inspect` event (decision 5) whose `conversation.link` is
    the pasted link; the row appears under "Seen, not subscribed" within seconds and the web opens
    the subscribe dialog on it. The link is stored on the row, the external's `chat.link`, the
    group's `bridge.link` and sent in every outbox send so tier 2 can initiate when tier 1 has no
    reply action. *(decided after the owner's "send a message to grandma" requirement: a stored
    link is the one way to open a thread nobody has posted in since a reboot.)*
11. **Outbox and acks.** `bridges/{id}/outbox/{obId}` = `{kind: "send"|"inspect", source, to:
    {phone?, conversationId?, link?}, text, msgId, bid, replyHint: conversationId?, state:
    "pending"|"sent"|"failed", createdAt, ackedAt, tier, reason}`. `GET /bridge/outbox?wait=N`
    returns `{items: [pending, oldest first, ≤20]}`, long-polling in 2-s Firestore reads up to
    `wait` (cap 25 s, default 0) when empty, sleeping with `await asyncio.sleep` so a waiting poll
    *(9 Oct 2026: the phone never sends `wait>0` — see "Orchestrator decisions" O2; `wait` stays for the simulator and e2e only)*
    holds no threadpool thread; the relay also pushes FCM data `{kind: "outbox"}` to
    `bridges.fcmToken` on every enqueue through `alerts_module.get_fcm_client()`. `POST /bridge/
    outbox/{obId}/ack {state: "sent"|"failed", reason?, tier: 1|2}`: `sent` → `mark_delivery_sent_
    if_queued`; `failed` → `mark_delivery_failed_if_queued` with `error = reason` (never retried:
    the phone already tried both tiers); `tier: 2` increments `status.tier2Count`. Re-acks are
    idempotent and **re-apply the delivery transition from the stored row state** (both helpers
    are monotonic), so a lost delivery write is repaired by the phone's retry. Items with no
    `msgId` (the `too_long` hint, id `ob_h_<wireId>`; `inspect`) change only the outbox row. `POST /bridge/heartbeat {status, fcmToken?}` every 5 min → `lastSeenAt`, `status`,
    decision 3's SIM check. One INFO line per ack: `bridge out bridge=<id> ob=<id> state=<…>
    tier=<n> ms=<ack latency>`.
12. **Web.** Family → Devices gains **Bridge phones** (table: label, member, Google account(s), SIM
    number, Voice number, last seen, battery, listener/SMS/accessibility chips, tier-2 count;
    actions Reassign, Unpair; "Add bridge phone" → member picker + label → a pairing-code panel
    with the 10-min expiry and the phone setup checklist). A new page `/family/chat?uid=` ("Google
    Chat", member picker like `/settings/book`, linked from the member drawer as "Google Chat…"):
    Subscribed table (name on pager, type, people seen, last message, "Not on pager" past 32;
    Rename / Edit roster / Pause / Unsubscribe), "Seen, not subscribed" (Subscribe, Ignore,
    "Show ignored" toggle), "Add by link" (input + Inspect; polls the seen list for 30 s). The
    Subscribe dialog: name on pager (default `title[:16]`), "Kid can reply" switch, roster table
    (name, nick with the slug default, inline alias-shape validation), note "People who have not
    spoken yet appear here when they do." `AlertCard` gains `chat_unknown` (title, group/DM, people
    seen, newest text, "N messages waiting"; Subscribe → the dialog, Ignore, Dismiss). Address
    book page: Chat entries show a "Google Chat"/"Google Voice" chip. `DeliveryChips`: `bridge`
    kind → "waiting for the phone" / "sent on Google Chat". Existing SMS pickers unchanged.
13. **Android app** (`bridge-android/`, Kotlin, Gradle wrapper, AGP 8.7.x + JDK 17, minSdk 26,
    targetSdk 35, applicationId `app.kidpager.bridge`, sideloaded via `adb install`):
    - Setup screen: relay URL (default `https://kidpager.sps-by-the-numbers.com`; Hosting rewrites `/bridge/**` to Cloud Run, so `wait=` is capped at 25 s, under the 60 s rewrite timeout), pairing code → `POST /bridge/pair`; token in
      `EncryptedSharedPreferences`; status rows with "Open settings" buttons for Notification access
      (`ACTION_NOTIFICATION_LISTENER_SETTINGS`), Default SMS app (`RoleManager.createRequestRoleIntent
      (ROLE_SMS)`), battery optimisation (`ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`),
      Accessibility (`ACTION_ACCESSIBILITY_SETTINGS`); a log screen (ring buffer of 500 lines).
    - `BridgeService` (foreground, `START_STICKY`, boot receiver, `RECEIVE_BOOT_COMPLETED`), 5-min
      heartbeat (`WorkManager` periodic + in-service timer).
    - `ChatNotificationListener` (`NotificationListenerService`) for `com.google.android.apps.
      dynamite` and `com.google.android.apps.googlevoice`, `com.whatsapp` and `com.whatsapp.w4b` (WhatsApp, WA6): `NotificationCompat.MessagingStyle.
      extractMessagingStyleFromNotification`, `conversationTitle`, `isGroupConversation`,
      `messages[]` (sender `Person.name`, text, timestamp); conversation id = `sbn.notification.
      shortcutId` when set, else `sbn.key` with the account/tag stripped; dedup on `(conversation
      id, message timestamp, text hash)` persisted in Room (7-day TTL); reply-action cache
      `conversationId → (action, RemoteInput[])` from `notification.actions` and
      `NotificationCompat.WearableExtender(notification).actions`, rebuilt from
      `getActiveNotifications()` on `onListenerConnected`; Voice texts carry the peer's number
      parsed from the title/sender when present (`PhoneNumberUtils.normalizeNumber`).
    - Tier 1 reply: `RemoteInput.addResultsToIntent(remoteInputs, intent, bundleOf(key to text))`;
      `action.actionIntent.send(context, 0, intent)`; `CanceledException` → tier 2.
    - Default SMS app: `SMS_DELIVER` / `WAP_PUSH_DELIVER` receivers (`BROADCAST_SMS` / `BROADCAST_
      WAP_PUSH` permissions), `SmsManager.getSmsManagerForSubscriptionId` (dual SIM: `sim` from
      the outbox item, default otherwise), `divideMessage` + `sendMultipartTextMessage` with sent
      and delivered `PendingIntent`s; a quick-response service and a dummy `ACTION_SENDTO` activity
      so the role is grantable; MMS text extracted from the PDU, attachments reported by kind.
    - `BridgeAccessibilityService` (tier 2, `canRetrieveWindowContent`, packages above): open the
      link as `ACTION_VIEW`, wait ≤15 s for an editable node (`className` contains `EditText` or
      `isEditable`), `ACTION_SET_TEXT`, click the node with `contentDescription` matching
      `Send`, verify the text appears in a non-editable node within 5 s, `performGlobalAction
      (GLOBAL_ACTION_HOME)`; `inspect` reads the title bar and visible sender names. Used only when
      tier 1 has no action or failed.
    - Outbox worker: FCM `onMessageReceived` (`kind: outbox`) and a 60-s fallback poll (`GET
      /bridge/outbox?wait=25`), `ack` per item with `tier`. Firebase: `google-services.json` for the
      existing Firebase project (Android app `app.kidpager.bridge`, created in the console or
      `firebase apps:create android`); **if the file cannot be obtained in a session the Gradle
      plugin is applied conditionally and the build runs poll-only** (`BuildConfig.FCM = false`).
    - `Targets.kt` holds every per-app id and description the tier-2 code uses (Chat, Voice, WhatsApp);
      each is marked verify-on-bench in `bridge-android/README.md`. WhatsApp's are in WA4.
    - Phone setup (documented in `relay/README.md` "Bridge phones"): screen lock **None** *(a
      PIN leaves the phone in before-first-unlock after a power cut; no app runs, no notification
      fires)*, Do Not Disturb off, both Google apps signed in with notifications on, Chat never
      left open on a thread, charge limiter / smart-plug duty cycle, adb over Wi-Fi for maintenance.
14. **Simulator.** `tools/bridge_sim.py`: a Python stand-in for the phone speaking the §11/§5
    contract (pair, events, outbox poll, ack, heartbeat) with `/_inject` (an inbound event),
    `/_outbox` (what it "sent"), `/_fail_next`, `/_reset`; compose service `bridge-sim`; `tools/e2e_v2.py` scenario `bridge` (pair → SIM text from a known contact →
    pager; unknown Chat group → `chat_unknown` → subscribe → backlog on the pager with `sndr` →
    pager reply → sim outbox; Voice text → one contact for both channels; inspect by link).
15. **Rules, indexes, migration.** New collections `bridges`, `bridgePairCodes`,
    `bridgeConversations`, `heldChat`, `bridges/*/outbox` (all server-only, no match block); no index
    changes (outbox and held queries are per-document-prefix or single-field). No migration (prod
    is test data, 30 Sep 2026 rule). Retention: `jobs.sweep` deletes `heldChat` by `receivedAt`
    with the messages TTL and acked outbox rows older than 7 days.

## Rejected
- The Orange Pi headless-browser bridge: Chat's DOM changes often; a Playwright-launched Chrome
  trips Google's sign-in block; notification contracts are the more stable surface.
- The Chat API with user auth, Chat email notifications, Voice email forwarding as the primary path
  (API is Workspace-only; email notifications fire only when idle; Voice email reply is unverified
  since 2021 and the phone covers Voice anyway).
- A parent's account hosting a child's subscription (the child would post as the parent).
- Roster participants as contacts by default (a group would grant DMs to everyone in it).
- A second delivery state for "handed to the phone": `queued` + outbox ack keeps `DeliveryState`,
  the chips and the tick untouched.
- A separate `gvoice` backend kind: a Voice peer is a phone number; one external per number with
  `config.via` keeps Grandma one entry whether she texts the SIM or the Voice number.

## Transaction boundaries
- Pair: the code doc is read and deleted in one transaction; the token hash is written after it
  (a crash between leaves a bridge with no token: the admin issues a new code, which the create
  endpoint allows while `tokenHash` is null).
- Inbound text: `heldSms`/`heldChat` `create()` → alert upsert (non-transactional; repaired by the
  next text, exactly as RELAY_SMS_DESIGN "Transaction boundaries").
- Subscribe: name reservation + external `create` → backend row → group doc (+ alias `create()`)
  → edges → row `subscribed` → backlog (each `routing.send` dedup'd by `wire_id`) → alert decide.
  Every step idempotent on retry (`get_or_create_chat` and `create_bridge_group` derive their ids
  from `(bridgeId, conversationId)`, so a retry finds what the crashed attempt wrote).
- Outbox: `create()` on a deterministic id; ack transitions `pending → sent|failed` only (one
  transaction on the outbox row); the delivery write follows outside it and is re-applied by a
  re-ack or a redeliver (decision 4/11).

## Failure modes
| failure | effect | recovery |
|---|---|---|
| phone offline / app killed | outbox `pending`, chip "waiting to send SMS" / "waiting for the phone" | boot receiver + sticky service; tick fails rows > 24 h (`bridge_offline`) |
| reboot with a screen lock set | nothing runs until someone unlocks | setup checklist: lock None; heartbeat gap alert on the Devices row (last seen > 15 min, red) |
| Chat reply action gone (reboot, app update) | tier 1 fails, tier 2 opens the stored link | link stored on subscribe/inspect; no link → `failed` `no_link`, row shows "Add by link" |
| accessibility selectors rot | tier 2 `failed` `ui_changed`, `tier2Count` climbs | fix the service; tier 1 keeps working |
| SIM number taken by another member | `status.error`, owner's `smsNumber` unchanged | admin clears the other member's number |
| flood from an unknown Chat conversation | 25 held, rest dropped and logged | Ignore |
| held Chat text > 160 cp subscribed | row `too_long`, not on the pager | parents read it in the alert |
| Chat title changes | `title` follows; pager name follows unless custom | Rename |
| pairing code expired / reused | 404 from `/bridge/pair` | issue a new code |
| token leaked | holder can inject texts to the owner as any approved contact or subscribed conversation, read and ack (suppress) the owner's outbound texts; cannot change `smsNumber` (decision 3) or reach another bridge | Unpair, pair again (new token); per-bridge limiter bounds the flood |
| WhatsApp group tier 2 finds no conversation row with the title | outbox row `failed` `no_match` | tier 1 still works while the reply action exists; group tier 2 is best-effort (search by title) |
| WhatsApp sender is a `@lid` JID (no phone number) | `dropped_bad_from`, one INFO line, nothing stored | none in this change; the message is not bridged |

## What to measure
- `bridge in … outcome=` per outcome per day; `held` vs `delivered`; `dropped_ignored` volume.
- `bridge out … state= tier= ms=`: ack latency p50/p95 (expect < 5 s with FCM, < 60 s poll-only);
  tier-2 share (expect ≈ 0 after a week; a rise means the reply cache is being lost).
- Heartbeat gaps > 15 min per bridge per week; `status.battery` trend; `tier2Count`.
- Open `chat_unknown` alerts older than 7 days.

## PROTOCOL.md impact
**No envelope or firmware change.** *(9 Oct 2026 sweep: PROTOCOL.md carries two additive notes — the `c` row in §3.1 names `t:"chat"` for a bridged DM, and §4.2 case 3 lists the `bridge not set up; ask your admin` body.)* The pager receives a bridged group as a group page (`from` = the minted group alias,
`sndr` = the roster nick, both alias-shaped) and a bridged DM as a page from an external's alias;
the book lists them as `t:"grp"` / `t:"chat"` / `t:"sms"`, all of which `firmware/main/book.h`
already carries as icon hints, and `scr_pick.c` labels rows from `type` verbatim. A pager reply is
an ordinary `/up to:<alias>`. The §4.2 case-3 system reply list gains one body, `bridge not set
up; ask your admin` (relay text only; the device renders any `from:"system"` body). Firmware is
not rebuilt for this work.

## Orchestrator decisions (9 Oct 2026, owner not present; each is reversible and flagged in the report)

**9 Oct 2026: Twilio removed.** Twilio relay SMS was last present at commit 05ec3ed
(`05ec3ed703cf27c368cb4713d03ea3f25c8ac300`, 9 Oct 2026); added at a30ebca (8 Oct 2026), consent/keywords at
13c4a4b, first removal at 123efa4 (7 Oct 2026). With it, O3, O6 and O7 are moot. The `sms` backend is
bridge-only, and a non-bridge number fails `no_bridge`.

The server-architect review left eight items for the owner. The recommended option was taken for each so the build could continue:

- **O1 Voice-only bridge — REVISED (owner, 9 Oct 2026 00:xx PDT: "Google Voice only setup should be possible").**
  A bridge may carry `simNumber`, `voiceNumber`, or both. Both are top-level accepted fields set at pair
  (`POST /bridge/pair {…, simNumber?, voiceNumber?}`; the phone cannot read the Voice number, so the
  setup screen asks for it) and editable by a family admin on the Bridge row (`PATCH /api/family/bridges/{id}
  {voiceNumber?, simNumber?}`, same normalisation and 409 rules as `smsNumber`). `caps.sms` = SIM present
  and the default-SMS role held; `caps.gvoice` = Voice number present. The member's `smsNumber` on pair /
  reassign / number edit = `simNumber` if present, else `voiceNumber`; `bridges_store.get_by_sms_number(n)`
  matches **either** field, so decision 4's transport pick hands the send to the bridge in both cases and
  `_sms_route_reject` is unchanged (a Voice-only member has an `smsNumber`). Channel per send: the member's
  `via` entry for that external if set (decision 5), else `gvoice` when the member's number matched
  `voiceNumber`, else `sms`. A `gvoice` outbox item carries `to.phone` **and** `to.link =
  https://voice.google.com/u/0/messages?itemId=t.<E.164>` so tier 2 can initiate to any number with no prior
  notification *(the Voice web app addresses a thread by the peer's number; this is what makes a cold
  "text grandma" work on a Voice-only bridge)*. The welcome-text skip (O3) covers both numbers. Inbound is
  unchanged (the owner is known from the bridge). Web: the pairing panel and Bridge row carry both
  numbers and a "Voice only" chip when there is no SIM. Android: the setup screen has a Voice number field;
  an outbox item with `source:"gvoice"` tries the cached Voice reply action for that peer number (tier 1)
  before opening `to.link` (tier 2). Supersedes the earlier O1 text ("not supported in this pass").
- **O2 No long-poll from the phone.** A 25-s long-poll holds a Cloud Run instance for ~10 h/day per bridge and costs ~17k Firestore reads/day; the free tier is 50k. The phone polls `GET /bridge/outbox` with `wait=0` every 60 s (configurable 30–300 s on the setup screen) and additionally whenever an FCM data push arrives; the heartbeat response carries `pending` so a phone with FCM can skip idle polls. Outbound latency in the poll-only build is therefore ≤60 s, which is the accepted fallback until the owner registers the Android app with Firebase. `wait` remains implemented for the simulator/e2e.
- **O3 Welcome text.** *(Moot 9 Oct 2026: Twilio removed, last at 05ec3ed, and with it the welcome text.)* `_consent_by_admin`'s welcome text is skipped when the member's `smsNumber` is a bridge SIM (`bridges_store.get_by_sim_number`); person-to-person texting needs no disclosure. B2 Files gains `routers/family.py`.
- **O4 Voice sender shows a contact name.** The bridge account keeps an empty contacts list (setup checklist), and A3 reads the number from the notification's shortcut/URI when present before falling back to the title. A text whose sender cannot be resolved to E.164 is reported with `sender.name` only and lands as `dropped_bad_from` with one INFO line, never an alert.
- **O5 Out-policy on subscribe.** If the owner's outbound people rule is `none`, Subscribe forces `canReply:false` (the dialog shows the switch disabled with "@kid's policy does not allow outbound messages"); it never returns 409. Inbound is judged on the people column for a Chat DM and on the numbers column for an SMS contact, which is the owner's model (one person may be two contacts under two rules).
- **O6 STOP numbers.** *(Moot 9 Oct 2026: no opt-out registry exists; Twilio removed, last at 05ec3ed.)* A number that opted out of the Twilio number is not blocked on the bridge SIM: person-to-person traffic carries no opt-out semantics. Flagged for the owner; no code.
- **O7 Cross-design.** *(Moot 9 Oct 2026: TWILIO_ACCOUNTS is parked and Twilio is removed, last at 05ec3ed.)* If TWILIO_ACCOUNTS lands, an `smsNumbers` entry without `accountSid` is a bridge number. Noted there when that design is picked up.
- **O8 Router file.** B5 and B6 both live in `relay/app/routers/family_bridges.py`.


## WhatsApp (orchestrator decisions WA1–WA9, 9 Oct 2026)

*(owner, 9 Oct 2026: "implement whats app, both tiers". WhatsApp is a third notification source next
to Google Chat (`gchat`) and Google Voice (`gvoice`). The `sms` backend is bridge-only, so a WhatsApp
DM is a phone-keyed text and a WhatsApp group is a Chat-style conversation. Tasks: A9, B11, W7, D3 in
`docs/BRIDGE_PHONE_TASKS.md`.)*

**WA1 Source and capability.** `whatsapp` is a new source value wherever `sms|gchat|gvoice` is
enumerated: `BridgeEvent.source`, outbox `source`, `BridgeCaps.whatsapp`, web `BridgeSource`,
`ConversationOut.source`, book `chat.source`. `caps.whatsapp` is the phone's report: the pair body's
`caps.whatsapp` (listener bound and `com.whatsapp` installed), and heartbeat `status.whatsapp` (same
meaning) updates `caps.whatsapp`, because WhatsApp may be installed after pairing. `bridge_numbers.
caps_for` gains a `whatsapp` argument; number PATCH and accept-sim keep it. There is no number
requirement: the WhatsApp account's own number is whatever the owner registered (usually the SIM), and
the relay never needs it.
- Rejected: a WhatsApp number field on the bridge. The relay never uses it.

**WA2 DMs are phone-keyed texts (same path as Voice).** A WhatsApp notification with `isGroup=false`
goes to `_handle_text_event`: the target is the bridge owner, the text is held until approved for an
unknown number (`sms_unknown` alert), and `via: whatsapp` is recorded on the owner's external backend
`config.via[ownerUid]`, so later sends to that person go back on WhatsApp. The sender phone comes from
the shortcut id JID `<digits>@s.whatsapp.net` (becomes `+<digits>`), else the `dataUri`, else the sender
line or title normalized as E.164 (an unsaved contact shows as the number, with an empty contacts list),
else `dropped_bad_from`. Conversation tracking reuses the existing `voiceConv` slot generically; backend-dev
may rename it to `conv` or add `waConv`, whichever is the smaller change, and Voice behaviour stays
intact. Reply hints (`too_long` and the like) go back with `source: whatsapp`, `to.phone`,
`to.conversationId` (the JID) and `to.link = https://wa.me/<digits>`.
- Rejected: LID JIDs (`<digits>@lid`). A LID carries no phone number, so the sender is dropped with
  `dropped_bad_from` and logged once.

**WA3 Groups are Chat-style conversations.** `isGroup=true` goes to `_handle_chat_event` with source
`whatsapp`. The conversation id is the group JID `<id>@g.us` (shortcut id) and the title is the group
subject. The sender name is the message's Person name, with a leading `~ ` stripped (unsaved members
appear as `~ Name` or a number). Seen and held rows, one `chat_unknown` alert, and Subscribe, Ignore and
the roster work exactly as for Chat; a subscribed group projects as `t:"grp"`. Inspect by link is not
offered for WhatsApp: the relay's `Add by link` rejects WhatsApp links with 400 `whatsapp links cannot be
inspected; wait for a message`, and the web dialog says the same.
- Rejected: a group deep link. None exists for a WhatsApp group chat; `chat.whatsapp.com/<code>` is a
  join link, not a conversation link.

**WA4 Outbox and tiers (Android).** A `send` with `source: whatsapp` is dispatched as follows.
- DM: tier 1 is the ReplyCache by `conversationId`, else by phone (`rememberVoicePhone` and
  `conversationForPhone` generalised to any source). Tier 2 opens `https://wa.me/<digits>` with package
  `com.whatsapp` (WhatsApp opens the chat composer for a known number), then the existing composer and
  Send flow. Selectors in `Targets`: composer id `entry`, send button description `Send`.
- Group: `to.conversationId` (JID) and `to.title`. Tier 1 is the ReplyCache by `conversationId`. Tier 2
  opens the WhatsApp main activity, taps the toolbar search (`menuitem_search`, description "Search"),
  sets the title text, clicks the first conversation row whose text equals the title
  (`conversations_row_contact_name` / `conversation_contact_name`), then the composer and Send. No
  matching row fails with `no_match`.
- All ids and descriptions live in `Targets` and are marked "verify on bench" in `bridge-android/README.md`.
- Relay side (`SmsBackend._deliver_via_bridge`): `via == "whatsapp"` gives `source="whatsapp"`, `to.phone`,
  `to.link = wa_link(phone)`, and `to.conversationId` from the external's stored conversation when known.
  Caps fallback: no `caps.whatsapp` falls back to sms, then gvoice, then `failed no_bridge`. Group sends go
  through the existing bridge chat backend path, keyed by the conversation's source. Hint id, ack and retry
  semantics are unchanged.
- Risk: tier 2 drives the WhatsApp UI, which carries an account-ban risk. Group tier 2 is best-effort: it
  finds the group by searching its title, and a miss fails `no_match`.

**WA5 Channel choice for a phone contact.** Unchanged: the `via` entry for (external, member) wins, and it
is written by the last inbound channel (`sms`, `gvoice`, now `whatsapp`). No admin override in this change.

**WA6 Notification hygiene (Android).** The listener covers `com.whatsapp` and `com.whatsapp.w4b`
(WhatsApp Business) with the same mapping. Only MessagingStyle notifications map. Calls, "checking for new
messages", backup, status, and group-summary notifications (`FLAG_GROUP_SUMMARY`, "N messages from M chats")
are ignored; the group-summary skip is added if it is missing. Media placeholders (`📷 Photo`, `🎥 Video`,
`🎤 Voice message`, `🎵 Audio`, `📄 <name>` or `Document`, `📍 Location`, `👤 Contact`, `GIF`, `Sticker`)
become an `attachments` entry (image, video, audio or file) with empty text, so the relay renders `[photo]`
or `[attachment]`; a caption after the emoji is kept as text. Own messages match `MessagingStyle.user` as
today, and the sender `You` (WhatsApp's "You: ..." lines in group notifications) is skipped.

**WA7 Setup and status.** SetupActivity shows a WhatsApp row (installed yes/no, listener bound) and no number
field. The status block gains `whatsapp: bool` (installed and listener bound). The phone checklist: install
WhatsApp, register it with the SIM number, notifications on with previews, no contacts, and archive or mute
nothing you want bridged. The checklist notes the ban risk of tier 2 and that group tier 2 is best-effort.

**WA8 Web.** `BridgeSource` gains `whatsapp`. Chips and labels read "WhatsApp" wherever "Google Voice" or
"Google Chat" is chosen by source (AlertCard, the family chat page, settings book, the BridgePhonesSection
caps chip). The Add by link dialog refuses `whatsapp.com` and `wa.me` links client-side with the WA3 message.

**WA9 Docs.** This section; the task rows in `docs/BRIDGE_PHONE_TASKS.md`; one-liners in `README.md`,
`docs/OVERVIEW.md` and `docs/ROADMAP.md`; the `relay/README.md` runbook and simulator support for source
`whatsapp`; and the `bridge-android/README.md` checklist and verify-on-bench list.

## Cross-family review (9 Oct 2026)

**Numbers are unique across the whole relay.** `POST /bridge/pair` now runs the same number check as the family API's edit and accept-SIM paths (`bridge_numbers.number_taken`) and answers 409 with the same detail text. The check runs before the pair code is consumed (via the read-only `bridges_store.peek_pair_code`), so a refused pair does not burn the code. An unknown code is still 404 before any number check.

**Lookup prefers the sender's own bridge.** `bridges_store.get_by_sms_number(e164, prefer_owner=...)` returns the matching paired bridge owned by `prefer_owner` when there is one, else the first match. `SmsBackend.deliver` and `apply_numbers` pass the sender or owner, so data written before the pair check cannot send one family's text through another family's phone. The uniqueness check itself stays unpreferred: any other bridge with the number is a conflict.

**Channel records are transactional.** `backends_store.record_member_channel` does the read-modify-write of `config.via` and `config.voiceConv` inside one Firestore transaction, so two siblings' inbound texts handled concurrently both keep their entry. It writes nothing when the row is missing, not live, or unchanged; `inbound_text.record_channel` delegates to it.
