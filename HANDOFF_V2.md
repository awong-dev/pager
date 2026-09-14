# HANDOFF_V2.md — build brief for the v2 server stack (orchestrator: Sonnet, unattended)

> **Status: not started.** Written 2026-09-14. This is the execution brief for a Claude Code
> session acting as **orchestrator** on the `sonnet` model, expected to run **unattended
> overnight**. It tells you what to build, in what order, with which subagent, and what to do
> when stuck with nobody to ask. Design decisions are *not* made here — they live in
> `docs/SERVER_PLAN.md` (the design) and `docs/PROTOCOL.md` (the wire contract). Read both
> before Phase 0. When this brief and `SERVER_PLAN.md` disagree, `SERVER_PLAN.md` wins; when
> either disagrees with `PROTOCOL.md` about anything the device sees, `PROTOCOL.md` wins.
>
> The MVP brief `HANDOFF.md` still describes the device, the topics and the phase history; its
> §2 sentence "Relay API … single long-lived process" is superseded by `SERVER_PLAN.md` §2.

## 1. What you are building (one paragraph)

A multi-user messaging service around the existing pager device. Users are registered by an
admin; each user has delivery backends (web app, SMS, Google Chat, the pager itself); an
allow-list says who may message and locate whom; the device reports periodic location and
answers on-demand location requests; a Next.js + MUI web app on Firebase Hosting is the primary
UI; data lives in Firestore; the relay is a scale-to-zero Cloud Run service fed by the MQTT
broker's rule engine over HTTPS; a Python test client plays the device and the parent for an
end-to-end suite. Costs ≈ $0/month. Full design: `docs/SERVER_PLAN.md`.

## 2. Unattended-run rules (read twice)

1. **Branch.** `git checkout -b v2` from `main` at the start. Commit at the end of every phase
   with message `v2 phase N: <one line>`. **Never push. Never rebase, reset --hard, or delete
   files you did not create.** Never touch `firmware/`.
2. **No accounts, no money, no cloud mutations.** Never sign up for any service (EMQX Cloud,
   Twilio, Firebase console…). Never run `terraform apply`, `terraform plan` against a real
   project, `firebase deploy`, or a mutating `gcloud` command. Anything that needs a human
   account goes into `BUILD_LOG.md` as `PENDING_ACCOUNT: <what, why, exact steps>` and into the
   relevant README as a runbook step.
3. **Local resources you may use freely:** Docker (compose up/down/build), ports 1883, 8000,
   8080, 9099, 4000, 18083, `pip`, `npm` (Node 24 is installed), `terraform init -backend=false
   && terraform validate`. Java is installed but the Firebase emulators run in Docker anyway.
4. **Delegate.** You are the orchestrator: plan, dispatch subagents, verify, commit. Do not
   write implementation code in the main context. Use `backend-dev`, `web-dev`, `infra-dev`,
   `docs-writer`; `server-architect` (opus) for reviews and escalation; `log-triage` (haiku)
   before reading any output longer than ~200 lines. Pipe test output through `| tail -40`.
5. **Stuck rule.** A subagent that reports blocked twice on the same problem → hand the exact
   error and the last 50 relevant log lines to `server-architect`. If that also fails, write
   `BLOCKED(<phase>.<step>): <error> / <what was tried>` into `BUILD_LOG.md`, commit what is
   green, and continue with the next step or phase that does not depend on it. **Never loop on
   the same failure more than 5 attempts total.**
6. **Green stays green.** The existing MVP tests (`relay/tests`, `tools/e2e_test.py`) keep
   passing until Phase 5 explicitly retires `e2e_test.py` and Phase 6 retires the legacy
   endpoints. Every phase's "done when" is a test you can run, not a claim.
7. **Log as you go.** `BUILD_LOG.md` at the repo root gets a dated entry per phase: what was
   done, the test summary lines, what was skipped and why, and any `TODO(orchestrator):` left
   by subagents. This is how the human resumes in the morning; write it for them.
8. **Order.** Phases 0–5 are mandatory and sequential. Phases 6, 7, 8, 9 may run in any order
   afterwards, and 6/7/9 may be dispatched in parallel subagents once 5 is committed. If you
   run out of session before finishing, the log is the deliverable — stop at a commit.
9. **Doc discipline.** `docs/PROTOCOL.md` is edited only in Phase 1, only per `SERVER_PLAN.md`
   §4, and only by `server-architect`. `SERVER_PLAN.md` is edited only to record findings
   (mark them `(build finding — …)`), never to change a decision.

## 3. Model policy

| Model | Used for |
|---|---|
| **opus** (`server-architect`) | PROTOCOL.md edits; design of the routing transaction and the security rules; review of Phases 1, 2, 3, 8 before commit; escalation |
| **sonnet** (orchestrator, `backend-dev`, `web-dev`, `infra-dev`) | Everything else |
| **haiku** (`docs-writer`, `log-triage`) | READMEs, `.env.example`, CI yaml, Dockerfiles from decided content; log summaries |

Never let `haiku` edit `relay/app/`, `web/src`, `infra/modules`, or `docs/PROTOCOL.md`.

## 4. Local stack you are building towards (`relay/docker-compose.yml`)

| Service | Image | Purpose | Ports |
|---|---|---|---|
| `emqx` | `emqx/emqx:5` | Broker with rule engine; config mounted from `relay/emqx/emqx.conf` (rules `pager/+/up`, `/status`, `/loc` → HTTP action `POST http://relay:8000/webhooks/mqtt` with header `X-Relay-Webhook-Key`; bootstrap API key file for the relay's REST publish) | 1883, 18083 |
| `firebase` | built from `relay/emulator.Dockerfile` (`node:20-slim` + `default-jre-headless` + `firebase-tools`) | `firebase emulators:start --only firestore,auth --project demo-pager` (a `demo-` project id needs no credentials) | 8080, 9099, 4000 |
| `relay` | `relay/Dockerfile` | FastAPI; env `FIRESTORE_EMULATOR_HOST=firebase:8080`, `FIREBASE_AUTH_EMULATOR_HOST=firebase:9099`, `GOOGLE_CLOUD_PROJECT=demo-pager`, `BROKER_API_URL=http://emqx:18083/api/v5`, `DEV_MODE=1`, `TASKS_MODE=inline` | 8000 |
| `twilio-mock` | tiny FastAPI in `tools/mocks/twilio_mock.py` | records outbound SMS, can be told to fail | 8010 |

`tools/emqx_setup.py` may replace the mounted config if declarative config proves awkward: it
logs into the EMQX dashboard API (`admin`/`public` default) and creates the connector, action and
rules idempotently. Either is fine; pick one and note it in `relay/README.md`.

## 5. Work plan

### Phase 0 — Housekeeping (sonnet orchestrator, haiku docs-writer)
- `git checkout -b v2`. Create `BUILD_LOG.md` with the start timestamp.
- `docs-writer`: add a 4-line status note at the top of `HANDOFF.md` §2 saying the relay
  transport is revised by `docs/SERVER_PLAN.md` §2 and pointing at `HANDOFF_V2.md`.
- Verify `.claude/agents/{backend-dev,web-dev,infra-dev,server-architect}.md` exist (they should).
- Run the MVP baseline once and record the summary lines in the log: `cd relay && pytest -q`,
  then `python3 tools/e2e_test.py | tail -20`.
- **Done when:** commit `v2 phase 0`.

### Phase 1 — Protocol v2 (opus `server-architect` writes; sonnet `backend-dev` codes)
- `server-architect` edits `docs/PROTOCOL.md` exactly per `SERVER_PLAN.md` §4.1–§4.8: alias
  `from`; optional `kind` on `/down`; new §13 "Location" (`/loc` topic, schema, QoS, rate limit
  §4.6); optional `to` on up messages; `loc_period_s`/`loc_min_s` on `/status`; §7 data-budget
  line; strike the §11 reservations; the §4.8 relay-transport paragraph. Each edit carries a
  `(v2 decision — reason)` note. Byte budget in §3.3 must show the new worst case (604 B).
- `backend-dev` updates `relay/app/wire.py`: `FROM_VALUES` → alias regex or `system`; `to`;
  `kind`; a `LocEnvelope` model; `tools/sim_device.py` accepts and ignores `kind:"loc_req"`.
  Tests in `relay/tests/test_wire.py` for every new shape, including a 604-byte worst case and
  a `loc_req` with a body (malformed).
- **Done when:** `pytest -q` green, `tools/e2e_test.py` green, `server-architect` has reviewed
  the PROTOCOL diff and its findings are applied. Commit `v2 phase 1`.

### Phase 2 — Serverless transport + Firebase foundation (sonnet `backend-dev`; opus review)
Split into two commits.

**2a — Transport.** `relay/app/broker.py` (`BrokerClient.publish(topic, payload, qos, retain)`
over EMQX REST `POST /api/v5/publish`; `verify_webhook(request)` on `X-Relay-Webhook-Key`;
`parse_webhook(body)` → `(topic, payload_bytes, qos)`), `relay/app/ingest.py` (the logic of
`mqtt_gateway.py`: `handle_up`, `handle_status` incl. online-edge re-publish, `handle_loc` stub),
`relay/app/routers/webhooks.py` with `POST /webhooks/mqtt`. Delete `mqtt_transport.py`, paho
from `pyproject.toml`, the background thread. Replace `eclipse-mosquitto` with `emqx` in compose
plus `relay/emqx/emqx.conf` (or `tools/emqx_setup.py`). Rewrite `tests/fake_transport.py` as a
fake `BrokerClient`; port `test_gateway.py` to post webhook payloads into `ingest`. Keep the
legacy `RELAY_TOKEN` endpoints working on the new transport and SQLite for now.
*Done when:* `pytest -q` green and `tools/e2e_test.py` passes **through EMQX's rule engine**
(the e2e script may need its compose health-check adjusted for EMQX's slower start).

**2b — Firestore + identity.** `relay/emulator.Dockerfile` + compose service; `relay/app/db/
firestore.py`; `relay/app/store/*` over `SERVER_PLAN.md` §3 exactly (collections, fields,
`seqCounter` transaction, `wireIds` create-in-txn); `relay/firestore.rules` and
`relay/firestore.indexes.json`; `relay/app/auth.py` (`verify_id_token`, registry gate, `admin`
custom claim); `routers/admin.py` (users, allowlist replace-all incl. `locatableBy` rewrite,
devices with one-time password + hash, settings); `python -m app.bootstrap --admin-email`;
`routers/dev.py` `POST /api/dev/token` (DEV_MODE only); `app/db/import_sqlite.py`. Move the
legacy endpoints and `ingest` onto Firestore; delete `store.py` (SQLite) and the migrations.
`tests/conftest.py` clears the emulator between tests via
`DELETE http://localhost:8080/emulator/v1/projects/demo-pager/databases/(default)/documents`.
*Done when:* pytest covers webhook auth, auth gate (unregistered UID → 403), admin CRUD,
allow-list rewrite, rules (a signed-in user cannot read another pair's message — test through the
emulator's REST API with an ID token), import of a seeded MVP SQLite file; MVP e2e still green.
`server-architect` reviews the transaction code and the rules file before commit.
Write `PENDING_ACCOUNT: EMQX Cloud Serverless — verify rule engine HTTP action, REST publish,
webhook timeout ≥ 15 s` into `BUILD_LOG.md` (SERVER_PLAN.md D2). Commit `v2 phase 2`.

### Phase 3 — Routing, core backends, first e2e (sonnet `backend-dev`; opus review)
- `relay/app/routing.py` per `SERVER_PLAN.md` §5.2 (one transaction per recipient; origin
  backend excluded; system reply on unknown `to`); `backends/base.py`, `registry.py`,
  `pager.py` (down envelope → `BrokerClient`; acks → delivery transaction; online-edge
  re-publish from `pendingDeviceIds`), `webapp.py` (FCM via a stubbable `messaging` client);
  `routers/conversations.py` (send, read receipt); `routers/me.py` (`/api/me`, backends, push
  tokens); `app/tasks.py` inline mode; `app/jobs.py` `tick()` (retry queued pager deliveries,
  cap 10/device) and `routers/internal.py`.
- `tools/pager_client.py` first cut per `SERVER_PLAN.md` §8: device side `connect/disconnect/
  crash/inbox/msg/ack/autoack/status/bytes`; server side `login/contacts/chat/say/watch/tick/
  admin user-add/allow/deny/device-add`. stdlib `cmd` + argparse subcommands + `--json`.
- `tools/e2e_v2.py` scenarios 1–4 (`bootstrap`, `text_roundtrip`, `allowlist`, `republish`
  including "broker API down → queued → tick delivers": stop the `emqx` container's API by
  pointing `BROKER_API_URL` at a dead port for one send, then restore).
- **Done when:** scenarios 1–4 pass against compose; `pytest -q` green; `server-architect`
  review applied. Commit `v2 phase 3`.

### Phase 4 — Location (sonnet `backend-dev`)
- `relay/app/location.py` per §5.6: `/loc` ingest (dedup, `devices/{d}/locations`, `req` →
  fulfil + `kind='loc'` thread message per requester), `/locate` with `locReqs/{d}` coalescing
  and the 60 s cached answer, derived expiry, `tick` clearing stale `locReqs`.
- Client: `loc`, `loc auto [--walk]`, `loc min`, `loc fail`, `locate`, `locations`,
  implementing §4.6 exactly.
- Scenarios 5–6.
- **Done when:** scenarios 5–6 pass; unit tests for coalescing, cached, `no_fix`, expiry.
  Commit `v2 phase 4`.

### Phase 5 — Client complete, CI switch (sonnet `backend-dev`, haiku `docs-writer`)
- Remaining client commands (`admin settings`, `sweep`), `tools/mocks/twilio_mock.py`,
  scenarios 7 (`fanout` with a stub `sms` backend hitting the mock, including the failure/retry
  path with `TASKS_MODE=inline`), 8 (`retention`, needs `jobs.sweep` — implement it now per
  §5.7 with `{n, unit}` settings and a mid-run abort test), 9 (`bytes`).
- `.github/workflows/ci.yml`: replace `tools/e2e_test.py` with `tools/e2e_v2.py`; delete
  `tools/sim_device.py` and `tools/e2e_test.py`; re-point `tools/send.py` at
  `POST /api/conversations/{alias}/messages` with a dev token.
- `docs-writer`: `tools/README.md`, refresh `relay/README.md` and `relay/.env.example`.
- **Done when:** `tools/e2e_v2.py` all scenarios green locally; CI yaml updated. Commit
  `v2 phase 5`.

### Phase 6 — Web app (sonnet `web-dev`)
- `web/` per `SERVER_PLAN.md` §7: Next.js static export, MUI, Firebase SDK against the
  emulators, routes `/login`, `/chat`, `/chat/[alias]`, `/settings/backends`,
  `/settings/notifications`, `/admin/users`, `/admin/allowlist`, `/admin/devices`,
  `/admin/settings`; `firebase-messaging-sw.js`; `web/firebase.json` with Hosting rewrites;
  `web/README.md` with a manual checklist.
- Then `backend-dev`: delete `relay/static/index.html`, the static mount, and the legacy
  `RELAY_TOKEN` endpoints + their tests.
- **Done when:** `npm run build`, `tsc --noEmit`, `next lint` green; e2e still green after the
  legacy removal. Commit `v2 phase 6`.

### Phase 7 — SMS + Google Chat adapters (sonnet `backend-dev`)
- `backends/sms_twilio.py`, `backends/gchat.py`, `routers/webhooks.py` additions with signature
  / JWT verification, link flows, recorded-fixture tests; Cloud Tasks client in `tasks.py`
  (prod mode, untested locally beyond construction).
- `PENDING_ACCOUNT:` entries for Twilio (10DLC) and Google Chat (Workspace check), with the
  exact console steps, in `BUILD_LOG.md` and `relay/README.md`.
- **Done when:** fixture tests green; scenario 7 still green. Commit `v2 phase 7`.

### Phase 8 — Hardening (sonnet `backend-dev`; opus review)
- `/healthz`, structured JSON logging, `/internal/*` OIDC verification (audience = service URL,
  with a `DEV_MODE` bypass), rate limits on `/api/admin/*` writes, `jobs.sweep` idempotency.
- `server-architect` reviews the three webhook authenticators, `auth.py`, `firestore.rules`,
  and `routing.py` for the invariants in its agent file; apply findings.
- **Done when:** review findings closed; all tests green. Commit `v2 phase 8`.

### Phase 9 — Terraform + runbook (sonnet `infra-dev`, haiku `docs-writer`)
- `infra/` per `SERVER_PLAN.md` §9.1–§9.2 (modules `firebase`, `relay-service`, `schedule`,
  `secrets`, `ci-deploy`, `broker-gce`; `envs/prod`; `bootstrap/`).
- `.github/workflows/deploy.yml` (build → Artifact Registry → `terraform apply` → `firebase
  deploy`), gated on `main` and on secrets that do not yet exist — it must be inert until a
  human wires WIF.
- `infra/README.md` runbook: create project, enable Blaze, bootstrap state bucket, secrets,
  EMQX Cloud console steps (rules, HTTP action, API key, device credentials + ACLs), first admin,
  custom domain, and the Phase 9 cold-start measurement procedure from `SERVER_PLAN.md` §9.3.
- **Done when:** `terraform fmt -check` and `terraform validate` pass in every module and in
  `envs/prod` with `-backend=false`. Commit `v2 phase 9`.

## 6. Definition of done for the overnight run
- Phases 0–5 committed on branch `v2` with green tests, or a `BLOCKED` entry explaining exactly
  why not.
- `BUILD_LOG.md` readable top-to-bottom by a human who was asleep.
- Nothing pushed, nothing deployed, no account created, no money spent.

## 7. Kickoff

Run from the repo root on a machine that will not sleep (`caffeinate -i` on macOS). Non-interactive
print mode exits when the run ends; `tee` keeps the transcript. `--dangerously-skip-permissions`
is what makes it unattended — the rules in §2 are the safety net, so do not run this in a
directory containing anything you would miss.

```bash
cd ~/src/pager && git status --porcelain | grep -q . && echo "commit or stash first" || \
caffeinate -i claude --model sonnet --dangerously-skip-permissions --output-format text \
  -p "$(cat docs/kickoff-v2.md)" 2>&1 | tee -a build-v2.log
```

The prompt file `docs/kickoff-v2.md` is short on purpose: the brief is this file.
