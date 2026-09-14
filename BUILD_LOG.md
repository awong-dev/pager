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
