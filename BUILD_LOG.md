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

## Phase 7 — SMS + Google Chat adapters (2026-09-14)

- `sms-gchat-dev` built the real `sms` (`app/backends/sms_twilio.py`) and `gchat`
  (`app/backends/gchat.py`) adapters per `SERVER_PLAN.md` §6.4/§6.5, replacing Phase 5's
  `sms_stub.py` (deleted; `registry.py` now points `sms`/`gchat` at the real classes).
  `app/notify/sms.py` is the one place that builds a Twilio Messages-API request (outbound sends
  and `start_link`'s verification-code SMS both go through it), matching §5's package layout.
- **Twilio**: `X-Twilio-Signature` verified for real (HMAC-SHA1 over the exact webhook URL +
  sorted POST params, Twilio's documented algorithm) — `relay/tests/test_sms_twilio.py`'s
  `test_compute_twilio_signature_matches_known_fixture` checks the implementation against a
  signature computed *independently* (a standalone script, not a call into the code under test)
  and hardcoded as a fixture, not a self-referential mock. `POST /webhooks/twilio/sms` maps
  `From` → user via a new `phoneIndex/{phone}` lookup (written on verify, not on backend
  creation, so an unverified phone claim can't steal someone else's inbound texts), resolves the
  recipient via the shared `@alias`/single-peer rule (`app/backends/resolve.py`, used by both
  adapters), and rejects (never truncates) a body over 160 code points with an SMS usage hint,
  checked before `routing.send()` per the brief.
- **Google Chat**: outbound `spaces.messages.create` uses the relay's own ADC service-account
  credentials (no shared secret — confirmed matches Phase 9's infra decision, which already
  created no `GCHAT_*` secret for the same reason); a `ChatClient` Protocol
  (`NullChatClient`/`GoogleChatClient`) mirrors `webapp.py`'s `FCMClient` pattern so this is
  testable without real GCP credentials. Inbound `POST /webhooks/gchat` verifies a Google-issued
  JWT via `google-auth`'s own `google.auth.jwt`/`google.oauth2.id_token` primitives —
  `relay/tests/test_gchat.py` signs a **real** RS256 JWT with a locally generated RSA keypair
  (`cryptography`) and verifies it through the real verification code path (only the cert
  *source* is faked — an injected `{key_id: pem}` map instead of a network fetch of Google's
  published certs), covering valid-accepted and tampered-signature/wrong-audience/wrong-issuer/
  expired-all-rejected. The link flow is inbound-message-driven per §6.5 (`start_link()` shows the
  user a code on the web app; the user sends `/link <code>` in a Chat DM; the webhook matches it
  via a new `gchatLinkCodes/{code}` lookup and stores the DM `space` name), not the SMS-shaped
  outbound-code flow — documented as a deliberate difference from the `Backend` protocol's literal
  "e.g. send a code" phrasing.
- **`(build addition)`** three small top-level lookup collections not in §3's schema table —
  `phoneIndex/{phone}`, `gchatLinkCodes/{code}`, `gchatSpaces/{spaceId}` — added so both inbound
  webhooks can map "who just texted/messaged us" back to `(uid, bid)` with a single `get()`, the
  same `aliases/{alias}`-style trick §3 already uses for a different lookup. Flagged as additive,
  not a schema contradiction: §3 specifies the *behaviour* (map `From` → verified phone; store the
  DM `space`) but not the index mechanism.
- **`(build addition)`** `POST /api/me/backends/{id}/verify` — listed in §5.1 since Phase 3 but
  never implemented (Phase 6's web app already called it and documented the 404, per that phase's
  entry above). Now wired: `POST /api/me/backends` calls the new backend's `start_link()`
  automatically; `.../verify` calls `complete_link()` and, on success, publishes the verified
  phone to `phoneIndex`.
- **`app/tasks.py`**: added `CloudTasksQueue` (`TASKS_MODE=cloud_tasks`) alongside the unchanged
  `TASKS_MODE=inline` default. **Flagged, not silently worked around**: every existing
  `enqueue(fn, name=...)` call site (`app/jobs.py`, `app/routing.py`) passes an in-process Python
  closure, which a real out-of-process Cloud Tasks queue cannot serialize or invoke — only POST a
  JSON body to a URL. `CloudTasksQueue.enqueue()` therefore builds and creates a real Cloud Tasks
  HTTP task targeting `/internal/task` carrying only the caller's opaque `name` string, per this
  phase's narrower brief ("construct the client, confirm the payload/target URL, without
  enqueueing anything real" — `relay/tests/test_tasks.py` mocks `create_task` at the boundary).
  Two things this leaves genuinely unfinished: (1) `POST /internal/task` itself does not exist yet
  (`app/routers/internal.py` still only has `/tick`/`/sweep`); (2) even once it does, `name` alone
  can't reconstruct "retry this specific delivery" — making `cloud_tasks` mode fully load-bearing
  needs `app/jobs.py`/`app/routing.py`'s retry call sites reworked to enqueue a structured,
  replayable payload instead of a closure. Left as a documented follow-up
  (`app/tasks.py`'s own docstring), not attempted here.
- `tools/e2e_v2.py`'s `fanout` scenario needed **no code changes** — it already creates its sms
  backend through `POST /api/me/backends` (now real) and only asserts outbound-delivery/retry
  behaviour, which is unchanged in shape between the stub and the real adapter. Re-ran against
  the real adapter and the Twilio mock: **PASSED** (bootstrap + fanout, both scenarios).
- Verified: `cd relay && .venv/bin/pytest -q` — **242 passed** (up from Phase 6's 209; +33 new:
  8 sms-webhook, 16 gchat, 9 tasks — see test file list below). `ruff check app tests` clean.
  `tools/e2e_v2.py bootstrap fanout` — **PASSED** against the real compose stack.
- New/changed files: `app/notify/{__init__,sms}.py`, `app/backends/{sms_twilio,gchat,resolve}.py`
  (new), `app/backends/sms_stub.py` (deleted), `app/backends/registry.py`,
  `app/routers/{webhooks,me}.py`, `app/store/backends.py`, `app/tasks.py`, `app/main.py`,
  `relay/tests/{test_sms_twilio,test_gchat,test_tasks}.py` (new), `relay/tests/test_jobs.py`
  (import update only), `relay/.env.example`, `relay/docker-compose.yml` (comment only),
  `relay/pyproject.toml` (+`google-auth`, `google-cloud-tasks`, `python-multipart`),
  `relay/README.md`, this entry.
- **`PENDING_ACCOUNT: Twilio`** and **`PENDING_ACCOUNT: Google Chat`** — see `relay/README.md`'s
  new "Message backends" section for the full human runbook (number rental + US A2P 10DLC/toll-
  free registration for Twilio, a days-long manual carrier review; Google Workspace-vs-consumer-
  Gmail verification + Chat app console setup for gchat, per §11 D4 — if the family turns out to
  be on consumer Gmail, Email (§6.6, not built) is the documented fallback). Neither started here
  per this project's no-signups rule.

### server-architect security review — the SMS verification flow was bypassable, now fixed

Before commit, `server-architect` reviewed the two new webhook authenticators with the same
rigor `/webhooks/mqtt` got in Phase 2a. **The Twilio signature check and the Google Chat JWT
verification are both correct** (independently recomputed the Twilio fixture in a standalone
interpreter and confirmed a match; confirmed the Chat JWT path checks signature, audience *and*
issuer via the real `google-auth` library, not a hand-rolled or bypassed check). But the review
found the SMS *identity-claiming* flow around those authenticators had three real holes, plus two
related ones, all fixed by `backend-dev` before commit:

- **H1 (high):** the SMS verification code was stored in `users/{uid}/backends/{bid}.config`,
  which a user can read for their own uid via the normal Firestore-reads-through-rules path — so
  a user claiming a phone number they don't control could read the code straight out of Firestore
  and verify it **without ever receiving the SMS**. Anyone could claim any phone number. Fixed by
  moving the code to a new server-only `smsVerifyCodes/{bid}` collection (hashed, no
  `firestore.rules` match block, default-deny). This is not the same situation as `gchatLinkCodes`,
  which is correctly owner-readable (the user reads their own code to type into the Chat DM) —
  only the *inbound verification* material needed this fix.
- **H2 (high):** outbound SMS was sent to **unverified** numbers — `routing.py`'s fan-out only
  checked `backend.enabled`, and creating a backend defaulted `enabled=True`, so adding an sms
  backend with any phone number started texting it immediately, verify flow untouched. Fixed by
  forcing `enabled=False` at creation for backend kinds with a link flow (`sms`, `gchat`);
  `verify_backend()` already flips it to `True` on success.
- **H3 (high):** `phoneIndex` was never torn down — `PATCH`ing a verified backend's phone number
  left the old number's index entry live and didn't clear `verifiedAt` (a second bypass of H2:
  verify with your own number, then patch to a victim's), and `DELETE` left a stale entry
  attributing inbound SMS to a dangling backend id. Fixed: `PATCH`/`DELETE` now clear the phone
  index and `verifiedAt` on a phone change or deletion.
- **M1 (medium):** `phoneIndex` document ids were unnormalized user input — a mismatch with
  Twilio's E.164 `From` silently broke the lookup (failed closed, not exploitable, but dead), and
  a phone string containing `/` would have thrown an unhandled 500 as an illegal Firestore path.
  Fixed: phone numbers are normalized to E.164 before ever being used as a `phoneIndex` doc id,
  rejected if they don't validate.
- **M2 (medium):** any member of a linked Google Chat space could send as the linked user — the
  webhook mapped `space → (uid, bid)` on receipt and never checked who actually sent the message.
  Fixed: linking now requires the space be a DM (rejects a group/room), and every subsequent
  inbound message's sender is checked against who performed the original `/link`.
  `gchatSpaces` gained a `senderName` field for this; `smsVerifyCodes` and the `senderName`
  addition are both recorded in `docs/SERVER_PLAN.md` §3 alongside the original three-collection
  build-finding note.
- Two low-severity fixes folded in: Google Chat JWT verification now tolerates 30s of clock skew
  (was 0, risking spurious 401s from sub-second drift between Google's signer and Cloud Run); the
  webhook handler now redacts an unlinked inbound phone number to its last 4 digits in logs
  instead of logging it in full. A third low-severity bug (an `hmac.compare_digest` `TypeError`
  on a non-ASCII forged `X-Twilio-Signature` header, which would have 500'd instead of cleanly
  401ing) was found and fixed by `server-architect` directly during the review, before the H/M
  list above was even handed off.
- Re-verified after all fixes: `relay/tests` — **255 passed**. `tools/e2e_v2.py` — **all 9
  scenarios PASSED** against the real compose stack (the `fanout` scenario now completes a real
  SMS verify flow before sending, since H2 made an unverified backend correctly inert).

**Done when:** commit `v2 phase 7`. ✅

---

## Phase 8 — Hardening (2026-09-15)

**This is the last content phase.** Phases 0-9 are now all committed on branch `v2`.

- `backend-dev` built, per server-architect's Phase 7 bundled punch list: real OIDC ID-token
  verification for `/internal/tick` and `/internal/sweep` (`app/auth.py`'s
  `verify_internal_oidc_token`, reusing Phase 7's `google-auth` JWT pattern — signature via
  Google's certs, audience checked against `OIDC_AUDIENCE`, email checked against an
  `OIDC_ALLOWED_EMAILS` allow-list, not just "any valid Google identity"; `DEV_MODE` keeps its
  local-dev bypass, never set by Terraform); Firestore-backed, transactionally-atomic fixed-window
  rate limiting on `POST /api/me/backends` (Phase 7's M3 finding — real SMS cost per call),
  every `/api/admin/*` write route, and a per-IP cap on the two inbound webhooks; a
  `jobs.sweep()` pass for expired `gchatLinkCodes`; `/healthz` now genuinely checks Firestore and
  broker reachability per `SERVER_PLAN.md` §5.1 instead of a bare 200; structured JSON logging
  with a request-correlation-id middleware; `firestore.rules` test coverage pinning default-deny
  on all four server-only lookup collections.
- `server-architect`'s review fixed two real bugs directly:
  - **The per-IP webhook rate limiter was keyed on `request.client.host`**, which behind Cloud
    Run/Hosting is the proxy's own address for every caller — so all inbound Twilio/Chat traffic
    shared one bucket, making 30 forged requests/minute from anywhere a total, credential-free
    denial of service against real webhook delivery, running *before* signature verification even
    got a chance to reject them. Fixed to key on `X-Forwarded-For` with a same-address fallback —
    correctly reasoned as strictly better even though a forger can still pick their own bucket,
    since the actual security control (signature/JWT) is unaffected either way.
  - An `IndexError`-causing 500 (not a clean 401) on a bare `"Bearer "` token with no content on
    `/internal/*`; the identical bug pattern was found and fixed at `/webhooks/gchat` too (one
    orchestrator-applied fix, same pattern server-architect had just fixed on `/internal/*`).
- **The review's most consequential finding is about Phase 9, not Phase 8's own code**:
  `/internal/*` being fail-closed is correct, but it means **tick and sweep do not run at all in
  a real deployment** until Terraform actually wires `OIDC_AUDIENCE`/`OIDC_ALLOWED_EMAILS` onto
  the Cloud Run service. The implementing agent had flagged this as a two-apply Terraform
  bootstrap problem needing a module dependency restructure; the review **disagreed and is
  right**: Cloud Run v2's `custom_audiences` lets one shared variable feed both the service's env
  var and the Scheduler module's OIDC token audience in a single apply, and the scheduler service
  account's email is deterministic (`pager-scheduler@<project>...`) and needs no module
  restructure to compute in `envs/prod`. Documented precisely in
  `infra/modules/schedule/variables.tf` and `infra/README.md` for whoever next touches Terraform
  for real — **not attempted as a `.tf` edit here**, since `custom_audiences` behavior can't be
  verified without a real `apply`, which this build never runs. **This is the single most
  important thing for a human to action before a real deployment** — without it, retries and
  weekly retention silently never happen.
- Tests: `relay/tests` — **291 passed**. `tools/e2e_v2.py` — **all 9 scenarios ALL PASSED**,
  confirming the rate-limit tuning doesn't trip real e2e traffic.

**Done when:** commit `v2 phase 8`. ✅

---

## Final summary (2026-09-15) — the unattended run is complete

Phases 0-9 are all committed, green, on branch `v2`. Nothing pushed, nothing deployed, no cloud
account created, no money spent, `firmware/` untouched throughout. `docs/PROTOCOL.md` was edited
only in Phase 1, only by `server-architect`, per the plan's doc-discipline rule; every other
protocol-adjacent finding was recorded as a `(build finding — …)`/`(v2 decision — …)` note in
`SERVER_PLAN.md` or this file instead.

**What's built**: protocol v2 (device aliases, `kind`, the `/loc` topic — Phase 1); a request-
driven serverless transport replacing the always-on MQTT relay, EMQX's rule engine + REST publish
(Phase 2a); Cloud Firestore + Firebase Auth identity, replacing SQLite (Phase 2b); the routing
engine and pager/webapp delivery backends (Phase 3); device location tracking with on-demand
requests and coalescing (Phase 4); real weekly retention, the SMS mock, and the CI switch to
`e2e_v2.py` (Phase 5); the Next.js web app (Phase 6); SMS (Twilio) and Google Chat delivery
backends (Phase 7); OIDC-authenticated internal endpoints, rate limiting, structured logging
(Phase 8); Terraform for the whole GCP/Firebase footprint plus the deploy runbook (Phase 9).
`tools/pager_client.py` plays both the device and the parent for the full `tools/e2e_v2.py` suite
(9 scenarios), which supersedes the MVP's `tools/e2e_test.py` (deleted in Phase 5).

### Consolidated punch list for the human (everything logged as PENDING_ACCOUNT / BLOCKED / TODO
### anywhere above, gathered in one place)

**No `BLOCKED` items exist anywhere in this file.** Every phase reached green.

**PENDING_ACCOUNT (needs a human to open a real account — never attempted here, per the no-
signups rule):**
1. **EMQX Cloud Serverless** (Phase 2a) — open the free account; verify the rule engine supports
   an HTTP action on the Serverless tier, the REST publish API is available on it, and the
   webhook action's timeout can be set ≥ 15s (a cold Cloud Run relay can take 2-4s to respond).
   Record findings back into `SERVER_PLAN.md` §9.4.
2. **Twilio** (Phase 7) — number rental + US A2P 10DLC/toll-free registration, a manual,
   days-long carrier process, separate from any code here. See `relay/README.md`'s "Message
   backends" section.
3. **Google Chat** (Phase 7) — verify the family's Google accounts are on Workspace, not
   consumer Gmail (`SERVER_PLAN.md` §11 D4); third-party Chat apps are Workspace-only as of the
   last check. If consumer Gmail, Email (§6.6, not built) is the documented fallback slot.

**Before a real deployment — the single most load-bearing TODO in this list:**
4. **`/internal/tick` and `/internal/sweep` will not run at all in production** until Terraform
   wires `OIDC_AUDIENCE`/`OIDC_ALLOWED_EMAILS` onto the Cloud Run service (Phase 8). The fix is
   known and documented precisely (Cloud Run v2's `custom_audiences`, a single shared variable,
   one `apply` — not a two-apply bootstrap as first suspected) in
   `infra/modules/schedule/variables.tf` and `infra/README.md`. Without this, message retries and
   the weekly retention sweep silently never execute.
5. **Device MQTT credentials and ACLs are never pushed to any broker** (Phase 3, still true as
   of Phase 9's Terraform — `broker-gce` doesn't close this either). `POST /api/admin/devices`
   mints and returns a credential, but nothing provisions it against EMQX, so a minted device
   credential currently authenticates nothing. Part of the EMQX Cloud console runbook steps in
   `infra/README.md`.

**Smaller, non-blocking follow-ups (safe to defer, none affect correctness of what's built):**
6. `tools/sim_device.py`'s replacement, `pager_client.py`, never had a gap here — noted only
   because Phase 1 flagged it before Phase 4 closed it. No action needed.
7. Phase 6's two web-app gaps: `firestore.rules` gives non-admin members no server-side way to
   resolve a contact's alias (worked around client-side; needs a rules change or a contacts
   endpoint for a cleaner fix); device revoke has a store function but no mounted relay route.
8. Phase 7's `CloudTasksQueue` is a partial implementation (construction + payload shape only,
   per its scoped brief) — `POST /internal/task` doesn't exist yet and the existing `enqueue()`
   call sites pass closures a real out-of-process queue can't serialize. `TASKS_MODE` stays
   `inline` by default (Terraform-validated, can't be flipped by accident) until this is finished.
9. `smsVerifyCodes` (Phase 8 review, L3) isn't yet swept on expiry the way `gchatLinkCodes` now
   is — an abandoned SMS verification attempt accumulates forever. One-line addition to
   `jobs.sweep()` when someone's next in that file.
10. `docs/SERVER_PLAN.md` §5.9's index list is slightly behind the indexes Phases 5 and 7 added
    (`wireIds(createdAt)`, `conversations(lastMessageAt)`) — the doc predates those passes, not a
    contradiction, but worth a tidy-up pass.
11. `/healthz` does a live Firestore read + broker HTTP call on every request with no caching
    (Phase 8 review, L2) — cheap to add a ~10s cache if it ever becomes a load concern.

### Running the full test suite

```bash
# Relay unit tests (against the Firestore/Auth emulators)
cd relay
docker compose up -d firebase && sleep 10
.venv/bin/pytest -q
docker compose down -v

# Full end-to-end suite (EMQX + Firestore/Auth + relay + Twilio mock, all 9 scenarios)
cd relay
docker compose up -d --build && sleep 5
python3 tools/e2e_v2.py
docker compose down -v

# Web app
cd web
npm run build && npx tsc --noEmit && npm run lint

# Infra validation (never apply/plan against a real project)
cd infra
terraform fmt -check -recursive
for d in bootstrap envs/prod modules/*; do
  (cd "$d" && terraform validate -backend=false)
done
```

**Last commit on `v2`**: green, re-verified immediately before this entry was written
(`relay/tests`: 291 passed; `tools/e2e_v2.py`: all 9 scenarios PASSED). Nothing pushed — the
branch stays local for a human to review, per the kickoff brief.
