# BUILD_LOG.md — v2 server stack unattended build

Orchestrator: Claude (Sonnet 5), unattended overnight run per `HANDOFF_V2.md`.
Started: 2026-09-14.

Read in full before starting: `HANDOFF_V2.md`, `docs/SERVER_PLAN.md`, `docs/PROTOCOL.md`, `relay/README.md`.

Rules in effect: branch `v2`, never push, no accounts/money/cloud mutations, delegate
implementation to subagents, log every phase here.

---

## Phase 0 — Housekeeping (2026-09-14)

- Created branch `v2` from `main`.
- Verified `.claude/agents/{backend-dev,web-dev,infra-dev,server-architect,docs-writer,log-triage,firmware-architect,firmware-dev}.md` all exist.
- `docs-writer` added a 4-line status note at the top of `HANDOFF.md` §2 pointing at
  `docs/SERVER_PLAN.md` §2 (transport revision) and `HANDOFF_V2.md` (the v2 brief).
- MVP baseline, run against `relay/docker-compose.yml` (MVP's `eclipse-mosquitto` + relay):
  - `relay/.venv/bin/pytest -q` → **52 passed**, 2 warnings (unrelated deprecation notices).
  - `relay/.venv/bin/python tools/e2e_test.py` → **4/4 scenarios PASS**: full round-trip
    (queued→sent→shown→read + student reply), malformed payloads over the real broker, broker
    outage recovery, power-on with no network.
  - Compose stack torn down (`docker compose down -v`) after the run.
- No accounts created, nothing pushed, nothing deployed.

**Done when:** commit `v2 phase 0`. ✅

---

## Phase 1 — Protocol v2 (2026-09-14)

- `server-architect` edited `docs/PROTOCOL.md` per `SERVER_PLAN.md` §4.1–§4.8: `from` is now an
  alias regex (`^[a-z0-9][a-z0-9_-]{0,15}$` or `system`); down envelopes gain optional `kind`
  (`msg` default | `loc_req`); up messages gain optional `to`; new §13 "Location" spends the
  `pager/{id}/loc` topic and `loc` schema reservations from §11 (full payload schema, QoS rules,
  §13.3 device-side rate limit made normative); `/status` gains `loc_period_s`/`loc_min_s`
  (display-only); §7 data budget extended for `/loc`; §12 gained a GNSS-power-measurement item;
  one relay-transport paragraph added to §2/§4/§5.3/§6.1 (rule-engine HTTPS push + REST publish
  replaces the persistent `relay-1` MQTT session; `sent` now means "publish API accepted it").
  Every edit carries a `(v2 decision — reason)` note.
- `backend-dev` implemented the matching `relay/app/wire.py` changes: alias validation replacing
  the old `FROM_VALUES` enum, `DownEnvelope` (with `kind`), `LocEnvelope`/`LocFix`, `to` on
  `UpEnvelope`; extended `tools/sim_device.py` to drop (not ack, not render) a `loc_req` down
  message per §3.2.
- `server-architect` reviewed the combined diff and found three real issues, all fixed by
  `backend-dev` before commit:
  - **S1 (high):** `build_down_payload` could silently emit `"body":""` for a `kind:"msg"`
    envelope, which `DownEnvelope`'s own validator calls malformed — the device would drop it
    without acking, and it would sit at `sent` until the 24h expiry sweep. Now raises `ValueError`
    instead. (Verified the one production caller, `mqtt_gateway.py`, never actually hits this path
    since the HTTP API already rejects empty bodies upstream — but the widened `wire.py` API made
    the hole real for any future caller.)
  - **S2 (medium):** `LocFix` (the `loc` object) had no range validation on `lat`/`lon`/`acc`/
    `fix_ts`. Added Pydantic bounds (`lat` ±90, `lon` ±180, `acc` ≥ 0, `fix_ts` ≠ 0 when `loc` is
    non-null).
  - **S3 (medium):** `UpEnvelope` wrongly allowed `to` on an ack payload; §3.1 restricts `to` to
    up content messages only. Now rejected.
  - `server-architect` also made two small doc-only fixes directly: corrected §3.3's "realistic
    v2 worst case is 604 bytes" claim (the true jointly-achievable ceiling is 438 B for an up
    message with `to`; 604 sums two fields' independent maxima, which the body's own dual
    code-point/byte caps make unreachable together — the 640B hard limit and the per-field
    ceiling table are unaffected, only the prose claim was wrong), and added a `(v2 decision — …)`
    clause closing a real gap: `from:"system"` was inbound-shape-valid but PROTOCOL.md's stated
    rationale ("prevents forged relay notices") only forbade the *relay* from emitting the alias,
    not a device claiming it inbound. Now the relay MUST derive attribution from the device→user
    mapping and log a mismatch as a security event — additive, no code change required in Phase 1.
- **`(build finding — SERVER_PLAN.md)`**: `tools/sim_device.py` correctly drops a `loc_req`
  without acking (matching old-firmware behaviour per §3.2), but has no code path that *answers*
  one on `/loc` at all yet. That means no test double can currently drive §13.4's `fulfilled`
  transition — only `expired` is reachable end-to-end. This is expected to land in Phase 4
  (Location) via `pager_client.py`'s `loc`/`loc auto`/`loc fail` commands, not a gap in Phase 1.
- Tests: `relay/tests/test_wire.py` — **102 passed**. `tools/e2e_test.py` — **4/4 MVP scenarios
  PASS**, unchanged.

**Done when:** commit `v2 phase 1`. ✅

---

## Phase 2a — Serverless transport (2026-09-14)

- `backend-dev` replaced the MVP's always-on paho-mqtt relay client with a request-driven design:
  `relay/app/broker.py` (`BrokerClient` — REST publish over EMQX's `/api/v5/publish`, webhook
  key verification, webhook body parsing), `relay/app/ingest.py` (ported from the deleted
  `mqtt_gateway.py`: `handle_up`, `handle_status` incl. online-edge re-publish, `handle_loc` stub
  for Phase 4), `relay/app/routers/webhooks.py` (`POST /webhooks/mqtt`). No process holds an MQTT
  connection anymore. `relay/docker-compose.yml` now runs `emqx/emqx:5.8.0` instead of
  `eclipse-mosquitto`, configured idempotently by `tools/emqx_setup.py` against EMQX's REST
  management API (connector → HTTP action → rule on `pager/+/{up,status,loc}`; `query_mode: sync`
  is required — an initial `async` probe left requests permanently "inflight"). Legacy
  `RELAY_TOKEN` endpoints keep working over SQLite, now publishing via `BrokerClient`.
- `server-architect` reviewed the diff and fixed two high-severity issues directly:
  - **H1:** synchronous `httpx` broker calls (up to 10 × 5s in the online-edge republish path)
    were running inline on the asyncio event loop inside `POST /webhooks/mqtt`, so one slow
    `/status` webhook could stall every other request the process serves for up to ~50s. Fixed by
    wrapping the handler dispatch in `run_in_threadpool`.
  - **H2:** the opportunistic `retry_queued()` retry (see below) could cost up to 10×5s on a
    plain `GET` when the broker was unreachable — exactly the worst case. Fixed to stop at the
    first failed publish.
- `backend-dev` fixed two medium findings from the same review:
  - **M3:** `webhooks.py`'s blanket `except Exception → 200` could swallow a genuine
    `sqlite3.OperationalError` on an `/up` insert and silently lose a student reply (up messages
    have no republish rule per §4.2). Now `OperationalError` re-raises → 500 → broker retries;
    everything else still gets 200 per §3.4's malformed-payload rule.
  - **M4:** wire-id dedup was check-then-insert (`id_exists()` then `insert_up_message()`), not
    atomic — two concurrent at-least-once webhook deliveries of the same id could both pass the
    check. Now `INSERT ... ON CONFLICT(id) DO NOTHING`, returning whether the insert happened.
  - **M5 (device-level allow-list enforcement on the ingest path)** is explicitly deferred to
    Phase 2b's registry gate, where it belongs — not a regression, the MVP had no registry either.
  - Minor follow-ups noted, not blocking: broker ACLs unimplemented in the dev EMQX compose
    service (dev-only, PROTOCOL.md §2's ACL bullet is normative for a real deployment); unset
    `WEBHOOK_KEY` fails closed silently (worth a startup warning); webhook body read is unbounded
    before the 640B check (low risk behind a proxy).
- **`Ingest.retry_queued()`** (called from the legacy status-read path) is a narrow, read-time-
  derived stand-in for one specific gap: a *transient* publish failure (timeout/5xx/DNS blip)
  while a device stays continuously connected, so no `/status` event ever fires to trigger the
  existing online-edge republish. (A full broker outage is already covered: the device reconnects
  with a fresh session, which republishes via the existing `session_changed` path.) **To be
  deleted once `/internal/tick` (`SERVER_PLAN.md` §5.8) lands** — marked with a
  `TODO(orchestrator)` at its definition.
- Tests: `relay/tests` — **115 passed**. `tools/e2e_test.py` — **4/4 scenarios PASS through the
  real EMQX rule engine**, run twice for stability. `docker compose down -v` after each run.
- **`PENDING_ACCOUNT: EMQX Cloud Serverless`** — a human needs to open the free account and verify
  three assumptions before Phase 9 deploys for real (`SERVER_PLAN.md` D2): (1) the rule engine
  supports an HTTP action on the Serverless tier, (2) the REST publish API is available on the
  Serverless tier, (3) the webhook action's timeout can be set ≥ 15s (a cold Cloud Run relay can
  take 2-4s to respond; the local dev config in `relay/emqx/` uses a 10s `request_ttl`, which will
  need raising for production). Steps: sign up at emqx.com/cloud (free tier, no card required
  last checked), create a Serverless deployment, replicate the rule/connector/action shape
  `tools/emqx_setup.py` creates locally, confirm all three points above, and record the findings
  back into `SERVER_PLAN.md` §9.4 as a `(build finding — …)` note. Not done here per HANDOFF_V2.md
  §2 rule 2 (no accounts, no signups).

**Done when:** commit `v2 phase 2a`. ✅

---

## Phase 2b — Firestore + identity (2026-09-14/15)

- `backend-dev` replaced the MVP's SQLite store with Cloud Firestore + Firebase Auth, running
  against local emulators: `relay/emulator.Dockerfile` (`node:20-bookworm-slim` + Eclipse Temurin
  21 — bookworm's own JDK tops out at 17 and current `firebase-tools` needs a JRE ≥ 21, a
  necessary empirical correction to the plan's suggested `default-jre-headless`),
  `relay/app/db/firestore.py` (emulator-aware `firebase-admin` init, `run_transaction` helper),
  `relay/app/store/{users,devices,backends,allow,messages,locations,settings}.py` implementing
  `SERVER_PLAN.md` §3's collections field-for-field, `relay/firestore.rules` +
  `firestore.indexes.json`, `relay/app/auth.py` (`verify_id_token`, the registry gate, admin
  custom claim), `relay/app/bootstrap.py`, `routers/admin.py` (users, allow-list replace-all incl.
  `locatableBy` rewrite, devices with a one-time MQTT password), `routers/dev.py`
  (`POST /api/dev/token`, `DEV_MODE` only), `app/db/import_sqlite.py`. `relay/app/store.py`
  (SQLite) and its migration are deleted.
- **`relay/app/store/legacy.py`** adds three collections outside `SERVER_PLAN.md` §3
  (`legacyMessages`, `legacyStatus`, `legacySettings`) to keep the MVP `RELAY_TOKEN` endpoints
  working without synthesizing fake users/allow-edges into the real §3 collections.
  `server-architect` reviewed this and **accepted it as a clean, isolated, temporary measure** —
  separate collections, separate counter, default-denied by `firestore.rules`, no crossover with
  the real schema. **Phase 6 must drop all three `legacy*` collections along with the endpoints
  themselves** — added to that phase's checklist here so it isn't orphaned data.
- `server-architect` reviewed the transaction code and rules file (required before commit per
  `HANDOFF_V2.md`) and found/fixed three real bugs directly:
  - **S1 (high):** `run_transaction`'s outer retry caught only a commit-phase `ValueError`
    wrapper, but a hot-document contention failure (`Aborted`) can also surface from a
    **read**-phase call inside the transaction, which propagates uncaught — the retry as written
    did not cover the failure mode it was built for. Fixed with an explicit `Aborted` handler and
    a cause-based check instead of string matching.
  - **S3 (medium):** `legacy.py`'s `mark_sent` was a non-transactional read-then-write — an ack
    landing between the two could walk delivery state backwards (PROTOCOL.md §4.1 rule 2). Now
    wrapped in `run_transaction`.
  - **S4 (low):** `settings/meta` initialization could race `create_message`'s own bootstrap of
    the same document, clobbering `seqCounter` back to 0 and handing out a duplicate `seq`. Now
    uses `create()` + catches `AlreadyExists` instead of exists-check-then-`set()`.
  - Two follow-ups closed by `backend-dev` before commit: **S2**, the retry backoff was tuned
    from linear/~0.75s total to exponential/full-jitter/~6.3s total — validated at **0 failures
    across 13 repeated 12-thread concurrent `create_message` bursts** against the real emulator
    (versus frequent failures before); **S5**, added direct invariant tests
    (`test_messages.py`, `test_users.py`) for `create_message`'s `wireId` dedup + `seq`
    monotonicity and `create_user`'s alias uniqueness + rollback-on-conflict, which had no direct
    test file before this phase existed to establish those exact invariants.
  - Three low-severity hardening notes recorded for later, not blocking: the admin API accepts
    either the Firestore `role` field or the Firebase custom claim as proof of admin (rules only
    honor the claim — should require the claim only); `devices/{d}` read exposes
    `mqttUsername`/`mqttPasswordHash` to everyone in `locatableBy`, not just the owner (matches
    the plan's rules sketch as written, but move credentials to a sibling doc before the web app
    reads devices directly); test-emulator wipe responses aren't checked for failure.
- `firestore.rules` matched the `SERVER_PLAN.md` §3 sketch line-by-line, with a few defensive
  `request.auth != null` additions that tighten rather than loosen it. No collection found
  over-permissive.
- **`(build finding)`**: the local Firestore emulator's own per-transaction cost under 12-way
  contention on a single hot document (`settings/meta`) has an empirical worst case of **~20-31s**
  wall-clock for the last straggler, independent of the retry-backoff tuning (it's the emulator's
  serialization cost, not our sleep calls). Not a problem yet — `create_message` isn't wired into
  the live webhook path until Phase 3 — and not expected to matter at the household scale
  `SERVER_PLAN.md` §9.3 describes (low thousands of ops per *day*, not a 12-way concurrent burst),
  but worth remembering if Phase 3's `routing.py` puts `create_message` on the synchronous webhook
  path and a future load test contradicts that assumption.
- Auth-emulator test cleanup uses `DELETE /emulator/v1/projects/{project}/accounts` (verified
  empirically; the plan's suggested `/emulation/v1/...` path doesn't exist).
- Security-rules tests (`tests/test_rules.py`) go through the **Firestore emulator's REST API
  with real Firebase ID tokens** (minted via custom-token + the Auth emulator's Identity Toolkit
  exchange), the same path a browser client uses — proving `firestore.rules` enforces on its own,
  independent of relay-side logic.
- Tests: `relay/tests` — **164 passed**. `tools/e2e_test.py` — **4/4 scenarios PASS** against the
  full Firestore-backed stack.
- See Phase 2a's `PENDING_ACCOUNT: EMQX Cloud Serverless` entry above — unchanged, still open.

**Done when:** commit `v2 phase 2b`. ✅

---

## Phase 3 — Routing, core backends, first e2e (2026-09-15)

- `backend-dev` built the routing engine and the first two delivery backends:
  `relay/app/routing.py` (`send()` per `SERVER_PLAN.md` §5.2 — recipient resolution including
  device default and broadcast, per-recipient allow-list check with drop+log+system-reply on
  denial, one Firestore transaction per recipient reusing Phase 2b's `create_message`, then
  inline in-request delivery), the `Backend` protocol and `pager.py`/`webapp.py` adapters
  (`relay/app/backends/`), `tasks.py` (inline mode only), `jobs.py` (`tick()`, now with a
  5-attempt delivery failure cutoff — see below), `routers/{conversations,me,internal}.py`, and
  the first cut of `tools/pager_client.py` (device side over real MQTT, server side over the
  Firestore-backed API) and `tools/e2e_v2.py` (scenarios 1-4: `bootstrap`, `text_roundtrip`,
  `allowlist`, `republish`).
- `server-architect`'s review found and fixed two high-severity bugs directly:
  - **A revoked device's up-messages silently fell through to legacy SQLite-era storage** instead
    of being dropped — revocation was cosmetic, not enforced, on the ingest path.
  - **The `system` "unknown recipient" reply minted a fresh id on every call**, so a QoS-1
    redelivery of the same offending up-message produced a second down message with a different
    id — exactly the redelivery-storm failure mode PROTOCOL.md §4.1 rule 7's id-keyed dedup ring
    exists to prevent. Now the reply id is deterministic (derived from the offending message),
    so a redelivery reuses the same id and the device's own dedup suppresses the re-render.
- `backend-dev` closed two schema/behavior gaps flagged by the same review, before commit:
  - **`originBackendId` was storing a backend *kind*** (`"pager"`/`"webapp"`), not a real
    per-user backend document id — unrecoverable once Phase 7 gives a user two backends of the
    same kind (two phones) and needs to know which one a reply arrived through. Split into
    `originBackendKind` (always set) and `originBackendId` (real id; `null` until Phase 7 has one
    to pass). Fan-out exclusion now prefers an id match, falling back to the kind-based self-loop
    guard that pager/webapp still use (unchanged behavior for both today). A `(build finding)`
    note was added to `SERVER_PLAN.md` §3 recording this as the definitive schema.
  - **Delivery `attempts`/`error` were never written**, and a backend's `DeliverResult` was
    discarded — so a permanently-failing pager delivery (e.g. dead broker credentials) retried
    forever with no failure state, on every `tick()` and every online-edge event, for 24h.
    Added `record_delivery_attempt()`: 5 failed attempts → `'failed'`, transactionally, and the
    device is removed from `pendingDeviceIds` so it stops being retried.
  - Two smaller fixes: `conversations/{convKey}.unread` only ever incremented — `mark_read` now
    clears it for the reading user in the same transaction as the read ack. `mark_read`'s
    `{alias}` URL path segment was accepted but never actually checked against the message's
    `convKey` (authorization was correct via `recipientUid` alone, but the alias was decorative) —
    now validated, 404 on mismatch.
- **`retry_queued` (legacy collection) vs. `jobs.tick()` (v2 collection)** — confirmed by the
  review to operate on genuinely disjoint storage (`legacyMessages` vs. `messages`/
  `pendingDeviceIds`), so keeping both is correct, not redundant. Both are retired together in
  Phase 6 when the legacy endpoints go.
- **`(deploy blocker, tracked for Phase 9)`**: EMQX still provisions no device-level MQTT
  credentials or ACLs locally — only the rule-engine forwarding and the relay's own webhook key.
  `POST /api/admin/devices` mints and returns a credential, but nothing pushes it (or PROTOCOL.md
  §2's three ACL rules) into the broker, so a minted device credential currently authenticates
  nothing against the local EMQX. Not a regression — MQTT auth wasn't enforced in the MVP either
  — but must be closed before a real deployment; add to Phase 9's runbook checklist alongside the
  EMQX Cloud Serverless account verification.
- Tests: `relay/tests` — **207 passed**. `tools/e2e_v2.py` — **scenarios 1-4 ALL PASSED**
  (`bootstrap`, `text_roundtrip`, `allowlist`, `republish`, including the "broker API down →
  queued → tick delivers" leg) against the full compose stack.

**Done when:** commit `v2 phase 3`. ✅

---

## Phase 4 — Location (2026-09-15)

- `backend-dev` built device location tracking: `relay/app/location.py` (`/loc` ingest — dedup on
  a new `locWireIds` collection, periodic fixes written to `devices/{d}/locations`, on-demand
  answers fulfil the matching `loc_req` delivery and post a `kind='loc'` thread message to every
  coalesced requester; `Location.locate()` — a three-way decision: coalesce onto an in-flight
  request < 15 min old, answer from a cached fix < 60s old, or claim a fresh request; derived
  15-minute expiry), a new `POST /api/conversations/{alias}/locate` endpoint (requires
  `allow.locate`), `jobs.tick()` cleanup of stale `locReqs` docs, `tools/pager_client.py`
  additions (`loc`/`loc auto`/`loc min`/`loc fail` on the device side implementing PROTOCOL.md
  §13.3's rate limit exactly; `locate`/`locations` on the server side), and `tools/e2e_v2.py`
  scenarios 5-6 (`location_periodic`, `location_on_demand`), passing alongside 1-4.
- **Device-targeted delivery decision**: the `loc_req` down message is built directly via
  `create_message` with a single `pager`-kind delivery, not through `Routing.send()` (whose
  per-user fan-out would incorrectly also queue it to the device owner's webapp/sms backends) —
  but the publish step still goes through `Routing.redeliver_pager`, so it gets identical
  `attempts`/`error`/`failed`/`pendingDeviceIds` bookkeeping to every other pager delivery.
- **Three bugs found and fixed while building this phase** (new gaps this phase's traffic was
  the first to exercise, not regressions):
  - `firestore.rules` threw during Firestore's abstract rule pre-check for `list` (collection
    query) requests specifically — every prior test only exercised single-document `get`s. Fixed
    with null-safe field accessors; confirmed non-loosening.
  - A device lookup needed to filter on `locatableBy array-contains <caller>` instead of
    `ownerUid`, because Firestore's rules engine can only prove a *list* query safe against the
    former shape — a genuine security-rules constraint, not a workaround.
  - `locatableBy` was only recomputed for devices that already existed at the moment an allow
    edge changed, so setting an edge **before** creating a device (or importing from the MVP's
    SQLite file) silently and permanently lost the grant. Fixed at both call sites: the admin
    device-creation API and `import_sqlite.py`.
- `server-architect`'s review found one high-severity and two medium bugs, all fixed by
  `backend-dev` before commit:
  - **S1 (high):** `/loc` dedup and `loc_req` fulfilment were two separate transactions. A crash
    between them let a QoS-1 redelivery see the dedup marker already present and silently
    swallow the retry — the fix stayed stored but the requester was never notified, permanently.
    Folded into one transaction; a forced-failure test confirms a redelivered `/loc` now reaches
    `'fulfilled'` on retry instead of being silently dropped.
  - **S2 (medium):** the `locReqs` "claim a fresh slot" path used `set()`, making "at most one
    in-flight `loc_req` per device" (PROTOCOL.md §13.3 rule 5, normative) an accident of
    Firestore's read-lock timing rather than an explicit precondition. Now `transaction.create()`
    + retry-on-conflict, matching every other uniqueness invariant in this codebase.
  - **S3 (medium):** a permanently-failed `loc_req` left `locReqs` in place forever, so every
    later `/locate` call coalesced onto a request that could never be answered. Now cleared
    immediately when the pager delivery reaches `'failed'`.
- New `locWireIds` collection (a `/loc` payload has no recipient to key `wireIds` against, so it
  needs its own dedup marker) documented in `SERVER_PLAN.md` §3 — **Phase 8's retention sweep
  must delete it alongside `locations`**, added to that phase's checklist.
- Tests: `relay/tests` — **237 passed**. `tools/e2e_v2.py` — **scenarios 1-6 ALL PASSED**.

**Done when:** commit `v2 phase 4`. ✅

---

## Phase 5 — Client complete, CI switch (2026-09-15)

**This closes out the mandatory, sequential Phases 0-5.** Phases 6, 7 and 9 can now run in
parallel per `HANDOFF_V2.md` §5.

- `backend-dev` built `tools/mocks/twilio_mock.py` (records outbound SMS, can be told to fail),
  `relay/app/backends/sms_stub.py` (a deliberately marked Phase 5 stand-in for Phase 7's real
  Twilio adapter — no credentials, no signature verification, `TODO(orchestrator)`-marked in
  three places so it can't be mistaken for the real thing), a real `jobs.sweep()` implementing
  `SERVER_PLAN.md` §5.7 (retention `{n, unit}` settings converted to seconds only in the sweeper;
  batched deletes across `messages`+`wireIds`, a `locations` collection-group sweep,
  `locWireIds`, `locReqs`, and — after review — `wireIds`' own independent pass and
  `conversations`), the remaining `pager_client.py` commands, `e2e_v2.py` scenarios 7-9
  (`fanout`, `retention`, `bytes` — **all 9 scenarios now pass**), a CI switch from
  `e2e_test.py` to `e2e_v2.py`, and `tools/send.py` re-pointed at the new API.
- **`(build finding)`**: while switching CI, `backend-dev` found and fixed a pre-existing bug —
  `.github/workflows/ci.yml` never actually started the Firestore/Auth emulators before running
  `pytest`, which has required them since Phase 2b. Fixed with a bounded poll on both ports
  (8080 and 9099 — Firestore's answers before Auth's) followed by a hard failure if either
  doesn't come up, not a bare `sleep`.
- `server-architect`'s review found and fixed one high-severity bug directly: the sweep's
  write-batch loop could split a message from its `wireIds` companion across two separate batch
  commits, so an interruption between them — the exact failure mode retention's resumability
  requirement (§5.7) exists to survive — permanently orphaned the `wireIds` doc. Fixed by
  reserving both writes as one unit before either is staged.
- Three medium follow-ups closed by `backend-dev` before commit:
  - **`wireIds` had no independent reclamation path** and would grow unbounded once Phase 8's
    user-deletion orphans them from their message (no message left to delete them alongside).
    Added a `createdAt` field and an independent sweep pass.
  - **`conversations/{convKey}` summaries were never swept** — `lastPreview` (a truncated
    message body) and `unread` counts survived their thread's retention period entirely. Added a
    sweep pass on `lastMessageAt`.
  - **`jobs.tick()` could block for minutes** if the broker and Twilio-mock were both down
    simultaneously (up to 50 non-pager retries running inline with no dispatch cap, each with a
    5s timeout). Capped actual dispatches at 10 per tick, mirroring the existing pager-retry cap.
- **`tick()` extension beyond §5.8's literal text**: it now also retries non-pager (`sms`)
  queued deliveries, not just pager ones — justified because sms deliveries carry no device id
  (so `pendingDeviceIds`-based retry doesn't cover them) and the `fanout` scenario's
  "retry → `failed` after max attempts" requirement had no other mechanism to attach to. Reuses
  the existing attempts-capped failure machinery via a new generic `Routing.redeliver()`, and is
  now bounded by the same 10-per-tick cap as pager retries (see above). Documented at length in
  `jobs.py`'s module docstring — a deliberately small, scoped addition, not a rewrite of the
  retry architecture (real Cloud Tasks enqueue-at-send-time stays Phase 7/8).
- **`(build finding)`**: review noted `docs/SERVER_PLAN.md` §5.9's index list doesn't yet
  mention the two new indexes (`wireIds(createdAt)`, `conversations(lastMessageAt)`) added for
  the sweep passes above — the doc predates those passes, not a contradiction, but worth a
  follow-up pass to keep it the source of truth.
- Tests: `relay/tests` — **250 passed**. `tools/e2e_v2.py` — **all 9 scenarios ALL PASSED**
  (`bootstrap`, `text_roundtrip`, `allowlist`, `republish`, `location_periodic`,
  `location_on_demand`, `fanout`, `retention`, `bytes`).

**Done when:** commit `v2 phase 5`. ✅

---

**Phases 6 and 9 ran in parallel from here**, per `HANDOFF_V2.md` §5's explicit permission —
`web/` and `infra/` are disjoint directories with no shared trust boundary.

## Phase 9 — Terraform + runbook (2026-09-15)

- `infra-dev` built `infra/` per `SERVER_PLAN.md` §9.1-9.2: `bootstrap/` (one-off project APIs +
  GCS state bucket), `modules/{firebase,relay-service,schedule,secrets,ci-deploy,broker-gce}`,
  `envs/prod` wiring them together. Cloud Run matches §9.2 exactly (min 0 max 2,
  `startup_cpu_boost`, concurrency 20, timeout 300s) with `min_instance_count=0` enforced by a
  Terraform **validation block**, not just documentation. Scheduler jobs (tick/5min, sweep
  weekly) and the Cloud Tasks queue match the plan's retry description. Secret Manager creates
  empty containers only. `broker-gce` is the optional EMQX-on-GCE fallback (§9.5a), off by
  default, mirroring `tools/emqx_setup.py`'s exact rule/connector/action shape.
- **`.github/workflows/deploy.yml`** is gated so it cannot reach a real cloud account today:
  push-to-main/`workflow_dispatch` only (no `pull_request`, so a fork can't trigger it), every
  cloud-touching job additionally requires two deploy secrets that don't exist in this repo yet,
  and even a fully-secreted `terraform init` would still fail on a placeholder state-bucket name
  until a human runs `bootstrap/` by hand.
- `infra/README.md` is the human runbook: project creation, Blaze billing, EMQX Cloud console
  steps (cross-referencing Phase 2a's `PENDING_ACCOUNT` note and Phase 3's device-credential/ACL
  gap), first admin, and the Phase 9 cold-start measurement procedure.
- `server-architect`'s review **verified the safety claims empirically** rather than trusting the
  description (parsed the workflow YAML directly, confirmed via `gh api` that no deploy secrets
  exist in this repo, ran `terraform fmt -check`/`validate -backend=false` locally) and fixed one
  real issue directly: the gate job interpolated `${{ secrets.* }}` straight into a shell script —
  a known GitHub Actions injection pattern (a secret value could alter the script, not just its
  data) — moved to an `env:` block.
- Follow-ups recorded, none blocking (nothing here can reach a cloud account without a human
  first running `ci-deploy` and `bootstrap` by hand): the WIF trust policy scopes to the repo but
  not specifically to the `main` branch (matters only once the deploy SA exists for real); the
  Hosting deploy step had no `firebase.json` to find at all (closed in Phase 6, see below);
  `npx firebase-tools@latest` was unpinned (also closed in Phase 6).
- No `GCHAT_*` secret was created — Google Chat's outbound call uses the relay's own service
  account (ADC) and inbound verifies a Google-signed JWT, so there is no shared secret to store;
  documented in place as a deliberate deviation from §9.2's listing.
- `terraform fmt -check` and `terraform validate -backend=false`: clean in every module,
  `infra/bootstrap/`, and `infra/envs/prod/`. **No `terraform apply` or `plan` run against a real
  project; no cloud account created.**

**Done when:** commit `v2 phase 9`. ✅

---

## Phase 6 — Web app (2026-09-15)

- `web-dev` built `web/` per `SERVER_PLAN.md` §7: Next.js (App Router, static export), MUI,
  Firebase JS SDK against the emulators. All routes from §7.2: `/login` (email-link + phone
  code), `/chat`, `/chat/[alias]` (delivery-state chips per backend, a location card with
  open-in-maps, "Request location"), `/settings/{backends,notifications}`,
  `/admin/{users,allowlist,devices,settings}`. `firebase-messaging-sw.js` for background push;
  foreground notifications gated on tab visibility. Admin routes are hidden client-side based on
  `/api/me`'s server-verified role — not a security boundary in themselves; `require_admin` and
  `firestore.rules` are, and are unchanged.
- **Deleted `relay/static/index.html`, its mount, and the legacy `RELAY_TOKEN` endpoints**
  (`routers/legacy.py`, `store/legacy.py`, their three Firestore collections, their tests) per
  `HANDOFF_V2.md`'s sequencing for this phase — nothing since Phase 3 depended on them. This
  required a genuine rewrite of `ingest.py`, which had run the legacy and v2 device-traffic
  models side by side since Phase 2a; only the uid-addressed v2 model remains, with traffic from
  an unregistered device now logged and dropped rather than falling through to a second store.
- **`(cross-phase fix)`**: found that Phase 9's `deploy.yml` had no `firebase.json` to find at
  all — Hosting config lived in `web/firebase.json`, Firestore config in `relay/firebase.json`,
  and the deploy step ran from the repo root with neither. Added a root-level `firebase.json` +
  `.firebaserc` that references both by relative path (`web/out` for hosting,
  `relay/firestore.rules`/`indexes.json` for Firestore), without touching either local-dev
  config (`relay/firebase.json` is baked into the emulator image; `web/firebase.json` is a local
  Hosting preview — neither needs to know about the other). Also pinned `firebase-tools` to a
  major version in the deploy step instead of floating `@latest`, closing a Phase 9 follow-up.
- Two gaps found and **documented rather than silently worked around**:
  - `firestore.rules` gives a non-admin member no server-side way to resolve a conversation
    partner's alias, needed for the contact list and to open a thread at all. Worked around
    entirely client-side (admins get the full directory; members bootstrap per-contact on first
    send, cached locally) — flagged as needing either a rules change or a contacts endpoint.
  - `POST /api/me/backends/{id}/verify` and device revoke have no mounted relay route yet (the
    store function exists, unmounted since Phase 2b/3). The UI surfaces the resulting 404 rather
    than pretending success.
  - Both noted in `web/README.md` for a human to pick up.
- No embedded map (§7.7 explicitly defers this to a follow-up) — link-out only, as specified.
- Verified: `npm run build` / `tsc --noEmit` / `next lint` clean. `relay/tests` — **209 passed**
  after the legacy deletion (fewer than Phase 5's 250 — the deleted legacy tests accounted for
  the difference, not a regression). `tools/e2e_v2.py` — **all 9 scenarios PASSED** after the
  deletion.

**Done when:** commit `v2 phase 6`. ✅

---
