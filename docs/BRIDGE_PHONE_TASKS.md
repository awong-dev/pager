# Bridge phone: execution tasks (8 Oct 2026)

> **9 Oct 2026: Twilio removed** (last at 05ec3ed). The Twilio items below (the B2 transport switch, the B3 Twilio handler, `twilio-mock` in B7) are historical and void. SMS is bridge-only.

Design: `docs/BRIDGE_PHONE_DESIGN.md` (decision numbers below refer to it). Order: **B1–B9**
land the relay contract first; **W** and **A** consume it and may run in parallel after B4;
**D** last. Every task: `Read` before touching anything, `Files` is exhaustive, `Verify` is the
exact command. House rules: no unverified "defensive" code, cite the decision in docstrings,
numbers redacted in logs, one INFO line per event.

Common verify (relay): `cd relay && ruff check . && ruff format --check . && pytest -q`
(needs `docker compose up -d firebase`). Web: `cd web && npm run lint && npx tsc --noEmit &&
npm run build`. Android: `cd bridge-android && ./gradlew assembleDebug`.

---

## Backend (backend-dev)

### B1 Bridges store, pairing, bridge auth
- **Read:** design decisions 1–3, 11; `relay/app/store/setup_codes.py`, `relay/app/devsetup.py`
  (the issue/consume shape), `relay/app/store/users.py` (`set_sms_number`, `SmsNumberTaken`),
  `relay/app/routers/webhooks.py` (`_check_webhook_ip_rate_limit`), `relay/app/ids.py`,
  `relay/app/store/rate_limits.py`, `relay/app/book.py` (`rederive_sms_contacts`),
  `relay/app/routers/admin.py` `_patch_user_impl` (where the SMS re-derive runs today),
  `relay/tests/test_devsetup.py` (test shape).
- **Files:** new `relay/app/store/bridges.py`, new `relay/app/bridgeauth.py`, new
  `relay/app/routers/bridge.py`, `relay/app/main.py` (include the router), `relay/app/jobs.py`
  (pair-code sweep), new `relay/tests/test_bridges_store.py`, new `relay/tests/test_bridge_pair.py`.
- **Do:** `store/bridges.py`: `Bridge` model (decision 1 fields; `status` and `caps` as nested
  pydantic models with defaults), `create(owner_uid, family_id, label, created_by) -> Bridge`
  (`b_` id), `get`, `list_for_family`, `list_all`, `get_by_sms_number(e164) -> Bridge | None`
  (query the top-level accepted `simNumber == e164`, then filter `tokenHash` not null in Python;
  single-field, no index), `set_token_hash`, `set_sim_number`,
  `set_owner`, `set_label`, `touch(bridge_id, status: dict, fcm_token: str | None)`, `unpair`,
  `create_pair_code(bridge_id) -> (code, expires_at)` (8 digits, `secrets.randbelow`, 10 min,
  `bridgePairCodes/{code}`), `consume_pair_code(code) -> str | None` (transaction: read + delete),
  `list_expired_pair_codes(now)`. `bridgeauth.py`: `mint_token(bridge_id) -> (token, hash)`,
  `verify(token) -> Bridge | None` (`split(".", 1)`, sha256, `hmac.compare_digest` on bytes; a
  row with `tokenHash` null never verifies), FastAPI dependency `require_bridge(request) -> Bridge`
  (401 `{"detail": "unauthorized"}` uniformly, including a malformed header).
  `routers/bridge.py`: `POST /bridge/pair` (limiter bucket `bridge` **and** the global
  `bridge_pair:global` 10/min bucket, design decision 2; body `{code, version,
  accounts: [str], simNumber?: str, voiceNumber?: str, caps: {sms, gchat, gvoice}}`; 404 on a
  bad/expired code; normalizes `simNumber` with `externals_store.normalize_phone` (422 on
  garbage); stores status; sets the bridge's top-level `simNumber` and calls
  `users_store.set_sms_number(owner, sim)` when it differs, catching `SmsNumberTaken` →
  `status.error = "sim number belongs to @alias"` and log `bridge sim taken bridge=… holder=…`;
  on success `book.rederive_sms_contacts(owner, request.app.state.broker)`; returns `{bridgeId,
  token}`), `POST /bridge/heartbeat` (`require_bridge`; `{status, fcmToken?}` → `touch`; a
  reported `status.simNumber` different from the accepted `simNumber` sets `status.error = "SIM
  changed"` and **never** calls `set_sms_number` (decision 3); 204). Register the router in
  `main.py` next to `webhooks.router`. `jobs.sweep`: delete expired `bridgePairCodes` (there is no
  setup-code sweep to piggyback on today; `consume_pair_code` rejects an expired code anyway).
- **Tests:** store CRUD; pair happy path returns a token that `verify` accepts; a reused code is
  404; wrong token 401; token of an unpaired bridge 401; pair sets the owner's `smsNumber` and
  pushes a book bump (FakeBroker); heartbeat updates `lastSeenAt`/`status`; a heartbeat with a
  different SIM sets `status.error` and leaves `smsNumber` untouched; `SmsNumberTaken` recorded in
  `status.error` and the user untouched; 11 pair calls in a minute from different
  `X-Forwarded-For` values → the 11th is 429.
- **Verify:** common relay verify.

### B2 Outbox store + `bridge` backend kind + Twilio transport switch
- **Read:** decisions 4, 7 (backend row bullet), 11; `relay/app/backends/base.py`,
  `relay/app/backends/sms_twilio.py` (`deliver`, `_render_body`), `relay/app/backends/registry.py`,
  `relay/app/store/backends.py` (`_live_kind`, `_owner_is_external`, `BackendKind`),
  `relay/app/store/messages.py` (`mark_delivery_sent_if_queued`, `mark_delivery_failed_if_queued`,
  `record_delivery_attempt`, `list_recent_queued_by_kind`), `relay/app/jobs.py` `tick`,
  `relay/app/backends/webapp.py` (`FCMClient`), `relay/app/alerts.py` (`get_fcm_client`),
  `relay/tests/test_sms_twilio.py` (fixtures `outbound`, `world`).
- **Files:** new `relay/app/store/bridge_outbox.py`, new `relay/app/backends/bridge.py`,
  `relay/app/backends/sms_twilio.py`, `relay/app/backends/registry.py`, `relay/app/store/backends.py`,
  `relay/app/routers/bridge.py`, `relay/app/jobs.py`, `relay/app/store/externals.py`
  (`ensure_bridge_backend`), `relay/app/store/messages.py` (`error` kwarg,
  `set_delivery_external_id`), new `relay/tests/test_bridge_outbox.py`, `relay/tests/test_sms_twilio.py`
  (transport switch cases), `relay/tests/test_jobs.py` (stale outbox).
- **Do:** `store/bridge_outbox.py`: `OutboxItem` model, `enqueue_send(bridge, msg, bid, *, source,
  to: dict, text, reply_hint=None) -> str` (id `ob_<msgId>_<bid>`, `create()`, `AlreadyExists` →
  return the id **and the existing row's `state`**; after a *new* create push FCM `{"kind":
  "outbox", "bridgeId": …}` to `bridge.fcmToken` via `alerts_module.get_fcm_client().send_data`
  when a token exists), `enqueue_hint(bridge, *, source, phone, text, wire_id) -> str` (id
  `ob_h_<wire_id>`, no `msgId`/`bid`, same create/FCM rule), `enqueue_inspect(bridge, link) -> str`
  (`ob_` + 8 hex), `list_pending(bridge_id, limit=20)`,
  `ack(bridge_id, ob_id, state, reason, tier) -> tuple[OutboxItem, bool] | None` (transaction
  `pending → sent|failed` only; returns the row and whether this call transitioned it, `None` if
  no such row), `fail_pending(bridge_id, reason) -> list[OutboxItem]` (used by unpair, B6), `list_stale_pending(older_than)`,
  `delete_acked_before(cutoff)`. `backends/bridge.py` `BridgeBackend` (`kind = "bridge"`,
  `BridgeConfig {bridgeId, source, conversationId?, link?}`): `deliver` loads the bridge (`None` or
  unpaired → `failed` `no_bridge`), checks `bridgeConversations` row `paused` → `failed` `paused`
  (B4 adds the row; until then skip), enqueues with `to={conversationId, link}` and `text =
  msg.body` (a `loc`/`loc_req` kind → `failed` `unsupported`), returns `ok=True, state="queued",
  external_id=ob_id`; `render_state` per decision 7. `sms_twilio.deliver`: after the
  `no_sms_number` check, `bridge = bridges_store.get_by_sms_number(from_number)`; if paired and
  `caps.sms`: `source = (backend.config.get("via") or {}).get(msg.senderUid) or "sms"`, demoted to
  `"sms"` unless `bridge.caps.gvoice`; enqueue `to={"phone": phone}` plus `"conversationId":
  config["voiceConv"][senderUid]` when `source == "gvoice"`, `text=_render_body(msg)`; then
  `messages_store.set_delivery_external_id(msg.id, backend.id, ob_id)`; map the returned state
  (`pending` → `ok=True, state="queued"`; `sent` → `mark_delivery_sent_if_queued`, `ok=True,
  state="sent"`; `failed` → `mark_delivery_failed_if_queued`, `ok=False, state="failed"`); log
  `sms out … status=queued code=bridge` — all **before** the consent gate. `store/backends.py`: `"bridge"`
  joins `BackendKind`; `_live_kind` treats it like `sms` (valid only on an external); add
  `has_kind(uid, kind) -> bool`. `externals.ensure_bridge_backend(user, config) -> str` (fixed id
  `bridge`). `registry`: `"bridge": BridgeBackend()`. `routers/bridge.py`: `GET /bridge/outbox?
  wait=` (`require_bridge`; `wait` default 0; loop `await run_in_threadpool(list_pending)`, then
  `await asyncio.sleep(2)` — never a sleep inside the threadpool — up to `min(wait, 25)`; `{items:
  [...]}`), `POST /bridge/outbox/{ob_id}/ack` (`{state, reason?, tier}`; unknown id → 404; then,
  whether or not this call transitioned the row and only when the row has a `msgId`: row `sent`
  → `mark_delivery_sent_if_queued(msgId, bid)`, row `failed` → `mark_delivery_failed_if_queued(...,
  error=row.reason)` (add `error` kwarg to that helper, default unchanged); `tier == 2` on a
  transition → increment `status.tier2Count`; log `bridge out bridge= ob= state= tier= ms=`; 204).
  `jobs.tick`: for **every** bridge (`list_all`), `list_stale_pending(24 h)` → `ack(failed,
  "bridge_offline", tier=0)` + the delivery failed; in the existing `sms` retry loop skip a
  delivery whose `externalId` starts with `ob_`; `jobs.sweep`: `delete_acked_before(7 d)`.
- **Tests:** enqueue idempotent on the deterministic id; FCM data sent once per new item (use a
  `RecordingFCM`); Twilio path untouched when the sender's number is not a bridge number (existing
  tests pass unchanged); bridge path: no consent row needed, no disclosure, text is the raw body,
  delivery stays `queued` with `externalId == ob_…`, `redeliver` twice → one outbox row; ack
  `sent` → delivery `sent`; a second ack after resetting the delivery to `queued` by hand
  re-applies `sent`; ack `failed` → `failed` with `error`; tick fails a 25-h-old pending row and
  does not redeliver a bridge-queued `sms` delivery (11 of them do not block a Twilio retry);
  outbox long-poll returns at once when an item exists.
- **Verify:** common relay verify.

### B3 Inbound text core shared by Twilio and the bridge; bridge events (sms, gvoice)
- **Read:** decision 5 (sms/gvoice bullets); `relay/app/routers/webhooks.py` `_handle_inbound_sms`
  (whole), `relay/app/store/held_sms.py`, `relay/app/alerts.py` `sms_held_upsert`,
  `relay/app/store/externals.py` (`get_family_contact`, `normalize_phone`), `relay/app/store/
  backends.py` `update_backend`, `relay/tests/test_sms_twilio.py` (inbound cases, keep green).
- **Files:** new `relay/app/inbound_text.py`, `relay/app/routers/webhooks.py`,
  `relay/app/routers/bridge.py`, `relay/app/store/backends.py`, new `relay/tests/test_bridge_events.py`.
- **Do:** `inbound_text.py`: `handle_text(target: User, from_number: str, raw_body: str, sid: str,
  routing: Routing, *, reply: Callable[[str], None], attachments: int = 0, via: str = "sms") ->
  str` = the Twilio handler from the *blocked* step to the end (blocked, duplicate, empty →
  `[photo]`/`[attachment]` by `attachments` and `via`, known contact → deliver (`too_long` →
  `reply(sms_twilio.too_long_hint())`), else held + `sms_held_upsert`). On a delivered text,
  when `config.via[target.uid] != via` (or, for `gvoice`, `config.voiceConv[target.uid] !=
  voice_conv`), `backends_store.update_backend(contact.uid, bid, config={**config, "via": {**via_map,
  target.uid: via}, "voiceConv": {...}})` — keyed by the member, never one value per contact
  (design decision 5). `handle_text` gains `voice_conv: str | None = None`. `webhooks._handle_inbound_sms` keeps To-resolution + keywords and then
  returns `handle_text(..., reply=lambda t: sms_client.send_sms(from, t, from_number=to),
  via="sms")`. `routers/bridge.py` `POST /bridge/events` (`require_bridge`; pydantic `BridgeEvent`
  per decision 5 with its bounds as pydantic constraints (422); per-bridge limiter
  `rate_limits_store.check_and_increment(f"bridge_events:{bridge.id}", limit=120, window_s=60)`
  once per event before processing, 429 for the whole batch when it trips; `{results: [{id,
  outcome}]}`; `run_in_threadpool`): for `sms`/`gvoice` with `kind` absent or `"message"`:
  `conversation.isGroup` → `dropped_group`; target = `users_store.get_user(bridge.ownerUid)` (missing/disabled/
  no family → `dropped_owner`), `caps` check (`dropped_cap`), `from_number` from `sender.phone`
  (bad or absent → `dropped_bad_from`), `sid = f"br_{bridge.id}_{event.id}"`, `reply` =
  `bridge_outbox.enqueue_hint(bridge, source=event.source, phone=from_number, text=t,
  wire_id=sid)` (deterministic, so a retried batch does not text the hint twice), `voice_conv =
  event.conversation.id` for `gvoice`, outcome from `handle_text`. `gchat` events → B4 (return `dropped_unsupported` until then). One
  INFO line per event as decision 5.
- **Tests:** Twilio inbound tests unchanged; bridge SMS from a known contact → pager `/down`
  (FakeBroker) and `config.via == "sms"`; same number via `gvoice` → same contact, `via ==
  "gvoice"`, and the next outbound send is enqueued with `source == "gvoice"` and that
  `conversationId`; a sibling's later SIM text from the same number leaves the first member's
  `via` alone; an `sms` event with `isGroup: true` → `dropped_group`; 121 events in a minute →
  429; unknown number →
  held + `sms_unknown` alert (push body contains the text); duplicate event id → `duplicate`;
  attachments-only → `[photo]`; too long → hint enqueued on the outbox once even when the batch is
  posted twice, nothing stored.
- **Verify:** common relay verify.

### B4 Chat conversations: seen rows, held chat, `chat_unknown` alert, events
- **Read:** decisions 5 (gchat bullet), 6, 8; `relay/app/store/alerts.py` (`Alert`, `AlertKind`,
  `find_open`), `relay/app/alerts.py`, `relay/app/backends/webapp.py` (`_ALERT_TITLES`,
  `push_alert`), `relay/app/store/held_sms.py` (copy its shape), `relay/app/routers/family.py`
  (`dismiss_alert`, `block_alert`, `_mark_held`), `relay/tests/test_alerts.py`.
- **Files:** new `relay/app/store/bridge_conversations.py`, new `relay/app/store/held_chat.py`,
  `relay/app/store/alerts.py`, `relay/app/alerts.py`, `relay/app/backends/webapp.py`,
  `relay/app/routers/bridge.py`, `relay/app/routers/family.py` (dismiss/block on the new kind),
  `relay/app/jobs.py` (sweep `heldChat`), new `relay/tests/test_bridge_chat_events.py`,
  `relay/tests/test_alerts.py`.
- **Do:** `store/bridge_conversations.py`: `BridgeConversation` model (decision 6, plus `ref`),
  `conv_ref(conversation_id) = sha256(conversation_id)[:16]`, `row_id(bridge_id, conversation_id)
  = f"{bridge_id}_{conv_ref(conversation_id)}"`, `count_created_since(bridge_id, since)` (for the
  50-per-day new-row cap → `dropped_conv_cap`), `upsert_seen(bridge, event) -> BridgeConversation`
  (transaction: create or
  update `title/isGroup/link/people (append sender, ≤64)/lastPreview/lastAt`; never changes
  `status`), `get`, `list_for_owner(owner_uid, statuses)`, `set_status`, `set_fields`. `store/
  held_chat.py`: `HeldChat` model, `create` (`create()`, False on exists), `list_held(bridge_id,
  conversation_id, status)`, `count_held`, `set_status`, `HELD_CAP = 25`. `store/alerts.py`: add
  `"chat_unknown"` to `AlertKind`; fields `bridgeId`, `conversationId`, `convRef`, `convTitle`, `isGroup`,
  `people: list[str]`, `source` (all optional); `find_open(..., bridge_conv=None)`. `alerts.py`:
  `chat_held_upsert(family_id, target, row, body, sender_name) -> str` mirroring `sms_held_upsert`
  (`pushBody = f"{title} ({sender}) → @{alias}: {body}"`). `webapp._ALERT_TITLES["chat_unknown"]`.
  Events: `gchat` branch in `POST /bridge/events`: `inspect` → `upsert_seen` only
  (`inspectedAt`), outcome `inspected`; message → row = `upsert_seen`; `subscribed` → B5's
  `deliver_subscribed` (until B5 lands: `held`); `ignored`/`paused` → dropped; else `count_held`
  cap → `held_cap`; `held_chat.create` (id `f"br_{bridge.id}_{event.id}"`, the same string as the
  live `wire_id`, decision 5) → `duplicate` if exists;
  `chat_held_upsert`; `set_fields(heldCount, alertId)`. `family.py`: `dismiss` on `chat_unknown`
  marks `heldChat` rows `dismissed`; `block` → 400 `use Ignore for a Google Chat conversation`;
  `approve` → 400 `subscribe this conversation under People → Google Chat`. `jobs.sweep`: `heldChat`
  by `receivedAt` with the messages TTL.
- **Tests:** first message from an unknown group → one open alert with `people == [sender]`,
  `heldCount 1`, FCM push body carries title + text; second message updates the same alert
  (`heldCount 2`, `people` grows), no new doc; inspect event creates a `seen` row and no alert;
  ignored row → `dropped_ignored` and nothing stored; cap at 25; dismiss marks held rows.
- **Verify:** common relay verify.

### B5 Subscribe / ignore / patch / unsubscribe; routing and policy changes
- **Read:** decisions 7, 9, 10; `relay/app/routing.py` (`send`, `_send_group`,
  `_sms_route_reject`, `_create_and_deliver`), `relay/app/policy.py`, `relay/app/store/
  conversations.py` (`create_group`), `relay/app/store/externals.py` (`get_or_create`, `_reserve`,
  `delete`), `relay/app/book.py` (`validate_nick`, `entries_for`, `bump_and_push`), `relay/app/
  devcfg.py` (`_approved_contacts`), `relay/app/wire.py` (`is_valid_alias`), `relay/app/ingest.py`
  (the §4.2 case-3 reply table), `relay/app/routers/family.py` (`_approve_sms_unknown` for the
  idempotent-steps style), `relay/tests/test_routing.py`, `relay/tests/test_policy.py`,
  `relay/tests/test_book.py`.
- **Files:** `relay/app/routing.py`, `relay/app/policy.py`, `relay/app/store/conversations.py`,
  `relay/app/store/messages.py` (`Conversation.bridge: dict | None`, `Conversation.roster:
  dict[str, str] = {}`; `extra="ignore"` drops them otherwise),
  `relay/app/routers/conversations.py` (409 on add/leave for a bridge group),
  `relay/app/store/externals.py`, `relay/app/store/users.py` (`User.chat: dict | None`),
  `relay/app/book.py`, `relay/app/devcfg.py`, `relay/app/ingest.py`, new `relay/app/chat_subscribe.py`,
  new `relay/app/routers/family_bridges.py` (design O8)
  mounted under the family router prefix, `relay/app/routers/bridge.py` (subscribed delivery),
  new `relay/tests/test_chat_subscribe.py`, `relay/tests/test_routing.py`, `relay/tests/test_policy.py`,
  `relay/tests/test_book.py`, `relay/tests/test_devcfg.py`.
- **Do:** `routing.send(..., sender_alias=None)` → `_send_group`/`_create_and_deliver` use it when
  given. `_send_group`: skip an external only if `not backends_store.has_kind(uid, "bridge")`
  (both the sender-is-external and recipient checks), and for any pair with an external side call
  `_sms_route_reject(sender_uid, recipient_uid)` before `_policy_reject_reason` (reject → append
  to `rejected`, like the policy branch). `_sms_route_reject`: external with a `bridge` backend →
  person must be `bridges_store.get(config.bridgeId).ownerUid` and the bridge paired, else
  `no_bridge`; person → external with `chat.canReply is False` → `not_allowed`; else existing SMS
  branch. Add `"no_bridge"` to `RejectReason`. `policy._peer_kind(user)`: `"person"` when
  `user.kind == "external" and getattr(user, "chat", None)`; `check` uses it. `ingest` case-3
  reply `no_bridge` → `bridge not set up; ask your admin`. `externals.get_or_create_chat(family_id,
  bridge_id, conversation_id, display_name, *, source, link, is_group, title, can_reply) -> User`
  (`h = sha256(f"{bridge_id}|{conversation_id}").hexdigest()`, uid `"x_c" + h[:16]`, alias `"c" +
  h[:11]` — 12 characters, inside `ALIAS_RE`; name reservation as `get_or_create`, `chat` field,
  `ensure_bridge_backend`; idempotent like `get_or_create`). `conversations.create_bridge_group(...)`
  (decision 7; convKey `"g_c" + h[:14]`, alias `"bc" + h[:8]`, both `create()` in one transaction,
  `AlreadyExists` on the conv doc → return it; fields `bridge`, `roster`; `get_bridge_group(conv_key)`, `update_bridge_group
  (name/roster)`, `delete_bridge_group` (doc + alias)). `chat_subscribe.py`: `subscribe(family_id,
  bridge, conversation_id, pager_name, can_reply, roster, by_uid, broker, routing) ->
  SubscribeResult(uid, convKey, delivered, undelivered)` with the decision-7 steps in order;
  `deliver_subscribed(bridge, row, event, routing)` (used by B4's events: `routing.send(sender_uid=
  row.uid, recipient_alias=<group alias|owner alias>, …, sender_alias=<roster nick for event.sender.
  name, else slug_nick(name, taken=roster) and auto-add to roster>)`); `slug_nick(name, taken) ->
  str`: NFKD, drop non-ASCII, lowercase, runs of `[^a-z0-9]` → `-`, strip `-`, cut to 16; empty →
  `"p" + sha256(name)[:6]`; on collision cut to 13 and append `-2`, `-3`, …; result must pass
  `wire.is_valid_alias`; `ignore`, `patch`, `unsubscribe`. Routes
  (family admin or super, write limiter): `GET /api/family/members/{uid}/chat` → `{subscribed:
  [row + onPager], seen: [rows seen|ignored]}` (`onPager` = the entry's alias is among the first
  `devcfg.MAX_PULL_CONTACTS` of `devcfg._ordered_contacts(owner, <owner's first device's default
  alias or None>)`); `POST /api/family/bridges/{b}/conversations/{ref}/subscribe`, `/ignore`,
  `PATCH`, `DELETE` (`ref` = `bridge_conversations.conv_ref`, 404 when no row); `POST /api/family/bridges/{b}/inspect {link}` (422 on a
  non-Google link; `enqueue_inspect`; 202). `book.entries_for`: externals with `chat` are listed
  from the owner's out-edges (not `sms_contacts_for`) with `chat: {source}` on `BookEntry`,
  `sendable` by `policy.check` + bridge paired; `devcfg._approved_contacts` emits `t:"chat"` for
  them; a group with `bridge` set is `sendable` only while the bridge is paired, the row is not
  `paused` and the external's `chat.canReply` is true. `rederive_sms_contacts` must not put chat
  externals into `cfg.sms` (already true: `sms_contacts_for` requires `phone`). `routers/family.py`:
  `_approved_out`, `put_approved` and `_list_contacts` skip externals with `chat` set (listing,
  the full-replacement delete loop, and 404 when a request names one) — otherwise saving the
  ApprovedEditor silently deletes every subscribe edge.
- **Tests:** subscribe a group → external + group doc with `uids == [owner, external]`, edge,
  book bump (broker nudge recorded), held backlog delivered oldest first with `sndr` = nick on the
  FakeBroker `/down`, alert handled; subscribe a DM → external only, book entry `t:"chat"`;
  pager `/up to:<group alias>` → one outbox item with `conversationId` and `link`; `canReply:false`
  → owner's send rejected `not_allowed`; policy `people` owner can still receive (person peer
  kind); paused → inbound dropped, outbound `failed paused`; unsubscribe removes docs and edges,
  messages remain; retry of subscribe after a simulated crash (call twice) is idempotent; roster
  nick with a space → 422; duplicate pager name → 409; `no_bridge` reply text when the bridge is
  unpaired, for both a DM and a group `/up`; a sibling added to the bridge group by API → 409 and
  a sibling's `/up to:<bc alias>` → `not_member`; a sibling with policy `open` sending `/up
  to:<chat external alias>` → `no_bridge`; `PUT /members/{uid}/approved` with no contacts keeps the
  subscribe edge; `canReply:false` keeps inbound delivery working under the default `people` policy;
  a roster-less sender named `奶奶` gets a valid `p…` nick.
- **Verify:** common relay verify.

### B6 Family bridges API (list / create / reassign / unpair) + rules
- **Read:** decisions 1–2, 15; `relay/app/routers/family.py` (`FamilyScope`, `_require_family_member`,
  `create_device`), `relay/firestore.rules`, `relay/tests/test_rules.py`, `relay/tests/test_family_router.py`.
- **Files:** `relay/app/routers/family_bridges.py` (design O8), `relay/firestore.rules`,
  `relay/tests/test_family_bridges.py`, `relay/tests/test_rules.py`.
- **Do:** `GET /api/family/bridges` → rows (never `tokenHash`); `POST /api/family/bridges
  {ownerUid, label}` → `{bridge, code, expiresAt}` (owner must be a non-disabled person of the
  family; refuse a second SIM bridge per decision 1); responses never carry `tokenHash` or
  `fcmToken`; `POST /api/family/bridges/{id}/code` → a new
  code (only while `tokenHash` is null or after unpair); `PATCH /api/family/bridges/{id} {ownerUid?,
  label?}` (reassign: clear the old owner's `smsNumber` if it equals `simNumber`, set the new
  owner's, same `SmsNumberTaken` handling, then `book.rederive_sms_contacts` for both);
  `POST /api/family/bridges/{id}/accept-sim` (status `simNumber` becomes the accepted one, same
  handling, clears `status.error`); `DELETE` = unpair (decision 2: `bridge_outbox.fail_pending(id,
  "unpaired")` + each delivery failed, rederive). Rules: **no** block for `bridges`,
  `bridgePairCodes`, `bridgeConversations`, `heldChat`, `bridges/*/outbox` (all server-only,
  design decision 1).
- **Tests:** router cases above; unpair fails a pending outbox row and its delivery; rules
  (`test_bridge_collections_are_default_deny`): nobody — family admin, member, other family's admin,
  unauthenticated — reads or writes `bridges/{b}`, `bridges/{b}/outbox/{o}`, `bridgePairCodes/{c}`,
  `bridgeConversations/{r}` or `heldChat/{h}`.
- **Verify:** common relay verify.

### B7 Simulator + compose
- **Read:** decision 14; `tools/mocks/Dockerfile`, `relay/docker-compose.yml` (relay env; `twilio-mock` removed 9 Oct 2026).
- **Files:** new `tools/bridge_sim.py`, new `tools/mocks/bridge_sim.Dockerfile` (or extend the
  mocks Dockerfile with a second image), `relay/docker-compose.yml`, `relay/README.md` (one
  paragraph under "End-to-end test scenarios"; D1 writes the rest).
- **Do:** FastAPI app on `:8020` with a background thread that: pairs on start when `BRIDGE_SIM_
  PAIR_CODE` is set or via `POST /_pair {code}`; long-polls `GET /bridge/outbox`, records each item
  under `/_outbox` and acks `sent` (tier 1) unless `/_fail_next` armed; heartbeats every 30 s with
  a fixed status (`simNumber` from `BRIDGE_SIM_SIM_NUMBER`, default `+15550007777`,
  `accounts: ["kid@example.com"]`, all caps true); `POST /_inject {event}` forwards one event to
  `POST /bridge/events` and returns the relay's result; `POST /_reset`. Compose service
  `bridge-sim` with `RELAY_URL=http://relay:8000`; relay `depends_on` it.
- **Verify:** `cd relay && docker compose up -d --build bridge-sim && curl -s localhost:8020/_outbox`
  returns `[]`; `python3 -I tools/bridge_sim.py --help`.

### B8 End-to-end scenario
- **Read:** `tools/e2e_v2.py` (`scenario_relay_sms`, `make_device`, `wait_until`, `ServerClient`
  helpers in `tools/pager_client.py`), decision 14.
- **Files:** `tools/e2e_v2.py`, `tools/pager_client.py` (helpers `family_create_bridge`,
  `family_list_bridges`, `family_member_chat`, `family_chat_subscribe`, `family_bridge_inspect`).
- **Do:** `scenario_bridge()` registered in `SCENARIOS`: admin creates @bkid + device + bridge →
  `POST localhost:8020/_pair {code}` → wait until the bridge row has `lastSeenAt` and @bkid's
  `smsNumber == +15550007777`; create contact BridgeMom, approve for @bkid; inject an `sms` event
  from BridgeMom → pager `/down`; pager `/up to:<mom alias>` → sim `/_outbox` has it with `source
  sms`; inject a `gvoice` event from BridgeMom → pager; next pager reply lands with `source
  gvoice`; inject two `gchat` group messages (`Soccer carpool`, senders `Dana P`, `Lee`) → one
  `chat_unknown` alert, nothing on the pager; subscribe with roster nicks `dana`, `lee` → two
  pages with `sndr` on the pager in order; pager `/up to:<bc alias>` → sim outbox with
  `conversationId`; inspect by link → seen row appears; ignore a second conversation → further
  injects `dropped_ignored`.
- **Verify:** `cd relay && docker compose up -d --build && cd .. && python3 tools/e2e_v2.py bridge`
  passes; the full `python3 tools/e2e_v2.py` still passes.

### B9 Relay README + .env.example touch (backend-dev, small)
- **Files:** `relay/.env.example` (no new secrets; note that bridges need no env), `relay/README.md`
  "Message backends": a "Bridge phones" subsection pointer to D1's runbook.
- **Verify:** `ruff` clean; nothing else.

---

## Web (web-dev) — after B4; B5/B6 contracts as specified above

### W1 Types, API helpers, delivery chips
- **Read:** decisions 7, 8, 11, 12; `web/lib/types.ts`, `web/lib/api.ts`, `web/components/
  DeliveryChips.tsx`, `web/lib/book.ts`.
- **Files:** `web/lib/types.ts` (`AlertKind` + `"chat_unknown"`, `AlertDoc` new optional fields,
  `BackendKind` + `"bridge"`, new `BridgeRow`, `BridgeConversationRow`, `ChatTabOut`,
  `SubscribeRequest`), new `web/lib/bridges.ts` (typed wrappers over `api.*` for every B5/B6
  route + `slugNick(name, taken: Set<string>)`), `web/components/DeliveryChips.tsx` (`bridge`:
  "waiting for the phone" / "sent on Google Chat"), `web/lib/book.ts` (`chat?: {source}` on entries).
- **Verify:** web verify.

### W2 Family → Devices: Bridge phones
- **Read:** `web/app/family/devices/page.tsx`, `web/app/admin/devices/SetupCodePanel.tsx` (copy the
  panel's look), decision 12 and 13's setup checklist.
- **Files:** `web/app/family/devices/page.tsx`, new `web/components/BridgePhonesSection.tsx`, new
  `web/components/BridgePairPanel.tsx`.
- **Do:** section below the devices table; "Add bridge phone" dialog (member select from the
  family's persons, label) → pair panel (code in large type, expiry countdown, the checklist as a
  numbered list: sign in to Google, notifications on for Chat and Voice, Notification access,
  Default SMS app, battery exemption, Accessibility, screen lock None, DND off, charge limiter);
  rows with chips (`listenerBound`, `smsDefault`, `accessibility`, last seen red when > 15 min),
  Reassign (member select), Unpair (confirm), New code (when unpaired). Poll the list every 10 s
  while the page is open.
- **Verify:** web verify.

### W3 `/family/chat` page + Subscribe dialog
- **Read:** `web/app/settings/book/page.tsx` (member picker), `web/components/MemberDrawer.tsx`,
  `web/components/ApprovedEditor.tsx` (table style), decision 12.
- **Files:** new `web/app/family/chat/page.tsx`, new `web/components/ChatSubscribeDialog.tsx`,
  new `web/components/ChatRosterEditor.tsx`, `web/components/MemberDrawer.tsx` ("Google Chat…"
  button → `/family/chat?uid=`), `web/components/AppShell.tsx` (Family menu entry "Google Chat").
- **Do:** page: member picker (admins), "Subscribed" table (name on pager, Group/DM, people seen,
  last message, "Not on pager" chip; menu Rename, Edit roster, Pause/Resume, Unsubscribe with
  confirm), "Seen, not subscribed" list (title, Group/DM, people, last message, Subscribe, Ignore;
  "Show ignored" switch), "Add by link" (TextField + Inspect → 202 → poll `GET …/chat` every 3 s for
  30 s until a row with that link appears → open the dialog). Dialog: name on pager (default
  `title.slice(0,16)`, counter, inline 409 text), "Kid can reply" switch, roster table (name,
  nick default `slugNick`, validation `/^[a-z0-9][a-z0-9_-]{0,15}$/`, unique), helper text about
  people who have not spoken; on success a toast "<name> is on @kid's pager; N waiting messages
  delivered".
- **Verify:** web verify.

### W4 Alerts: `chat_unknown` card
- **Read:** `web/components/AlertCard.tsx`, `web/app/family/alerts/page.tsx`.
- **Files:** `web/components/AlertCard.tsx`, `web/app/family/alerts/page.tsx`.
- **Do:** icon + label "Google Chat"; body: title, Group (N people) or DM, people chips, newest
  text, "N messages waiting"; actions Subscribe (opens `ChatSubscribeDialog` with the alert's
  bridgeId/conversationId; success decides the alert), Ignore (`POST …/ignore`), Dismiss.
- **Verify:** web verify.

### W5 Address book chips + README
- **Files:** `web/app/settings/book/page.tsx` ("Google Chat" / "Google Voice" chip from
  `entry.chat?.source`), `web/components/NewChatDialog.tsx` (chat externals listed with the
  SMS group when sendable), `web/README.md` (manual checklist rows for W2–W4).
- **Verify:** web verify.

---

## Android (general-purpose agent; the Mac has adb + JDK 25 only)

### A1 Toolchain + project skeleton
- **Read:** decision 13; `.gitignore`.
- **Files:** new `bridge-android/` (`settings.gradle.kts`, `build.gradle.kts`, `gradle.properties`,
  `gradle/libs.versions.toml`, `gradlew`, `gradlew.bat`, `gradle/wrapper/*`, `app/build.gradle.kts`,
  `app/src/main/AndroidManifest.xml`, `app/proguard-rules.pro`), `.gitignore` (`bridge-android/
  .gradle`, `bridge-android/app/build`, `bridge-android/local.properties`,
  `bridge-android/app/google-services.json`).
- **Do:** `brew install --cask android-commandlinetools && brew install openjdk@17 gradle`;
  `export JAVA_HOME=$(/usr/libexec/java_home -v 17)`; `sdkmanager --install "platform-tools"
  "platforms;android-35" "build-tools;35.0.0"` and `yes | sdkmanager --licenses`;
  `local.properties` with `sdk.dir`; `gradle wrapper --gradle-version 8.10.2`; AGP 8.7.x, Kotlin
  2.0.x, `compileSdk 35`, `minSdk 26`, `targetSdk 35`, `applicationId "app.kidpager.bridge"`,
  Jetpack: core-ktx, appcompat, material, lifecycle, work-runtime-ktx, room (ksp), security-crypto,
  okhttp, kotlinx-serialization-json, firebase-messaging (BOM). Apply `com.google.gms.google-services`
  **only if** `app/google-services.json` exists (`if (file("google-services.json").exists())`),
  and set `buildConfigField("boolean", "FCM", …)` accordingly. Empty `MainActivity` compiles.
- **Verify:** `cd bridge-android && JAVA_HOME=$(/usr/libexec/java_home -v 17) ./gradlew
  assembleDebug` produces `app/build/outputs/apk/debug/app-debug.apk`.

### A2 Relay client, token storage, setup screen, foreground service, heartbeat
- **Read:** decisions 2, 11, 13; `docs/BRIDGE_PHONE_DESIGN.md` request shapes.
- **Files:** `app/src/main/java/app/kidpager/bridge/{RelayClient.kt, Prefs.kt, SetupActivity.kt,
  LogActivity.kt, Log.kt, BridgeService.kt, BootReceiver.kt, HeartbeatWorker.kt, Status.kt}`,
  `res/layout/activity_setup.xml`, `res/layout/activity_log.xml`, manifest entries.
- **Do:** `RelayClient` (OkHttp, JSON via kotlinx; `pair`, `events`, `outbox(wait)`, `ack`,
  `heartbeat`; bearer header; retries with backoff on 5xx/IO; 401 → `Prefs.clearToken()` and a
  persistent "re-pair" notification). `Prefs`: `EncryptedSharedPreferences` for `relayUrl`,
  `token`, `bridgeId`. `Status` gathers battery (`BatteryManager`), listener bound (`NotificationManager
  Compat.getEnabledListenerPackages`), default SMS (`RoleManager.isRoleHeld`), accessibility
  enabled, accounts (`AccountManager.getAccountsByType("com.google")` → emails), SIM number
  (`SubscriptionManager` / `TelephonyManager.line1Number`, may be empty → setup screen asks for it
  by hand), app version. Setup screen per decision 13; log screen. `BridgeService`: foreground
  with a low-priority notification, starts the outbox worker (A5), schedules `HeartbeatWorker`
  (15 min `PeriodicWorkRequest` as a backstop) and an in-service 5-min timer; `BootReceiver`
  starts it.
- **Verify:** `./gradlew assembleDebug`; `adb install -r` on a phone, pair against the compose
  relay (`adb reverse tcp:8000 tcp:8000`, relay URL `http://localhost:8000`), the Devices row
  shows `lastSeenAt` within a minute.

### A3 Notification listener, message extraction, reply cache, tier 1
- **Read:** decision 13 listener bullet; Android docs for `NotificationListenerService`,
  `NotificationCompat.MessagingStyle`, `RemoteInput`.
- **Files:** `ChatNotificationListener.kt`, `ReplyCache.kt`, `Dedup.kt` (Room entity + DAO),
  `Events.kt` (event model → relay JSON), manifest (`BIND_NOTIFICATION_LISTENER_SERVICE`).
- **Do:** per decision 13; events batched (debounce 500 ms) to `POST /bridge/events`; on
  `onListenerConnected` rebuild the cache; `source` = `gchat` for dynamite, `gvoice` for Voice;
  for Voice set `sender.phone` when the sender/title normalizes to a number; conversation `link`
  left null (the relay stores links from subscribe/inspect). `ReplyCache.reply(conversationId,
  text): Boolean` fires tier 1.
- **Verify:** `./gradlew assembleDebug`; on the phone: a Chat DM to the signed-in account appears
  in the log screen with sender/text and reaches `POST /bridge/events` (relay log `bridge in`).

### A4 Default SMS app: receive, MMS text, send
- **Files:** `SmsReceiver.kt`, `MmsReceiver.kt`, `SmsSender.kt`, `HeadlessSmsSendService.kt`,
  `ComposeActivity.kt` (stub for `ACTION_SENDTO`), manifest (`RECEIVE_SMS`, `SEND_SMS`,
  `READ_SMS`, `RECEIVE_MMS`, `RECEIVE_WAP_PUSH`, role intents).
- **Do:** per decision 13; inbound → event `source: sms` with `sender.phone`; `SmsSender.send(to,
  text, subscriptionId?)` with sent/delivered intents → ack `sent` on the sent intent, `failed`
  with the result code on failure; multipart; `[photo]`/attachments from MMS parts (text part
  delivered, media kinds reported).
- **Verify:** `./gradlew assembleDebug`; on the phone with a SIM: a text from another phone reaches
  the relay; an outbox send arrives at the other phone.

### A5 Outbox worker, FCM, tier dispatch
- **Files:** `OutboxWorker.kt`, `FcmService.kt` (`FirebaseMessagingService`, guarded by
  `BuildConfig.FCM`), `Dispatcher.kt`.
- **Do:** loop: FCM wake (then `wait=0`) or, poll-only builds, a 60-s timer (30–300 s setting) with `wait=0` (design O2: no long-poll from the phone) → per
  item, dispatch on `source`: `sms` → A4 with `to.phone`; `gchat`/`gvoice` → `ReplyCache.reply(to.
  conversationId)` (tier 1) else A6 (tier 2) with `link`; `inspect` → A6; ack with `tier`; failures `no_link`, `ui_changed`, `sms_<code>`. Register
  the FCM token in the heartbeat body.
- **Verify:** `./gradlew assembleDebug`; pager → Chat DM reply observed on another Chat account;
  relay log `bridge out … tier=1`.

### A6 Accessibility tier 2 + inspect
- **Files:** `BridgeAccessibilityService.kt`, `res/xml/accessibility_service_config.xml`, manifest.
- **Do:** per decision 13; a `sendViaUi(link, text): Result` and `inspect(link): Conversation`
  with the node searches written against both Chat and Voice; always finish with HOME; 20-s
  overall timeout.
- **Verify:** `./gradlew assembleDebug`; on the phone: a pasted Chat DM link → inspect event
  reaches the relay; a reply to a thread whose notification was dismissed and the app force-stopped
  arrives in Chat, relay log `tier=2`.

### A7 Firebase registration
- **Do:** `firebase apps:create android app.kidpager.bridge --project <prod project>` (needs the
  owner's Firebase login; `firebase apps:sdkconfig android <appId>` → `app/google-services.json`).
  If the CLI is not logged in or the command is refused, **stop, leave `FCM=false`**, and record in
  the report that the owner must create the Android app in the Firebase console and drop the file
  in place; the poll-only build is fully functional with ≤60 s outbound latency.
- **Verify:** `./gradlew assembleDebug` with the file present builds with `FCM=true`.

---

## Docs (docs-writer; after everything lands)

### D1 Runbook + indexes
- **Files:** `relay/README.md` ("Bridge phones": pairing, the phone checklist from decision 13 —
  screen lock None, DND off, charge limiter, adb over Wi-Fi — and the simulator), `docs/README.md`
  (index rows for `BRIDGE_PHONE_DESIGN.md` and `BRIDGE_PHONE_TASKS.md`), `README.md` Status
  (bridge phone, Chat/Voice/SIM SMS), `docs/OVERVIEW.md` (a "Bridge phone" section: what it is,
  what it replaces; SMS is bridge-only since 9 Oct 2026), `docs/ROADMAP.md` (Next: Twilio removal, done 9 Oct 2026, last at 05ec3ed; Android app items: Voice number detection, MMS media, multi-account
  Chat), `docs/SERVER_PLAN.md` §6.5 note + §10 D4 answered, `docs/RELAY_SMS_DESIGN.md` one
  italic note on decision 1/3 pointing at the bridge transport, `web/README.md` checklist (W5
  did the rows; D1 links the design), new `bridge-android/README.md` (build, sideload, pairing,
  what each permission is for).
- **Verify:** every link resolves (`grep -o '\](\S*\.md' docs/README.md | sort -u` and `ls`).

### D2 PROTOCOL.md
- **Do:** §4.2 case-3 list gains `bridge not set up; ask your admin`; §3.2 `c[]` row gets an
  italic *(8 Oct 2026: `t:"chat"` is also emitted for a bridged Google Chat contact,
  docs/BRIDGE_PHONE_DESIGN.md)*; nothing else.
- **Verify:** `git diff --stat docs/PROTOCOL.md` touches only those lines.
