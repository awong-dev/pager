# SERVER_PLAN.md — server stack: users, backends, location, web app, Firebase + serverless GCP

> **Status: this describes what is built.** It is the design reference for everything on the server
> side — the data model, the routing engine, the delivery backends, the web app, the test client
> and the GCP/Terraform layout — and the record of why each piece is shaped the way it is.
>
> `docs/PROTOCOL.md` stays authoritative for anything the device sees: any wire change edits that
> file *first*, then the code, and §4 below only explains the server's side of those decisions.
>
> Firmware work this design implies is **listed but not scheduled** (§11). The device side of every
> feature here is exercised by the Python test client (§8) until real firmware catches up.

---

## 1. Goals and non-goals

**Goals (from the brief, restated as testable requirements)**

| # | Requirement | Where it lands |
|---|---|---|
| G1 | User registry. A device authenticates *as a user* and sends messages to other users. | §3 data model, §4 wire (`from`/`to` aliases), §5.3 auth |
| G2 | Device may address different recipients, constrained by a **server-side allow-list**. | §3 `allow`, §5.4 routing |
| G3 | Device publishes periodic location; **the device chooses the interval**. | §4.3 `/loc`, §5.6 |
| G4 | A recipient can request an immediate location; device answers subject to its own rate limit. | §4.2 `kind:"loc_req"`, §4.3, §5.6 |
| B1 | A user registers **multiple** message backends; delivery fans out to all enabled ones. | §3 `backends`, embedded `deliveries`; §5.4, §6 |
| B2 | Backends: Google Chat, SMS, Web app at minimum. | §6.2–§6.4 |
| B3 | Adding Email, Slack, iMessage later must not require touching the core. | §6.1 adapter contract, §6.5 |
| W1 | Web app is the native interface for reading and sending. | §7 |
| W2 | Login by one-time code to email or phone. | §5.3, §7.3 (Firebase Auth email link / phone code) |
| W3 | Admin role: register users, configure allow-lists (and devices, backends). | §5.5, §7.5 |
| W4 | Users configure their own backends. | §7.4 |
| W5 | Chat view: a dialogue with one recipient. | §7.4 |
| W6 | Browser Notifications API for incoming messages. | §7.6 |
| W7 | Retention period for data and chat history, in days or weeks, swept weekly. | §5.7 |
| I1 | GCP deployment described in Terraform, **no always-on compute**. | §9 |
| T1 | Python shell test client speaking MQTT, driving a full end-to-end exercise. | §8 |

**Non-goals**
- Firmware changes (listed in §11 so nobody forgets them; scheduled separately once hardware is in hand).
- Group chats (more than two humans in one thread). The model in §3 is pairwise; a group is a later
  additive feature and the layout does not preclude it.
- Message content beyond text + location (no images, no attachments). The pager cannot render them.
- Multi-tenant SaaS. One deployment = one family/household with one admin. Keep it that small.

---

## 2. Architecture

```
 Browser ──── Firebase Hosting (CDN, free TLS) ── serves web/out (Next.js static export + SW)
   │  │            └── rewrites /api/** and /webhooks/** ──► Cloud Run relay (below)
   │  └── Firebase JS SDK: Auth (email link / phone), Firestore listeners (live thread), FCM (push)
   │
   ▼ HTTPS, Authorization: Bearer <Firebase ID token>
 ┌──────────────── Cloud Run relay — request-driven, min instances 0 ───────────────────────┐
 │ FastAPI  ── /api/*          (send, locate, backends, admin — every WRITE goes through here) │
 │          ── /webhooks/mqtt  (broker rule engine → HTTP: /up, /status, /loc)                 │
 │          ── /webhooks/*     (Twilio SMS, Google Chat)                                       │
 │          ── /internal/*     (Cloud Scheduler tick + weekly sweep, Cloud Tasks retries)      │
 │ Routing engine ──► deliveries ──► Backend adapters:                                         │
 │   (allow-list, fan-out)            pager  ── broker REST publish ──► Broker ─MQTT─► Device  │
 │                                    webapp (Firestore write + FCM push)                      │
 │                                    sms (Twilio REST), gchat (Chat API), email/slack… later  │
 └───────────────────────────┬────────────────────────────────────────────────────────────────┘
                             │ firebase-admin (service account, ADC)
                     ┌───────▼────────┐          ┌───────────────────────────────┐
                     │ Cloud Firestore│          │ MQTT broker with rule engine  │
                     │ (~MBs, free    │          │ EMQX Cloud Serverless (free)  │
                     │  quota)        │          │ rules: pager/+/{up,status,loc}│
                     └────────────────┘          │        → HTTPS webhook        │
                                                 │ REST: POST /publish (QoS 1)   │
                                                 └───────────────────────────────┘
 Cloud Scheduler: */5 min → /internal/tick ; weekly → /internal/sweep
 Cloud Tasks:     retry queue for failed adapter deliveries
```

**Decisions, with the one-line reason each** (same convention as `PROTOCOL.md`):

1. **No process holds an MQTT connection. The broker's rule engine is the subscriber.** Every
   device publish on `pager/+/up`, `/status`, `/loc` (including broker-generated LWTs) matches a
   rule whose action is an HTTPS POST to the relay; the relay publishes `/down` through the
   broker's REST publish API. The relay therefore only runs while a request is in flight, which
   is what makes "min instances 0" correct rather than a hack. Device firmware, topics, QoS,
   retained flags and ACLs are unchanged by it.
2. **Broker = EMQX Cloud Serverless**, because its free tier includes the rule engine with an HTTP action and the REST publish API; HiveMQ Cloud's free
   tier has neither. Local dev runs the open-source `emqx/emqx:5` image, which has the same rule
   engine and REST API, so the webhook path is tested end-to-end offline. Both are `UNVERIFIED`
   items in §10 D2 until a real account is opened.
3. **The relay is the only writer to Firestore.** Clients read Firestore directly under
   read-only security rules; every write goes through the relay API, keeping the allow-list,
   dedup and ack state machine in one tested place.
4. **The pager device is modelled as just another delivery backend** of its owner
   (`kind='pager'`). Routing has one code path: *sender user → recipient user → recipient's
   enabled backends*.
5. **Lean into Firebase for what it does for free**: Firebase Auth replaces login codes,
   sessions and SMTP; Firestore listeners replace SSE; FCM replaces hand-rolled VAPID push;
   Hosting serves the static site and fronts the API. Cloud Scheduler and Cloud Tasks replace the
   in-process sweeper thread and retry worker, because there is no long-lived process to host them.
6. **Firestore over RTDB**: its no-cost quota carries into the Blaze plan, it bills per
   operation rather than bytes (our operations are in the low thousands per day, §9.3), it has
   real queries, and delivery state can live *inside* the message document so a thread is one
   query.
7. **Timers are lazy or scheduled, never in-process.** Anything that would otherwise run on a
   timer inside the relay is either derived at read time (`expired`) or driven by a Cloud
   Scheduler tick (retry of queued publishes, weekly retention sweep).
8. **Device addressing uses short lowercase aliases on the wire** (`from`/`to`, ≤16 chars). The
   CardKB can type `@mom hi`; a UID cannot be typed. Aliases are unique per deployment and map to
   Firebase UIDs server-side; the wire never carries a UID.

---

## 3. Data model — Firestore collections

Shaped by three habits: **top-level collections** (collection-group and cross-conversation
queries stay simple), **denormalised arrays for rules** (`uids`, `locatableBy`) so security rules
need no lookups, and **transactions** for anything monotonic or unique.

```
users/{uid}                    {alias, displayName, email, phone, role: 'admin'|'member', disabled, createdAt}
aliases/{alias}                {uid}                                  uniqueness = doc id, created in a txn
users/{uid}/backends/{bid}     {kind: 'pager'|'webapp'|'sms'|'gchat'|…, config, enabled, verifiedAt}
allow/{fromUid}_{toUid}        {fromUid, toUid, message: bool, locate: bool}
devices/{deviceId}             {ownerUid, label, mqttUsername, defaultToUid|null, revokedAt,
                                authMode: 'hmac'|'password', provisionState: 'issued'|'provisioned',
                                wire: 'json'|'cbor'|null, bookVersion: int,
                                locatableBy: [uid…],                  derived from allow.locate
                                smsContacts: [{name, phone}…],        v0.2 §6, max 8, owner/admin-managed
                                status: {state, mode, battMv, rssi, session, ts, fw, locPeriodS, locMinS,
                                         authAlarm, updatedAt}}
devices/{deviceId}/locations/{autoId}
                               {ts, fixTs, lat, lon, accM, src, cached, reqId|null, createdAt}
devices/{deviceId}/smsLog/{logId}
                               {ts, smsTs, dir: 'out'|'in', peer, st: 'sent'|'failed'|'recv'|'blocked',
                                body, receivedAt}      v0.2 §6/§7 (device-direct SMS audit log; `logId` is
                                                        the device's own `sms_log` envelope `id`, `s_…`,
                                                        which is what makes a webhook redelivery idempotent
                                                        — dedup is "the same document id", not a separate
                                                        marker collection). Not a thread entry, not routed
                                                        to anyone; read via `GET /api/devices/{id}/sms-log`,
                                                        never straight from Firestore (§5.1's usual "the web
                                                        app reads this collection directly" does not apply
                                                        here — see the rules sketch below). No retention
                                                        sweep yet (§5.7 does not cover it) — a gap flagged,
                                                        not filled, by the task that added this collection.
deviceSecrets/{deviceId}       {hmacKey, mqttPasswordHash, upN, upBits, downN, sigFailures, createdAt, rotatedAt} [server-only]
setupCodes/{bid}               {deviceId, expiresAt}                  [server-only]
contactRequests/{deviceId}_{reqId}
                               {deviceId, ownerUid, name, phone|null, alias|null,
                                status: 'pending'|'approved'|'rejected', reason, createdAt, decidedAt, decidedBy}
messages/{id}                  {id: 'm_…', seq, convKey, uids: [a, b], senderUid, recipientUid,
                                kind: 'text'|'loc_req'|'loc', body|null, loc|null, wireId|null,
                                originBackendKind, originBackendId|null, ts, createdAt,
                                deliveries: { {bid}: {kind, state: 'queued'|'sent'|'shown'|'read'|
                                                       'fulfilled'|'failed'|'expired',
                                                       attempts, externalId, error, sentTs, shownTs, readTs} },
                                pendingDeviceIds: [deviceId…] }      derived: pager deliveries still queued/sent
wireIds/{wireId}_{recipientUid} {messageId}                            QoS 1 dedup (PROTOCOL §4.2)
locWireIds/{locId}             {deviceId, createdAt}                  /loc dedup (PROTOCOL §13.2)
locReqs/{deviceId}             {messageId, requesterUids: [uid…], createdAt}   the one in-flight request
conversations/{convKey}        {uids, lastMessageAt, lastPreview, unread: {uid: n}}   list-view summary
settings/retention             {messages: {n: 4, unit: 'weeks'}, locations: {n: 1, unit: 'weeks'}}
settings/meta                  {schemaVersion: 2, lastSweepAt, seqCounter}
```

- `convKey` = the two UIDs sorted and joined with `_`; `uids` is the same pair as an array so a
  rule can say `request.auth.uid in resource.data.uids`.
- **Thread order is `seq`**, a counter the relay increments in the message-creating transaction
  (`settings/meta.seqCounter`). The relay is the single writer, so contention is nil, and this
  preserves `PROTOCOL.md` §3.5's "insertion order, not `ts`". Composite index: `(convKey, seq)`.
- **Deliveries live inside the message document.** One query returns the thread *with* its
  per-backend states, halving reads (§9.3); a state change is a transaction on that document
  that refuses to go backwards (`PROTOCOL.md` §4.1 rules 1–2). Message documents stay far below
  the 1 MiB limit (three backends ≈ 1 kB).
- One message document per (sender, recipient) pair. A device up-message with no `to` and a
  broadcast default becomes N documents sharing `wireId`; dedup is `create()` of the `wireIds`
  document inside the same transaction, which fails if it exists.
- **`originBackendKind` is the origin adapter's *kind* (`'pager'`/`'webapp'`/…);
  `originBackendId` is the specific `users/{uid}/backends/{bid}` document it arrived through, or
  `null` when the origin has no such row.** Two fields, not one: a single field holding a *kind*
  string is unrecoverable once real per-user backend rows exist for a kind a user can have more
  than one of. `pager`/`webapp` sends pass `originBackendId: null` (a device's pager backend
  belongs to its owner, not necessarily the sender, and webapp has no row distinct from the user),
  relying on `originBackendKind` alone for §5.2's self-loop guard. The SMS and gchat adapters,
  where one user can have two backends of the same kind (two phones) and "reply back through the
  channel it arrived on" must name the exact one, pass a real `originBackendId`. Nothing here is
  device-visible
  (`PROTOCOL.md` is unaffected). See `relay/app/routing.py`'s module docstring for the full
  reasoning and `relay/tests/test_routing.py` for the id-scoped-exclusion coverage.
- `pendingDeviceIds` is what the online-edge re-publish queries (`array-contains deviceId`,
  ordered by `createdAt`, limit 10); `locReqs/{deviceId}` being a single document is what makes
  coalescing (§4.6) a transaction rather than a query.
- **`locWireIds/{locId}` is `/loc`'s own dedup marker**
  (`PROTOCOL.md` §13.2's "dedup on `id` … in the same transaction that stores the fix"), the same
  `transaction.create()`-or-`AlreadyExists` pattern as `wireIds`, kept separate because the two
  keys are shaped differently: a `wireIds` doc is keyed `{wireId}_{recipientUid}` (one up-message
  fans out to N recipients), while a `/loc` envelope has no recipient to key against. Additive to
  this table; nothing device-visible. §5.7's sweep must delete these alongside
  `locations`, the way it deletes `wireIds` alongside `messages`, or the collection grows forever.
- **Three inbound-lookup collections, additive to this table:**
  `phoneIndex/{e164Phone}` → `{uid, bid}`, `gchatSpaces/{spaceId}` → `{uid, bid}`, and
  `gchatLinkCodes/{code}` → `{uid, bid, expiresAt}`. §6.4/§6.5 specify the *behaviour* ("map
  `From` → user by verified phone", "stores the DM `space` name") but not the mechanism; a
  `config.phone` / `config.space` lookup would need a `COLLECTION_GROUP`-scoped index on a nested
  field, so these use the same "the doc id *is* the lookup key" trick as `aliases/{alias}`. Accepted
  as-is; three rules they must keep. (a) **Write on verify, never on create** — `phoneIndex` is
  written by `POST /api/me/backends/{id}/verify` only, so an unverified phone claim can never
  capture another user's inbound texts. (b) **The index is derived state and must be torn down with
  its source** — deleting an sms backend, or `PATCH`ing its `config.phone`, has to
  `clear_phone_index(oldPhone)` and clear `verifiedAt`, or a stale row keeps attributing inbound
  SMS to a `bid` that no longer exists. (c) **`phoneIndex` doc ids are normalised E.164**
  (`+15551234567`), matching what Twilio puts in `From`; a raw user-typed string both misses the
  lookup and can contain a `/`, which is not a legal Firestore document id. None of the three is
  client-readable: `firestore.rules` has no `match` block for them (default-deny read) and the
  `match /{document=**} { allow write: if false }` catch-all denies writes, which is the intended
  posture — no rule edit needed, but `relay/tests/test_rules.py` should pin it so a future
  broadened rule cannot expose them. Nothing device-visible; `PROTOCOL.md` is unaffected.
- **A fourth collection, `smsVerifyCodes/{bid}` → `{codeHash, expiresAt}`, because rule (a)
  alone is not enough.** Storing the SMS verification code in
  `users/{uid}/backends/{bid}.config` would put it somewhere a user can read for their own uid —
  so a user claiming a phone number they don't control could read the code straight out of
  Firestore and verify it without ever receiving the SMS.
  `smsVerifyCodes` holds the (hashed) code server-side instead, with the same default-deny
  posture as the other three lookup collections. This is *not* the same situation as
  `gchatLinkCodes`, which is correctly owner-readable (the user reads their own code to type it
  into the Chat DM) — only inbound *verification* material needs this extra collection. Also:
  `gchatSpaces` gained a `senderName` field (the identity of whoever sent the `/link` message),
  checked on every subsequent inbound message in that space so a group/shared space can't let a
  second person send as the originally-linked user.
- Indexes: `messages(convKey, seq)`, `messages(pendingDeviceIds array-contains, createdAt)`,
  `messages(createdAt)` for the sweep, `locations(createdAt)` collection-group for the sweep.
  Declared in `relay/firestore.indexes.json`. **`smsLog` needs none**: `GET /api/devices/{id}/
  sms-log`'s only query is `smsLog` (a single device's subcollection, not a collection-group)
  ordered by `ts` descending with an optional `where('ts', '<', before)` on that same field —
  Firestore's automatic single-field indexing already covers a range filter and an order-by on
  the same field, the same reason `locations`' own per-device reads need no index either (only
  its *collection-group* sweep query does).
- Security rules (sketch; the real file is `relay/firestore.rules`, deployed by CI):
  ```
  function registered() { return exists(/databases/$(db)/documents/users/$(request.auth.uid)); }
  function isAdmin()    { return request.auth.token.admin == true; }
  match /users/{uid}                 { allow read: if request.auth.uid == uid || isAdmin(); }
  match /users/{uid}/backends/{b}    { allow read: if request.auth.uid == uid || isAdmin(); }
  match /messages/{id}               { allow read: if registered() && request.auth.uid in resource.data.uids; }
  match /conversations/{k}           { allow read: if registered() && request.auth.uid in resource.data.uids; }
  match /devices/{d}                 { allow read: if resource.data.ownerUid == request.auth.uid
                                                   || request.auth.uid in resource.data.locatableBy || isAdmin();
    match /locations/{l}             { allow read: if request.auth.uid in get(/databases/$(db)/documents/devices/$(d)).data.locatableBy; }
    match /smsLog/{l}                { allow read: if isAdmin()
                                                   || get(/databases/$(db)/documents/devices/$(d)).data.ownerUid == request.auth.uid; } }
  match /allow/{e}                   { allow read: if isAdmin() || request.auth.uid in [resource.data.fromUid, resource.data.toUid]; }
  match /contactRequests/{e}         { allow read: if isAdmin() || request.auth.uid == resource.data.ownerUid; }
  match /settings/{s}                { allow read: if registered(); }
  match /deviceSecrets/{d}           { }  /* server-only, no client reads */
  match /setupCodes/{b}              { }  /* server-only, no client reads */
  match /{document=**}               { allow write: if false; }
  ```
  Clients never write; the `admin` claim is a Firebase custom claim. `deviceSecrets` and `setupCodes` are server-only collections with no `match` block (default-deny read) and the catch-all write deny.
  **`smsLog` is deliberately narrower than `locations`**: a `locate`-permission `locatableBy` uid
  may read a device's location fixes but not its SMS audit log — a location grant says nothing
  about who should see a kid's texts. In practice the web app reads this log through
  `GET /api/devices/{id}/sms-log` (`relay/app/routers/devices.py`), not straight from Firestore
  (unlike `locations`, which §5.1 lists as a direct-Firestore read); the rule exists anyway so the
  boundary holds independently of the API route, per this file's own "the rule, not just the code"
  posture. `smsContacts` needs no separate rule: it is a plain field on the already owner/admin/
  locatableBy-readable `devices/{d}` document (a known, accepted trade-off — a `locate`-only uid
  can incidentally read it too, since Firestore rules cannot restrict one field within a document
  read differently from its siblings; the write side is unaffected, still denied to every client by
  the blanket rule above).
- **Schema versioning**: `settings/meta.schemaVersion`. There is no SQL-style migration
  mechanism; a schema change is a code change plus, if it needs one, a one-off Cloud Run job.

---

## 4. What the wire carries

`docs/PROTOCOL.md` is authoritative for every byte the device sees; this section records only why
the server side needs each piece and what it does with it. Everything here is additive to the
original text-only wire and relies on PROTOCOL.md §3.1's rule that unknown fields are ignored. Byte
budgets are against §3.3's 640-byte hard limit, which does not move.

### 4.1 `from` is the sender's alias
`from` is `^[a-z0-9][a-z0-9_-]{0,15}$` **or** the literal `system` (PROTOCOL.md §3.1). A deployment
has named users rather than one parent and one student, so the alias identifies the sender.
`msg.c` accepts any 1–16 byte string and renders it verbatim (`firmware/main/msg.c:270`), so this
costs the firmware nothing.

### 4.2 `kind` on the down envelope
`kind`: `"msg"` (default when absent) | `"loc_req"` (PROTOCOL.md §3.2). A `loc_req` has no `body`,
`ack:null`, and `from` = the requesting user's alias. The device does not ack or render it; it
answers on `/loc` with `req` set to the request's `id`. Relay-side lifecycle:
`queued → sent → fulfilled` (a `/loc` with matching `req` arrived) or `expired`, derived at read
time 15 minutes after creation. Never re-published on an online edge (§5.3) — a stale location
request is worthless.

*Why a field on `/down` rather than a `/cmd` topic:* PROTOCOL.md §2 gives the device exactly one
subscription, and a second subscription on a metered link is the thing that rule exists to prevent.

Firmware that predates `kind` treats a bodyless down message as malformed and drops it without
acking (PROTOCOL.md §3.4). That is correct for such firmware — the request simply expires.

### 4.3 The `pager/{device_id}/loc` topic
Device → relay. QoS **1** when `req` is non-null (an answer someone is waiting on), QoS **0**
otherwise; retained false. Schema and field table: PROTOCOL.md §13.2. The broker ACL lets the
device publish it and a broker rule forwards it to the relay's webhook.

Periodic at 15 min ≈ 96 × ~0.3 kB ≈ 29 kB/day of cellular data — irrelevant against PROTOCOL.md
§7.3's cap. **The real cost is GNSS power, not data**; that is §11's problem.

### 4.4 `to` on up messages
Optional, same alias regex as `from`. Absent → the server uses `devices.defaultToUid`, or
broadcasts to every user the owner is allowed to message. An unknown or disallowed `to` is dropped,
logged as a security event, and answered with one `system` down message (`"unknown recipient"`) so
the student gets feedback rather than silence.

### 4.5 `loc_period_s` and `loc_min_s` on `/status`
The device-chosen periodic interval (`0` = off) and the device's own minimum gap between on-demand
fixes. **Display and diagnosis only** — the server never writes them and never assumes a default
when they are absent.

### 4.6 The location rate limit
Normative in PROTOCOL.md §13.3 so that firmware and the test client agree. Device side:

- A `loc_req` arriving less than `loc_min_s` (default **120 s**) after the last fix *attempt* is
  answered from the last fix with `cached:true`, immediately, without powering GNSS.
- Otherwise the device attempts a fix, bounded by **60 s**, then answers (`err:"no_fix"` on
  timeout). At most one fix attempt is in flight; a second request during it gets the same result.

The server mirrors it (§5.6): at most one in-flight `loc_req` per device (`locReqs/{deviceId}`); a
second requester within the window **attaches to the existing request** so both get the answer, and
a request within 60 s of a fulfilled one is answered from `locations` with `cached:true`, without
touching the device at all.

### 4.7 Budget lines
PROTOCOL.md §7.2/§7.3 carry a `/loc` line, and §11 of this document tracks the GNSS power cost. The SIM's SMS budget line stays at "0 used": the SMS *backend* is server-side Twilio,
never the modem.

### 4.8 The relay is not an MQTT client
The relay's *role* is what PROTOCOL.md describes, but its *transport* is the broker's rule engine
(HTTPS push of `/up`, `/status`, `/loc`) and the broker's REST publish API — not a persistent MQTT
session. `sent` means "the broker's publish API accepted the QoS 1 message", which is the same fact
PUBACK reported. The §5.3 "re-publish on online edge" is triggered by the `/status` webhook.
Nothing the device sees changes.

---

## 5. Backend (Python) design

Package stays `relay/app/`; the top-level directory name stays `relay/` to avoid churning every
path in the docs. New layout:

```
relay/app/
  main.py            app factory; mounts routers; /healthz
  config.py          Settings (env only) — grows; see relay/.env.example
  db/firestore.py    firebase-admin init (emulator-aware), typed collection helpers, txn helpers;
  store/             users.py, devices.py, backends.py, allow.py, messages.py, locations.py, settings.py,
                     sms.py (devices/{id}/smsLog/{logId} — v0.2 §6/§7, device-direct SMS audit log)
  wire.py            + kind, to, alias regex, LocEnvelope
  broker.py          BrokerClient: publish(topic, payload, qos, retain) over the broker REST API;
                     verify_webhook(request); parse_webhook(body) → (topic, payload, qos)
  ingest.py          what mqtt_gateway.py was: /up acks + replies, /status (+ online-edge
                     re-publish), /loc — called by the webhook router, unit-tested with fake payloads
  routing.py         allow-list check, recipient resolution, fan-out → deliveries → adapters
  backends/          base.py (Protocol), pager.py, webapp.py, sms_twilio.py, gchat.py, registry.py
  auth.py            verify Firebase ID token → user; require_user / require_admin (custom claim)
  location.py        loc_req lifecycle, coalescing, cached answers
  jobs.py            tick(): retry queued publishes + failed adapter deliveries; sweep(): retention
  tasks.py           Cloud Tasks enqueue (prod) / inline thread (dev) for delivery retries
  routers/           me.py, conversations.py, admin.py, devices.py (GET /api/devices, owner/admin
                     sms-contacts + sms-log — v0.2 §6), webhooks.py (mqtt, twilio, gchat),
                     internal.py (tick, sweep, task handler — OIDC-authenticated), dev.py, legacy.py
  notify/            sms.py (Twilio) — used by the sms backend and its link flow
```
Deliberately absent: `mqtt_transport.py` (paho), a background thread, the reconnect/backoff
logic and `RELAY_KEEPALIVE_S`. `fake_transport.py` becomes a fake `BrokerClient` that records
publishes and lets tests inject webhook payloads.

### 5.1 API surface (JSON; `Authorization: Bearer <Firebase ID token>` unless noted)
Reads that the web app can do straight from Firestore (thread, contacts, locations, status, own
backends, settings) have **no** API endpoint. The API is writes plus anything that needs a secret.
```
GET  /api/me                                           → {user, role, claims refreshed}
POST/PATCH/DELETE /api/me/backends[/{id}]              → user's own backends
POST /api/me/backends/{id}/verify {code}               → phone / gchat link verification
POST /api/me/push-tokens {token} / DELETE …/{token}    → FCM registration tokens
POST /api/conversations/{alias}/messages {body}        → 201 {id}
POST /api/conversations/{alias}/messages/{id}/read     → webapp delivery → 'read'
POST /api/conversations/{alias}/locate                 → 202 {request_id} (requires allow.locate)
POST/PATCH/DELETE /api/admin/users[/{uid}]             → admin claim; creates the Auth user too
POST /api/admin/users/{uid}/backends {kind, config}    → admin-created backend (verifiedAt set, adminVerified)
PUT  /api/admin/allowlist                              → replace-all; rewrites allow/* and devices.locatableBy
POST/DELETE /api/admin/devices[/{id}]                  → POST {device, setupCode, expiresAt, brokerPush, manualAcl}
POST /api/admin/devices/{id}/rotate-credentials        → {device, setupCode, expiresAt, brokerPush, manualAcl}
POST /api/admin/devices/{id}/revoke                    → revoke and delete broker credential
POST /api/admin/devices/{id}/cfg {lock: {clear?, auto?}} → push cfg down message
GET  /api/admin/contacts?status=pending                → list pending contact requests
POST /api/admin/contacts/{key}/approve {mode, alias?, locate?} → approve and create user/backend if needed
POST /api/admin/contacts/{key}/reject {reason}         → reject request
PUT  /api/admin/settings                               → retention {n, unit} per class
GET  /api/devices                                      → caller's own devices: [{id, label, status}]
GET  /api/devices/{id}/sms-contacts                    → {contacts: [{name, phone}], pending}  (v0.2 §6)
PUT  /api/devices/{id}/sms-contacts {contacts}         → validate, store, push cfg.sms; same response shape
GET  /api/devices/{id}/sms-log?limit=&before=          → {entries: [{id, ts, smsTs, dir, peer, name, st, body}]}
POST /webhooks/mqtt                                    → broker rule engine; shared-secret header
POST /webhooks/twilio/sms                              → Twilio signature-validated
POST /webhooks/gchat                                   → Google-issued JWT-validated
POST /internal/tick, /internal/sweep, /internal/task   → Cloud Scheduler / Cloud Tasks; OIDC token
GET  /healthz                                          → 200 + firestore reachable + broker API reachable
POST /api/dev/token {uid}                              → DEV_MODE=1 only: custom token for the test client
```

### 5.2 Routing engine (`routing.py`)
```
send(sender, recipient_alias | None, kind, body, origin_backend, wire_id=None):
  1. resolve recipients: explicit alias → [uid] ; None → device default or broadcast set
  2. for each recipient: assert allow/{sender}_{recipient}.message else drop+log+system-reply
  3. one Firestore transaction per recipient: create wireIds/{wire}_{recipient} (dedup; aborts
     if present), increment seqCounter, create messages/{id} with a 'queued' delivery per enabled
     backend (excluding origin_backend) and pendingDeviceIds, update conversations/{convKey}
  4. deliver inline, in-request: pager → broker REST publish (→ 'sent' on 2xx); webapp → FCM;
     sms/gchat → provider call. Any failure leaves the delivery 'queued'/'failed' with attempts+1
     and enqueues a Cloud Tasks retry (backoff 30 s … 15 min, max 5) → 'failed'
```
`origin_backend` above is shorthand for the actual `(origin_backend_kind, origin_backend_id)`
pair — see the `originBackendKind`/`originBackendId` bullet in
§3 for why the two are stored (and passed) separately. "Excluding origin backend" is what stops an SMS reply from being echoed back to the same phone.
Step 3 being one transaction is what guarantees a crash never leaves a message without its
deliveries. Inline delivery in step 4 keeps the parent→pager path at one HTTP hop plus one broker
publish — no queue in the latency-critical path.

### 5.3 Auth — Firebase Auth
- **Sign-in methods enabled**: email link (passwordless) and phone (SMS code). Both are "a code
  sent to email or phone" from the user's point of view. Firebase sends the email; no SMTP.
- The relay verifies the ID token with `firebase_admin.auth.verify_id_token` in a dependency;
  `role=admin` is a **custom claim** set by the admin API (and refreshed on `/api/me`), so rules
  and the relay agree on who is admin without a lookup.
- **Registry gate**: Firebase Auth will happily create an account for any email that clicks a
  link. The relay's dependency rejects any UID with no `users/{uid}` document, and the admin
  creates users *by email/phone* ahead of time (`auth.create_user`), so a stranger who signs in
  gets a 403 and no data. Rules enforce the same (`registered()`).
- Rate limiting of code sends is Firebase's job (it has abuse protection for phone auth).
- Bootstrap: the first deploy runs `python -m app.bootstrap --admin-email …` (a Cloud Run job,
  also in Terraform) — there is no startup hook to rely on when instances come and go.
- Device auth is the broker's job (username/password per device, PROTOCOL §2 ACLs); the relay
  trusts `device_id` from the webhook's topic exactly as it trusted the MQTT topic, and the
  webhook itself is authenticated by a shared secret header (`X-Relay-Webhook-Key`, from Secret
  Manager) so nobody else can post "device traffic".

### 5.4 Allow-list semantics
Directed edges with two flags at `allow/{from}_{to}`. `message` gates `send()`; `locate` gates
`/locate` and read access to `locations` (via `devices.locatableBy`, rewritten whenever the list
changes). The admin UI's "connect A and B" writes both directions; the API keeps them separately
editable. Removing an edge does not delete history; the thread goes read-only.

### 5.5 Admin
Everything an admin does is an API call so the test client can drive it. Creating a device
returns the generated MQTT password once and stores only a hash; the same call is what the future
flash-time provisioning tool (`PROTOCOL.md` §12 item 5) will consume. If the broker's REST API
exposes credential and ACL management (EMQX does; whether the Serverless tier exposes it is D2),
the relay pushes the device credential and its three ACL rules to the broker in the same call;
otherwise the admin UI shows "add these to the broker" copy.

### 5.6 Location (`location.py`)
- `/loc` ingest (via webhook): validate (`LocEnvelope`), dedup on `wireId`, add to
  `devices/{d}/locations`; if `req` set → transaction: mark the `loc_req` delivery `fulfilled`,
  delete `locReqs/{d}`, add a `kind='loc'` message to each requester↔owner thread so the fix shows
  in chat. Periodic fixes are **not** thread messages; the contact card and map read `locations`.
- `/locate`: transaction on `locReqs/{d}` — if present and < 15 min old, append the requester and
  return its `messageId` (coalesced); if a fix < 60 s old exists, answer from `locations` with
  `cached:true`; else create the `loc_req` message and publish it through the pager backend
  (no pager → `409 no locatable device`).
- Expiry is **derived**: a `loc_req` delivery still `sent` after 15 min renders as `expired` (the
  web app computes it from `createdAt`; `/internal/tick` also clears stale `locReqs/{d}` docs so a
  new request is not coalesced onto a dead one).

### 5.7 Retention (`jobs.sweep`)
Settings are **a count plus a unit**, `{n, unit: 'days'|'weeks'}`, stored as given and converted
to seconds only in the sweeper. Cloud Scheduler calls `/internal/sweep` **once a week** (default
Sunday 03:00 in `TZ`). Because the sweep is weekly, a record lives for its configured period
**plus up to seven days**; the settings page says so.

| Class | Default | Hard cap | Setting |
|---|---|---|---|
| messages (+ their `wireIds`) | 4 weeks | 52 weeks | `retention.messages` |
| locations | 1 week | 52 weeks | `retention.locations` |
| device status | latest only (embedded in the device doc) | — | — |
| push tokens | removed after 3 consecutive FCM "unregistered" errors | — | — |

Mechanics: `messages where createdAt < cutoff orderBy createdAt limit 500`, `BulkWriter` deletes
of the documents and their `wireIds` docs, loop until empty; collection-group query on
`locations` the same way. Firestore returns only the matching documents, so a sweep's reads equal
its deletes (§9.3). The run is idempotent and resumable, so a Cloud Run request timeout mid-sweep
just means the next weekly run (or a manual `/internal/sweep`) finishes it. Deleting a user
removes every document keyed by their UID plus their conversations.
*(Firestore's built-in TTL policies could do this continuously instead of weekly; not used, per
the brief's "weekly sweep", but it is a one-line change if that preference flips.)*

### 5.8 Scheduled work (`jobs.tick`, every 5 min via Cloud Scheduler)
1. Retry pager deliveries still `queued` (broker publish failed) — at most 10 per device, oldest
   first, same cap as the online-edge re-publish.
2. Clear `locReqs/{d}` documents older than 15 min.
3. Drop push tokens past their error threshold.
Side effect: a request every 5 min keeps a warm instance around most of the time, which trims
cold starts on the latency-sensitive parent→pager path (§9.3). It is not a guarantee, and the
design does not depend on it.

### 5.9 Tests
- pytest runs against the **Firestore + Auth emulators** started by `docker compose` (the same
  containers CI uses; the Emulator Suite needs a JRE, so it gets its own small image,
  `relay/emulator.Dockerfile`). Each test clears the emulator via its REST endpoint.
- The broker is a fake `BrokerClient` in unit tests (records publishes; tests post webhook
  payloads straight to `ingest`). The real EMQX rule-engine path is covered by `e2e_v2.py` against
  the compose stack.
- HTTP adapters (Twilio, Chat) are tested against recorded request/response fixtures with the
  signature checks exercised for real; FCM sends go through a stub `messaging` client; Cloud
  Tasks runs in inline mode.
- Coverage called out for `backend-dev.md`: webhook auth, allow-list denial, fan-out to N
  backends, origin-backend exclusion, transaction atomicity (abort mid-transaction → no orphan
  message), loc_req coalescing and derived expiry, monotonic delivery transactions, tick retries,
  retention sweep with day and week units and with a mid-run abort.

---

## 6. Message backends

### 6.1 Adapter contract (`backends/base.py`)
```python
class Backend(Protocol):
    kind: ClassVar[str]
    config_schema: ClassVar[type[BaseModel]]          # validated per-user config
    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult: ...
    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None: ...   # e.g. send a code
    def complete_link(self, backend: BackendRow, proof: str) -> bool: ...
    def render_state(self, delivery: Delivery) -> str: ...  # "sent" / "shown" / "read" for the UI
```
Inbound (replies) is **not** part of the protocol: adapters that receive messages register a
webhook router and call `routing.send()` themselves, passing their own backend row as
`origin_backend`. A new backend = one module + one line in `registry.py` + a settings form in §7.4.

### 6.2 `pager` — the MQTT gateway, now over REST
`deliver()` builds the §3.2 down envelope with `from` = sender alias (or `system`) and `kind`
when non-default, and calls `BrokerClient.publish(topic, payload, qos=1, retain=False)`; a 2xx →
`sent`. Acks arrive through `/webhooks/mqtt` → `ingest.handle_up` → transaction on the message
document. The online-edge re-publish (`PROTOCOL.md` §5.3) runs inside the `/status` webhook:
query `pendingDeviceIds array-contains d`, oldest 10, publish each with its original `id`.

### 6.3 `webapp`
`deliver()` is nearly free: the message document written in routing step 3 *is* the delivery —
the browser's Firestore listener sees it immediately. The adapter's only work is FCM:
`messaging.send_each` to every token of the recipient with a data payload (`convKey`, `id`,
sender alias, body preview). Delivery goes `read` when the browser reports the thread viewed
(`POST …/messages/{id}/read`), so the sender sees "read" for the web too. Every user gets an
implicit `webapp` backend at creation.

### 6.4 `sms` — Twilio
Outbound: Messages API, `From` = the deployment's one Twilio number. Link flow: user enters phone
→ `start_link` sends a code → `complete_link`. Inbound webhook: validate `X-Twilio-Signature`;
map `From` → user by verified phone; resolve recipient with the rule *if the text starts with
`@alias ` use it, else if the user has exactly one allowed peer use that, else reply with a usage
hint by SMS*. Body limit 160 code points applies before `routing.send()`, so a long SMS is
rejected with a hint, never truncated. **Costs and chores to flag** (§10 D3): number rental, per-
segment fees, and US A2P 10DLC / toll-free verification, which can take days and is a manual
registration outside Terraform. (This number is for the *SMS backend*; login-code SMS is sent by
Firebase Auth and does not need it.)

### 6.5 `gchat` — Google Chat app
Outbound: `spaces.messages.create` with the relay's service account (Chat API enabled by
Terraform; the Chat app itself is configured in the console — there is no Terraform resource for
it). Link flow: the user opens a DM with the app and sends `/link 123456`; the inbound webhook
(`/webhooks/gchat`, Google-signed JWT verified) stores the DM `space` name in the backend config.
Inbound messages use the same `@alias`/single-peer resolution as SMS. **Unverified prerequisite**
(§10 D4): Chat apps require the *user* to be on a Google Workspace account; consumer Gmail accounts
cannot add third-party Chat apps as of the last time this was checked. If the deployment is on
consumer Gmail this backend is dead on arrival and Email (§6.6) should take its slot.

### 6.6 Later backends — what the contract already allows
- **Email**: outbound via any SMTP or the Firebase "Trigger Email" extension; inbound needs a
  provider inbound-parse webhook (Mailgun/SendGrid) or a polled IMAP mailbox. Thread by `In-Reply-To`.
- **Slack**: bot token + Events API webhook; DM channel per linked user; identical shape to gchat.
- **iMessage**: no official API. Only via a Mac-hosted bridge (e.g. BlueBubbles) exposing HTTP —
  that bridge would be the "provider" and the adapter is again the gchat shape. Not planned.
- **Signal / WhatsApp**: signal-cli or the WhatsApp Business API; same adapter shape, both
  operationally heavier than the family use-case justifies.

---

## 7. Web app (`web/` — Next.js, React, MUI, TypeScript, Firebase JS SDK)

### 7.1 Stack and build
- Next.js (App Router, `output: 'export'`), React, MUI (latest major; pin it
  in `package.json`), `@mui/icons-material`, `firebase` (auth, firestore, messaging). No state
  library beyond React context; Firestore listeners are the state. Typecheck with `tsc`, lint
  with `eslint-config-next`.
- `web/out/` is deployed to **Firebase Hosting** by CI (`firebase deploy --only hosting`);
  `firebase.json` holds the rewrites for `/api/**` and `/webhooks/**` → the Cloud Run service.
  Dev: `npm run dev` on `:3000` with `next.config.js` rewrites to `http://localhost:8000`, and the
  Firebase SDK pointed at the emulators (`connectAuthEmulator`, `connectFirestoreEmulator`).
- PWA: `manifest.webmanifest`, icons, and `firebase-messaging-sw.js` (background push +
  notification click → focus/open `/chat/{alias}`). No PWA plugin.

### 7.2 Routes
```
/login                 email-or-phone → link/code → done (hand-built with MUI over Firebase Auth)
/                      redirects to /chat
/chat                  contact list from conversations/* (last message, unread badge) + device
                       online/battery from devices/* for pager owners
/chat/[alias]          thread (Firestore listener on messages where convKey==k orderBy seq desc limit 50,
                       "load older"); composer (160-cp counter); per-message state chip from the
                       embedded deliveries map (queued/sent/shown/read/failed/expired per backend);
                       "Request location" (if allow.locate); last known location card with "open in maps"
/settings/backends     list + add (SMS phone verify, Google Chat link code, Email later) + enable toggles
/settings/notifications  enable browser notifications → registers FCM token; test button
/admin/users           table; create (alias, name, email/phone, role); disable; delete
/admin/allowlist       matrix of users × users with message/locate checkboxes; save = PUT replace-all
/admin/devices         create device (shows MQTT credentials once), owner, default recipient,
                       revoke/rotate; last status
/admin/settings        retention: number + days/weeks selector per class; note about the weekly sweep
```

### 7.3 Auth UX
Single input accepting email or E.164 phone. Email → `sendSignInLinkToEmail`; the link lands on
`/login?finish=1` which completes sign-in. Phone → invisible reCAPTCHA + `signInWithPhoneNumber`
→ 6-digit code. After sign-in the app calls `/api/me`; a 403 (not in registry) shows "ask your
admin to add you" and signs out. Admin routes are hidden unless the `admin` claim is present
**and** enforced server-side and in the rules (the client is not the gate).

### 7.4 Chat + backends
The thread is one `onSnapshot` query; delivery states come with each document, so there are no
per-message listeners. Delivery chips read per backend so a parent sees "pager: shown 15:02 ·
sms: sent". `expired` for a location request is computed client-side from `createdAt`. Backend
forms are hand-written per kind (three kinds; no generic form builder).

### 7.5 Admin
Thin tables over §5.1's admin endpoints, reading current state from Firestore. The allow-list
matrix is the one piece of non-trivial UI; keep it a plain MUI `Table` with checkboxes and a
single Save.

### 7.6 Notifications
- Foreground: the Firestore listener fires; if `document.visibilityState !== 'visible'` or the
  thread is not the open one → `new Notification(...)`. Permission is asked from
  `/settings/notifications`, never on first load.
- Background: FCM `onBackgroundMessage` in the service worker → `showNotification`;
  `notificationclick` opens the thread.
- Location answers and expired requests also notify the requester.
- iOS Safari needs the PWA installed to the home screen for push; say so on the settings page.

### 7.7 Map
The location card shows lat/lon, accuracy, age, and an "Open in Google Maps / Apple Maps"
link. A Leaflet + OpenStreetMap tile map is an optional follow-up (§10 D9).

---

## 8. Test client and end-to-end suite (`tools/pager_client.py`, `tools/e2e_v2.py`)

`pager_client.py` is **both** a simulated device (MQTT, exactly as the real device speaks it —
the rule-engine bridge is invisible to it) and, optionally, a server driver (HTTP + Firestore), so
one process can play "the pager" and "the parent" and an e2e run needs nothing else. stdlib `cmd`
REPL; every command also works as a one-shot subcommand for scripting; `--json` for
machine-readable output.

**Device side (`--device-id`, `--host/--port/--username/--password`, `--tls`)**
```
connect / disconnect / crash        crash = new session id + reconnect (tests §5.3 re-publish)
inbox                               received down messages with their kinds
msg <text> | msg @<alias> <text>    publish an up message (with/without `to`)
ack <id> shown|read ; autoack on|off|shown-only
status [--batt N] [--mode sleep|active] [--rssi N]     publish /status (with loc_period_s/loc_min_s)
loc <lat> <lon> [acc]               one periodic fix now
loc auto <period_s> [--walk]        periodic fixes; --walk drifts the position ~1 m/s
loc min <s> ; loc fail on|off       rate-limit window; simulate no-fix
bytes                               per-topic byte counters (comparable to PROTOCOL §7)
```
`loc_req` handling implements §4.6 exactly (cached answer inside the window, one in-flight fix,
`err:"no_fix"` when `loc fail on`).

**Server side (`--api URL`, `--as <alias>`)**
Sign-in for scripts: `POST /api/dev/token` (DEV_MODE) mints a Firebase custom token, which the
client exchanges for an ID token at the Auth emulator's Identity Toolkit endpoint. Reads use the
Firestore REST API with that token, so the client also exercises the security rules.
```
login <alias>                       dev-mode custom-token sign-in
contacts ; chat <alias> [n] ; say <alias> <text> ; locate <alias> ; locations <alias>
watch <alias>                       poll the thread (Firestore REST has no streaming; 1 s poll)
tick ; sweep                        call /internal/tick and /internal/sweep (dev auth)
admin user-add <alias> <name> --email/--phone [--admin]
admin allow <a> <b> [--no-locate] [--one-way] ; admin deny <a> <b>
admin device-add <device_id> --owner <alias> [--default-to <alias>]   prints MQTT creds
admin settings retention messages=4w locations=10d
```

**`tools/e2e_v2.py`** imports the client as a library, brings up `relay/docker-compose.yml`
(EMQX OSS with the rule + webhook configured by `tools/emqx_setup.py`, Firebase emulators, relay
with `DEV_MODE=1`, Twilio mock), and runs named scenarios:
1. `bootstrap`: admin sign-in, create parent + student, connect them, create device (broker
   credential pushed via EMQX's REST API).
2. `text_roundtrip`: parent → student via API → pager receives → `shown`/`read` chips update;
   student `msg` → parent thread; student `msg @dad` to a second parent.
3. `allowlist`: student `msg @stranger` dropped, system reply received; admin denies parent↔student,
   send returns 403; an unregistered signed-in UID gets 403 and cannot read Firestore.
4. `republish`: parent sends while device disconnected → `crash` → message arrives once (via the
   `/status` webhook's online edge); relay publish with the broker API down → stays `queued` →
   `tick` delivers it.
5. `location_periodic`: `loc auto 5` → `locations` fills; a user without `locate` cannot read it.
6. `location_on_demand`: `locate` → device answers; second `locate` inside 60 s answered cached
   without a wire message; two requesters coalesce onto one `loc_req`; `loc fail on` → `no_fix`;
   derived expiry after a shortened `LOC_REQ_TTL_S` + `tick`.
7. `fanout`: parent has webapp + a fake `sms` backend (Twilio stubbed via `TWILIO_BASE_URL` pointing
   at the mock) → both deliveries recorded, origin backend excluded on reply; mock returns 500 →
   retry path → `failed` after max attempts.
8. `retention`: set `retention.locations=1d`, back-date fixes, `sweep`, documents gone; messages
   untouched; repeat with `2w` for messages; abort the sweep mid-run (small `SWEEP_BATCH`) and
   re-run to completion.
9. `bytes`: prints the per-exchange byte counts next to PROTOCOL §7.2's figures (informational).
This is what CI runs.

---

## 9. GCP with Terraform (`infra/`)

### 9.1 Layout
```
infra/
  bootstrap/           one-off: project APIs, GCS state bucket (local state, applied once)
  modules/
    firebase/          google-beta: firebase project, Firestore database (native mode), web app
                       (+ SDK config output), Identity Platform config (email-link + phone sign-in),
                       Hosting site (+ custom domain)
    relay-service/     Cloud Run v2 service (min 0), its SA (Firestore + FCM + Secret accessor),
                       Cloud Run *job* for bootstrap/import, Secret bindings
    schedule/          Cloud Scheduler jobs (tick, sweep) with OIDC to the relay; Cloud Tasks queue
    secrets/           Secret Manager secrets (values supplied out-of-band, never in tfvars)
    ci-deploy/         Workload Identity Federation pool/provider + SA for GitHub Actions
    broker-gce/        OPTIONAL fallback: e2-micro + EMQX OSS + certbot (see §9.4)
  envs/prod/           main.tf wiring the modules; terraform.tfvars.example
relay/firestore.rules, relay/firestore.indexes.json, web/firebase.json   deployed by `firebase deploy`
```

### 9.2 Key resource settings
- `google_cloud_run_v2_service`: `scaling { min_instance_count = 0  max_instance_count = 2 }`,
  request-based billing (CPU only during requests), `startup_cpu_boost = true`, concurrency 20,
  request timeout 300 s (the sweep is resumable, §5.7). Public ingress (Hosting rewrites and the
  broker webhook need it); `/internal/*` additionally requires a Google OIDC token from the
  Scheduler/Tasks service account; `/webhooks/mqtt` requires the shared secret. No VPC connector.
- `google_cloud_run_v2_job` for `bootstrap` (run once by hand).
- `google_cloud_scheduler_job` × 2: `*/5 * * * *` → `/internal/tick`; `0 3 * * 0` (in `TZ`) →
  `/internal/sweep`. `google_cloud_tasks_queue` with max 5 attempts, min backoff 30 s.
- `google_firestore_database` (native, `nam5` or the region nearest the family),
  `google_firebase_web_app` + config output, `google_identity_platform_config` with email
  (passwordless) and phone providers, `google_firebase_hosting_site` + custom domain.
- Secrets: `BROKER_API_KEY`, `BROKER_API_SECRET`, `WEBHOOK_KEY`, `TWILIO_*`, `GCHAT_*`. The
  Firestore/FCM credential is the Cloud Run service account itself (ADC) — no key file.
  Terraform creates the *containers*; values are added with `gcloud secrets versions add`.
- CI: GitHub Actions builds the image, pushes to Artifact Registry (prune to the last 3 tags —
  the no-cost allowance is 0.5 GB), `terraform apply` on `main` via WIF, then
  `firebase deploy --only hosting,firestore` for the static site, rules and indexes.
- **Billing plan**: Cloud Run and Scheduler require billing, which puts the Firebase project on
  **Blaze**. Firestore, Auth, FCM and Hosting keep their no-cost allowances on Blaze.

### 9.3 Sizes, operations and costs — recomputed for Firestore and scale-to-zero

**Firestore operations per day** (the quota that matters; no-cost allowance on Blaze: 50k reads,
20k writes, 20k deletes per day, 1 GiB stored, 10 GiB/month egress). Nominal / pessimistic use
`PROTOCOL.md` §7.3's profiles (25 / 120 messages a day) with periodic location every 15 / 5 min
and 3 web users.

| Operation | What | Nominal | Pessimistic |
|---|---|---|---|
| **Writes** | message txn (message + wireId + conversation) = 3 | 75 | 360 |
| | delivery updates (sent, shown, read; sms/webapp read) ≈ 4 per message | 100 | 480 |
| | device status (1 update each) | 34 | 60 |
| | location fixes | 96 | 288 |
| | **total writes** | **≈ 300** | **≈ 1 200** |
| **Reads** | web: open a thread (50 docs, deliveries included) × 3 users × 20 opens | 3 000 | 100 opens: 5 000 |
| | web: listener deltas (changed docs only) × open tabs | 300 | 1 500 |
| | web: contact list, devices, settings per open | 600 | 1 000 |
| | relay: per webhook / send (lookups + txn reads, ≈ 3) | 750 | 3 000 |
| | relay: tick every 5 min (2 small queries) | 600 | 600 |
| | **total reads** | **≈ 5 000** | **≈ 11 000** |
| **Deletes** | weekly sweep, worst day = a week's writes of the swept classes | 7 × (25 + 25 + 96) ≈ 1 000 | 7 × (120 + 120 + 288) ≈ 3 700 |

Headroom against the no-cost lines: writes 16–65×, reads 4.5–10×, deletes 5–20× (the delete
figure is the one day a week the sweep runs). If reads ever crowd 50k/day, the first lever is the
thread page size (50 → 30), the second is moving the listener to `conversations/*` summaries.

**Storage** (document ≈ field names + values; measured shapes from §3):

| Class | Per doc | Nominal | Pessimistic | Retention (default + ≤7 d lag) | Stored |
|---|---|---|---|---|---|
| messages (3 embedded deliveries) + wireId | 700 + 100 B | 20 kB/day | 96 kB/day | 4 weeks | **0.7 MB / 3.4 MB** |
| locations | 150 B | 14 kB/day | 43 kB/day | 1 week | **0.2 MB / 0.6 MB** |
| users, devices, allow, conversations, tokens | | | | — | < 0.05 MB |
| **Total** | | | | | **≈ 1 MB / ≈ 4 MB** — 0.1–0.4 % of 1 GiB |

Even the 52-week cap on both classes tops out near 45 MB pessimistic. Egress: thread opens
dominate at ≈ 60 × 50 × 0.7 kB ≈ 2 MB/day ≈ 60 MB/month, under 1 % of the 10 GiB line.

**Cloud Run**, request-based, min instances 0 (no-cost allowance: 2M requests, 180k vCPU-s,
360k GiB-s per month):

| Requests/month | Nominal | Pessimistic |
|---|---|---|
| broker webhooks (acks ≈ 2/msg + replies + status + loc) | ≈ 6 000 | ≈ 20 000 |
| web API writes (send, read receipts, locate, settings) | ≈ 3 000 | ≈ 12 000 |
| scheduler tick (every 5 min) + weekly sweep | 8 650 | 8 650 |
| **total** | **≈ 18 000** | **≈ 41 000** |
| CPU at ≈ 150 ms/request on 1 vCPU (cold starts add ≈ 3 s × maybe 500/month) | ≈ 4 200 vCPU-s | ≈ 7 700 vCPU-s |

That is 1–2 % of the request allowance and 2–4 % of the CPU allowance. **Cloud Run cost ≈ $0.**
Cold start on the Python image is the price paid instead of dollars: expect **2–4 s** on the first
request after an idle gap. The 5-minute tick keeps an instance warm most of the time, and the
latency-critical parent→pager path is a single request; measure it on a real deployment (§9.3) and,
if the active-mode 5 s target is being missed, the remedies are (in order) a smaller image, a lazier
`firebase-admin` import, and only then `min_instance_count = 1` (≈ $10–22/mo, which this design
exists to avoid).

**Monthly bill** (order of magnitude; verify against current pricing):

| Item | Approx. monthly |
|---|---|
| Cloud Run (request-based, within no-cost allowance) | **$0** |
| Firestore (≈ 4 MB stored, ≈ 1–11k reads/day, ≈ 1k writes/day, weekly delete burst) | **$0** |
| Cloud Scheduler (2 jobs; 3 free per billing account) | $0 |
| Cloud Tasks (≪ 1M ops) | $0 |
| Firebase Auth (email link free; phone within the no-cost allowance), FCM, Hosting | $0 |
| Artifact Registry (pruned), Secret Manager (≤ 6 active versions free), logging | $0 – $1 |
| EMQX Cloud Serverless: 1 device × 43 200 session-min/month + ≈ 7 MB traffic vs 1M min / 1 GB free | **$0** |
| Twilio number + SMS backend (only if enabled) | ~$1 + usage |
| **Total** | **≈ $0 – $2, plus Twilio if used** |

Versus the two earlier drafts: Cloud SQL (≈ $25–40) → RTDB + always-on Cloud Run (≈ $12–25) →
**this (≈ $0–2)**. The whole saving is the always-on instance, which the rule-engine bridge
makes unnecessary.

### 9.4 Broker
**Default: EMQX Cloud Serverless**, outside Terraform (no provider), configured once by hand and
documented in `infra/README.md`: three rules (`SELECT topic, payload, base64_encode(payload) as
payload_b64, qos, clientid FROM "pager/+/up"` etc.) → one HTTP action to
`https://<hosting-domain>/webhooks/mqtt` with the `X-Relay-Webhook-Key` header; an API key for
the relay's REST publishes; per-device authentication and the three ACL rules from
`PROTOCOL.md` §2. Device sees TLS on 8883 exactly as before and pins the broker's CA. `PROTOCOL.md` §12 item 2 (free-tier session limits) is
re-asked against EMQX's numbers: 1M session-minutes/month is ≈ 23 devices always connected, and
QoS 1 / retained / LWT / persistent sessions are all supported.

**`base64_encode(payload)` in that rule is not optional, and neither is `payload_encoding:
"base64"` on the relay's REST publishes.** The HTTP action's `${.}` body is JSON and EMQX
replaces every non-UTF-8 byte of `payload` with U+FFFD, so a rule that selects only `payload`
destroys every CBOR envelope (`PROTOCOL.md` §3.1), every HMAC signature (§14) and the encrypted
bootstrap blob (`DEVICE_PLAN.md` §3.3) before the relay ever sees them; `payload_b64` is the
field `broker.py`'s `parse_webhook` reads. `tools/emqx_setup.py` provisions exactly this rule
locally — a hand-configured production broker must match it.

What must be true, and checkable only by opening the free account (§10 D2): rule engine with an
HTTP action on the Serverless tier; REST publish API on the Serverless tier; webhook retry
behaviour when the relay is cold (a 2–4 s first-byte delay must not be treated as failure —
set the action timeout ≥ 15 s). If the Serverless tier lacks any of these, the fallback is the
`broker-gce` module: EMQX open source on a free-tier `e2-micro` (1 GB RAM is enough for one
family's traffic), which is always-on but costs ≈ $0–4/mo and runs the identical rule config as
the local compose stack. The always-on *relay* does not come back in either case.

### 9.5 Open-source alternatives (no EMQX Cloud dependency)

All three keep the device firmware and the scale-to-zero relay unchanged; what moves is where
the always-on *broker* lives. Any self-hosted variant terminates TLS with Let's Encrypt, so the
CA the firmware pins in modem NVM is ISRG Root X1 (`PROTOCOL.md` §6.1).

| Option | Licence | What runs where | Monthly cost | Trade-off |
|---|---|---|---|---|
| **(a) EMQX open source on a free-tier `e2-micro`** (`infra/modules/broker-gce`) | Apache-2.0 | EMQX 5 in Docker on the VM; the *same* `relay/emqx/emqx.conf` (rules → HTTP action, REST publish, built-in-DB auth/ACL) as the local compose stack | ≈ $0–4 (VM free; external IPv4 ≈ $4 unless waived) | Erlang runtime wants ~300–500 MB RAM; fits the 1 GB VM for one family. Identical config dev ↔ prod is the big win. |
| **(b) Mosquitto + a 40-line bridge** | EPL/EDL + ours | Mosquitto on the VM; a tiny Python service (paho) subscribes `pager/+/{up,status,loc}` and POSTs each message to `/webhooks/mqtt`; the relay publishes `/down` with a short-lived paho connect→publish→disconnect per send (≈ 100 ms from Cloud Run) | ≈ $0–4 | Lightest footprint (a few MB). Two moving parts instead of one; the bridge needs its own retry/backlog logic, which EMQX's rule engine gives for free. |
| **(c) Everything self-hosted on one VM** | mixed | Broker + relay + SQLite/Postgres on the VM; Firebase Auth/Firestore/FCM replaced by the first draft's sessions, SSE and VAPID code | ≈ $0–4 | No Google dependency at all, but ≈ a third more backend code, OS patching, backups are yours, and the relay is always-on again (free, but a pet). Documented for completeness; not recommended. |

Recommendation: EMQX Cloud Serverless first (zero ops), (a) as the fallback the Terraform
already carries, (b) only if the VM turns out too small for EMQX. `PROTOCOL.md` §12 item 2
(free-tier limits) is answered by (a) with "whatever we configure".

---

## 10. Standing choices, and what is still open

These were the open questions this design had to settle. Most are now settled by what is built; the
three marked **OPEN** need a real account before they can be confirmed.

| # | Question | Where it landed |
|---|---|---|
| D1 | Cloud Run always-on vs. a VM | Scale-to-zero Cloud Run, ≈ $0. The VM reappears only as the broker fallback in §9.5. |
| D2 | Broker: EMQX Cloud Serverless vs. the open-source options in §9.5 | EMQX Cloud Serverless. **OPEN** — the free tier's rule-engine HTTP action, REST publish and ≥15 s webhook timeout are assumed but unverified against a real account (§9.4). §9.5 (a) is the documented fallback if any of the three does not hold. |
| D3 | SMS provider, and US A2P 10DLC / toll-free registration (manual, days, a small fee) | Twilio. **OPEN** — the number rental and 10DLC registration are real-world chores nobody has done; until then `TWILIO_*` stays unset and sms deliveries stay `queued`. |
| D4 | Are the deployment's Google accounts on Workspace? Chat apps are unavailable on consumer Gmail. | **OPEN** — unverified against a real Workspace console. The adapter is built on the documented contract regardless; Email (§6.6) is the fallback slot if the restriction holds. |
| D5 | Sign-in methods: email link, phone, or both | Both enabled; the login page accepts either. Phone needs reCAPTCHA and has a small per-SMS cost past the no-cost allowance. |
| D6 | Custom domain for the web app | None assumed. The `*.web.app` URL works for everything, including the broker/Chat/Twilio webhook URLs. |
| D7 | Retention defaults | 4 weeks messages, 1 week locations, 52-week cap, weekly sweep Sunday 03:00 local. |
| D8 | Device addressing UX: `@alias` typed on the CardKB vs. a recipient picker on the e-paper | The wire supports both (`to` is optional); firmware decides. |
| D9 | Map: link-out or an embedded map | Link-out. A Leaflet + OpenStreetMap tile map is an optional follow-up. |
| D10 | Cold start: accept 2–4 s on the first parent→pager send after idle, or ≈ $10–22/mo for a warm instance | Accept it. §9.3 has the measurement procedure; revisit only with a real number. |

---

## 11. Firmware follow-ups this design creates (not scheduled here)

1. Parse `kind`; treat `loc_req` per §4.2 (no render, no ack, answer on `/loc`).
2. GNSS: `walter-modem` GNSS fix API, assistance data, fix timeout, publish `/loc` per §4.3,
   rate limit per §4.6, `loc_period_s`/`loc_min_s` in `/status`. **Measure GNSS power first** —
   a cold fix on the GM02SP can run for tens of seconds at tens of mA; the periodic interval the
   device "chooses" should be derived from that measurement and the battery budget, which is why
   the interval is device-side and not a server setting.
3. Composer: optional `@alias` prefix → `to` (D8).
4. Broker ACL for `pager/{id}/loc` publish (config, not code) — and the broker moves from HiveMQ
   to EMQX, so the pinned CA in modem NVM (`PROTOCOL.md` §6.1) is whichever CA EMQX Cloud's
   endpoint chains to; confirm before flashing.
5. `PROTOCOL.md` §7.3 data budget line for `/loc`; §8.4 power line for GNSS.
6. Provisioning tool (`PROTOCOL.md` §12 item 5) consumes `POST /api/admin/devices`' one-time credentials.

---

## 12. Why the device keeps MQTT — HTTPS polling analysed

Asked 2026-09-14: *if the pager spoke HTTPS directly to the relay and the broker went away, what
would it cost in battery and mobile data?* Recorded here so the question is not re-opened without
new numbers. All figures are `(estimate)` in `PROTOCOL.md`'s sense and reuse its §6–§8 arithmetic.

### 13.1 The structural problem
HTTPS cannot push. The relay has no route to a device behind carrier NAT, so downlink becomes
**polling**, and every poll costs a full TLS handshake in data (the modem library exposes no TLS
session resumption, §7.3) and an RRC connection *plus its inactivity tail* in energy. MQTT-in-modem
pays neither: the network pages the modem only when there is something to deliver, and the TLS
session is set up once.

### 13.2 Per-poll cost
| Term | Estimate | Basis |
|---|---|---|
| Data: TCP + TLS 1.2 (2-cert chain) + HTTP request/response | ≈ 5 kB | `PROTOCOL.md` §7.2 reconnect line |
| Energy: ≈ 4 s transfer at ≈ 120 mA + ≈ 10 s RRC inactivity tail at ≈ 50 mA | ≈ 0.25 mAh (0.15–0.4) | §6.2 / §8.3(b) arithmetic; the tail is carrier-configured and not modelled in `PROTOCOL.md` |

### 13.3 Poll interval versus the current design
Sleep mode, one device, uplink (acks, status, `/loc`) piggybacked on the poll body so it costs
nothing extra. Battery assumes the polling design can use ESP32 deep sleep and modem PSM between
polls (there is no URC to catch, so §8's light-sleep constraint disappears).

| Downlink | Worst-case latency | Data / month | Battery / day | Idle life, 1500 mAh |
|---|---|---|---|---|
| **MQTT in modem (today)** | **≈ 27 s** | **1.7 MB nominal / 6.9 MB pessimistic** | **43–50 mAh** | **30–35 days** |
| HTTPS poll every 25 s | ≈ 30 s | ≈ 520 MB | modem never leaves RRC-connected: 1–2 Ah | ≈ 1 day |
| HTTPS poll every 60 s | ≈ 65 s | ≈ 216 MB | ≈ 360 mAh | ≈ 4 days |
| HTTPS poll every 5 min | ≈ 5.5 min | ≈ 43 MB | ≈ 75 mAh | ≈ 20 days |
| HTTPS poll every 15 min | ≈ 15.5 min | ≈ 14 MB | ≈ 27 mAh | ≈ 55 days |
| HTTPS poll every 30 min | ≈ 30.5 min | ≈ 7 MB | ≈ 15 mAh | ≈ 100 days |

Caps: SIM 100 MB/month; this project's own bar is < 10 MB. Active mode is worse: polling
every 2 s through a 10-minute window is ≈ 1.5 MB per window, ≈ 270 MB/month for six windows a day.

### 13.4 Reading the table honestly
- At the pager's **30 s target** HTTPS is ≈ 100× the data and drains the cell in a day. Not viable.
- The **15–30 min rows beat MQTT on battery**. That is real: deep sleep (9.5 µA vs 1 mA light
  sleep) and PSM are worth ≈ 30 mAh/day. But the identical saving is available to MQTT as the
  "mailbox mode" `PROTOCOL.md` §11 already reserves, so HTTPS wins only by *also* giving up the
  latency target by 30–60×. That is a different product (a mailbox pager), not a cheaper
  implementation of this one.
- Softeners, all `UNVERIFIED`: modem-internal HTTP keep-alive or TLS resumption (cuts data 3–5×,
  energy barely, because the RRC tail dominates); long polling (bounded by the modem's HTTP
  command timeout and carrier NAT idle; and a held request on Cloud Run bills for its duration,
  i.e. the always-on cost returns in another shape); Release Assistance Indication (could halve
  per-poll energy if the carrier honours it); the relay's 2–4 s cold start lands *inside* every
  poll's connected time.

### 13.5 The only broker-free design that keeps push
Use the SIM plan's messaging as the wake-up: a mobile-terminated **SMS** is paged to the modem
exactly like an MQTT publish; the device then makes one HTTPS pull (or the SMS *is* the message —
the §3.1 body limit of 160 code points fits one SMS). Latency stays ≈ 30 s, data ≈ 3 MB/month.
Costs: an outbound SMS per message from the server side (≈ $5/month at 20 messages/day via
Twilio), whether `walter-modem` exposes SMS receive (unverified), the SIM's SMS budget of
100/month (600 would exceed it), and weaker delivery-state semantics. Kept as a documented
alternative, not planned.

### 13.6 Decision
**The device keeps MQTT.** The broker is the only component that gives push at this power budget,
it costs nothing at this scale (§9.3), and firmware and protocol are already built around it. If
the concern is dependence on a hosted broker, the answer is §9.5 (a) or (b), not HTTPS polling.
