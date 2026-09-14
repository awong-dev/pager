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
