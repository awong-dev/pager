# DEVICE_TASKS.md — execution plan for `docs/DEVICE_PLAN.md`

**Audience:** an implementing agent (a smaller model is fine) working one task at a time.
**Source of truth:** `docs/DEVICE_PLAN.md` for design, `docs/PROTOCOL.md` for the wire. When this
file and those disagree, those win; fix this file.

## How to run a task

1. Read the `Read` list for the task. Do not skim the whole plan; read the sections named.
2. Touch only the files listed under `Files` (plus their tests). If you must touch another file,
   stop and say why before doing it.
3. Do the `Do` steps in order. Each is small on purpose.
4. Run every command under `Verify`. A task is done only when they all pass. Paste the last lines
   of each command's output in your report.
5. Commit with the task id first in the subject, e.g. `S1.2: devauth sign/verify + window`.
6. Never edit `docs/PROTOCOL.md` outside task **D0.1**. Never patch anything under
   `firmware/managed_components/`. Do not add firmware dependencies beyond what a task names.

**Tracks** run in parallel; tasks inside a track run in order. `D` docs, `S` server (`relay/`),
`T` tools, `W` web, `F` firmware. Suggested agents: `docs-writer` for D, `backend-dev` for S and T,
`web-dev` for W, `firmware-dev` for F, with `server-architect` / `firmware-architect` as escalation.

**Standing verification commands** (from `.github/workflows/ci.yml`, `relay/README.md`,
`web/README.md`, `firmware/README.md`):

```bash
# server unit tests (needs the emulators)
cd relay && docker compose up -d firebase && pytest -q
# end-to-end (brings up and tears down the whole stack itself)
relay/.venv/bin/python tools/e2e_v2.py
# web
cd web && npm run build && npx tsc --noEmit && npm run lint
# firmware (ESP-IDF 5.2 environment)
cd firmware && idf.py set-target esp32s3 && idf.py build
```

---

## Track D — documents

### D0.1 PROTOCOL.md edits
- **Read:** `DEVICE_PLAN.md` §8 (the numbered list is the spec for this task), §2.4, §2.5, §3.2
  step 4–5, §4.3, §5.6, §5.8.
- **Files:** `docs/PROTOCOL.md`.
- **Do:** apply §8 items 0–14 in order. Concretely:
  - §3: encoding paragraph per item 0; first-byte dispatch rule (`0x7B` JSON, `0xA0–0xBF` CBOR).
  - §3.1 table: add `n`, `sig`, `bv`, `name`, `ph`, `d`, `c`, `p`, `more`, and the `cfg`
    `lock` map, each with type, where-allowed and range, per §8 item 1–2.
  - §3.2: add rows for `/up contact_req`, `/down book`, `/down cfg`; state the rules for each.
  - §2: add the `pager/boot/{bid}/down|up` rows and the 4 kB limit scoped to that namespace.
  - §3.3: add the CBOR column and the `n`/`sig` lines.
  - §3.4: signature/window failure is malformed; the unsigned-LWT exception.
  - §4: the locked-device `shown` rule (item 2a).
  - §5.1: `bv`; `rssi` is now published. §5.3: `book`/`cfg` re-publish, newest only.
  - §6.1: the bootstrap TLS profile row (item 14).
  - §7.2/7.3: re-derived rows per item 8.
  - §9: item 9 (bodies to NVS, RTC table, §9.4 withdrawal, §9.6 NVS column).
  - §10: promote to normative; the full keymap per item 10.
  - §11: `book`/`cfg` spent as kinds; `/cfg` topic stays reserved.
  - §12: item 13. New **§14 Device authentication** = `DEVICE_PLAN.md` §2.3–2.6 verbatim in
    normative voice.
- **Verify:** `grep -n 'sig\|"n"\|book\|cfg\|boot/' docs/PROTOCOL.md` shows every addition;
  no other file changed (`git status`).

### D0.2 SERVER_PLAN.md data model
- **Read:** `DEVICE_PLAN.md` §2.6, §3.2, §4.1.
- **Files:** `docs/SERVER_PLAN.md` §3 and §5.1.
- **Do:** add `deviceSecrets/{deviceId}`, `setupCodes/{bid}`, `contactRequests/{deviceId}_{reqId}`
  to the collection sketch, the `devices/{d}` new fields (`provisionState`, `bookVersion`, `wire`,
  `authMode`, `status.authAlarm`), and the new endpoints to §5.1 (they are listed in S2.2, S2b.1,
  S4.1, S4.2 below). Mark `deviceSecrets` and `setupCodes` as server-only in the rules sketch.
- **Verify:** `git diff --stat` touches only that file.

### D8.1 README updates (last)
- **Files:** `README.md`, `relay/README.md`, `firmware/README.md`, `web/README.md`.
- **Do:** describe the setup-code flow for a household admin (`Devices → Add device`, type the
  code on the pager), the new test commands, the M9/M13 additions from `DEVICE_PLAN.md` §7 phase 8.
- **Verify:** links resolve; `git diff --stat` is docs only.

---

## Track S — server

### S1.1 `deviceSecrets` store
- **Read:** `DEVICE_PLAN.md` §2.6; `relay/app/store/devices.py`; `relay/app/store/backends.py`
  lines 1–62 (the server-only collection idiom); `relay/tests/test_rules.py`.
- **Files:** new `relay/app/store/device_secrets.py`; `relay/tests/test_device_secrets.py`;
  `relay/tests/test_rules.py`.
- **Do:**
  - `create(device_id, *, hmac_key: bytes, mqtt_password_hash: str)` → doc
    `{hmacKey: base64, mqttPasswordHash, upN: 0, upBits: 0, downN: 0, sigFailures: 0, createdAt}`.
  - `get(device_id)`, `rotate(device_id, hmac_key, mqtt_password_hash)` (sets `rotatedAt`, zeroes
    counters), `delete(device_id)`.
  - `accept_up_n(device_id, n) -> bool` as a **transaction**: window rule from §2.5 (64 wide);
    returns False and increments nothing on replay.
  - `next_down_n(device_id) -> int` as a transaction.
  - `bump_sig_failures(device_id) -> int`.
  - Move `mqttPasswordHash` here; `devices_store.create_device` no longer stores it.
  - In `test_rules.py`, pin that `deviceSecrets/*` is unreadable by an owner and by an admin
    client (same pattern as the `phoneIndex` test).
- **Verify:** `pytest -q relay/tests/test_device_secrets.py relay/tests/test_rules.py`.

### S1.2 `devauth.py` — sign, verify, both encodings
- **Read:** `DEVICE_PLAN.md` §2.4 (all of it), §2.5; `tools/authvectors.json` does not exist yet —
  you create it here.
- **Files:** new `relay/app/devauth.py`, `relay/app/wirecbor.py`; `relay/tests/test_devauth.py`;
  new `tools/authvectors.json`; `relay/pyproject.toml` (add `cbor2`).
- **Do:**
  - `wirecbor.py`: `KEYMAP` exactly as `PROTOCOL.md` §10 after D0.1 (envelope 0–20, status
    21–28, boot 29–37, cfg 38; sub-maps `loc`, `contact`, `request`, `lock`). `encode(obj: dict)
    -> bytes` (definite-length, integer keys, values unchanged, `sig` as bstr) and
    `decode(b: bytes) -> dict` (back to the JSON names). `is_cbor(b) -> bool` by first byte.
  - `devauth.py`:
    - `tag(key: bytes, topic: str, p: bytes) -> bytes`: `hmac.new(key, topic.encode()+b"\x00"+p,
      "sha256").digest()[:8]`.
    - `sign_cbor(key, topic, obj) -> bytes`: encode with map count +1, append `0x0D 0x48` + tag
      over everything before.
    - `sign_json(key, topic, obj) -> bytes`: minified JSON without `sig`, then the `,"sig":"…"}`
      rule with base64url no padding.
    - `verify(key, topic, payload: bytes) -> tuple[bool, bytes]`: detect encoding; for CBOR require
      the trailing 10 bytes shape; for JSON require the trailing `,"sig":"<11>"}`; `hmac.compare_digest`;
      return `(ok, payload_without_sig)`. Never parse before verifying.
  - `tools/authvectors.json`: at least 8 vectors: `{key_b64, topic, obj, json_signed_b64,
    cbor_signed_b64}` covering down msg, ack, up msg with `to`, status, loc, book, cfg, contact_req.
    Generate them with the code above, then hand-check one CBOR vector byte by byte in the test.
  - Tests: every vector round-trips in both encodings; flipping any byte fails; a payload with
    `sig` not last fails; `decode(encode(x)) == x` for every vector.
- **Verify:** `pytest -q relay/tests/test_devauth.py`.

### S1.3 Ingest verifies before it parses
- **Read:** `DEVICE_PLAN.md` §2.6; `relay/app/ingest.py` (device id from topic at lines ~52–57;
  the 640 check; where `wire.py` parsing happens); `relay/app/routers/webhooks.py` 143–183.
- **Files:** `relay/app/ingest.py`, `relay/app/wire.py`, `relay/app/store/devices.py`,
  `relay/tests/test_ingest.py`, `relay/tests/test_webhooks.py`.
- **Do:**
  - `devices/{d}` gains `authMode: "password"|"hmac"` (default `"hmac"` for new devices) and
    `wire: "json"|"cbor"` (set from each accepted inbound envelope's encoding).
  - In ingest, after the size check and before any parse: load secrets (cache per process,
    invalidate on rotate); if `authMode == "hmac"`: `devauth.verify`; on failure → log
    `SECURITY bad-sig device=… topic=… first64=…`, `bump_sig_failures`, return (still 2xx).
    Then decode (CBOR via `wirecbor.decode`, else JSON), then `accept_up_n`; replay → log
    `SECURITY replay` and return.
  - The unsigned-LWT exception: an unsigned `/status` is accepted only if the decoded object is
    exactly `{v, state:"offline", session}`.
  - `sigFailures` ≥ 20 in 10 min → `devices/{d}.status.authAlarm = true` (cleared on rotate).
  - Unknown `kind` on `/up`: drop and count, 2xx (same as unknown kind on `/down` today).
- **Verify:** `pytest -q relay/tests/test_ingest.py relay/tests/test_webhooks.py`; the new tests
  cover: valid signed JSON, valid signed CBOR, unsigned from an `hmac` device (dropped),
  replayed `n` (dropped), `n` inside the window but unseen (accepted), LWT unsigned (accepted),
  an `hmac` device's `wire` flips to `cbor` after one CBOR envelope.

### S1.4 One signed publish path for `/down`
- **Read:** `DEVICE_PLAN.md` §2.5–2.6; `relay/app/broker.py`; `relay/app/backends/pager.py`;
  `relay/app/routing.py` re-publish path.
- **Files:** `relay/app/broker.py`, `relay/app/backends/pager.py`, tests.
- **Do:** every `/down` publish goes through `broker.publish_down(device_id, obj)`: reads
  `devices/{d}.wire` and `authMode`, takes `next_down_n`, signs with `devauth.sign_cbor` or
  `sign_json`, publishes. No other code path may publish to `/down`. Re-publish on an online edge
  reuses `id`, gets a fresh `n`.
- **Verify:** `pytest -q relay/tests/test_messages.py relay/tests/test_routing.py` plus a new test
  asserting two publishes of the same message have different `n` and both verify.

### T1.5 `pager_client.py` signs and speaks both encodings
- **Read:** `DEVICE_PLAN.md` §2.4; `tools/pager_client.py` device side (publish and on-message).
- **Files:** `tools/pager_client.py`, `tools/e2e_v2.py`.
- **Do:** the simulated device takes `--hmac-key <b64>` and `--wire json|cbor` (default `cbor`);
  signs every publish with `relay.app.devauth`; verifies every `/down` and drops bad ones with a
  log line; keeps its own `n` (epoch/lo split as §2.5) and a 32-wide down window. `e2e_v2.py`
  creates devices through the admin API and passes the key through. Unsigned mode stays behind
  `--hmac-key ""` for the `authMode: password` case.
- **Verify:** `relay/.venv/bin/python tools/e2e_v2.py` all scenarios green in both `--wire`
  modes (run twice).

### S2.1 `emqx_admin.py`
- **Read:** `DEVICE_PLAN.md` §3.2 steps 2–3; `relay/app/broker.py` (API key, base URL);
  `tools/emqx_setup.py`; EMQX 5 HTTP API: `POST /api/v5/authentication/password_based:built_in_database/users`,
  `DELETE …/users/{user_id}`, `PUT /api/v5/authorization/sources/built_in_database/rules/users/{username}`.
- **Files:** new `relay/app/emqx_admin.py`; `relay/tests/test_emqx_admin.py` (httpx mock);
  `relay/emqx/emqx.conf` (enable built-in-database auth + authz, deny-by-default authz);
  `relay/docker-compose.yml` if the config path changes; `tools/emqx_setup.py` (creates the
  `relay` credential with the mirror-image ACL).
- **Do:** `ensure_device(username, password, device_id)`, `delete_user(username)`,
  `ensure_boot_user(bid, password)`; ACL rules exactly as `PROTOCOL.md` §2 and §13.1 for devices,
  §2's boot rows for boot users. A `BROKER_MANAGES_AUTH=0` config flag makes every call a no-op
  that returns `"manual"`.
- **Verify:** `pytest -q relay/tests/test_emqx_admin.py`; then `docker compose up -d` and prove by
  hand with `mosquitto_pub` that an unknown user is refused and a device user cannot publish to
  another device's topic; record the two commands in the PR description.

### S2.2 Device create / rotate / revoke return a setup code
- **Read:** `DEVICE_PLAN.md` §3.1, §3.2 (relay side), §3.5; `relay/app/routers/admin.py` 236–310.
- **Files:** `relay/app/routers/admin.py`, `relay/app/store/devices.py`, tests
  (`test_admin.py`).
- **Do:**
  - `POST /api/admin/devices` response becomes `{device, setupCode, expiresAt, brokerPush:
    "pushed"|"manual", manualAcl?: [str]}`. Internally: password + `hmacKey` → `deviceSecrets`
    (S1.1) → `emqx_admin.ensure_device` → setup code via S2b.1's `devsetup.issue()`.
  - `POST /api/admin/devices/{id}/rotate-credentials` → same shape; old broker user deleted,
    `deviceSecrets.rotate`.
  - Mount `POST /api/admin/devices/{id}/revoke` (store function exists,
    `relay/app/store/devices.py:100-107`); also `emqx_admin.delete_user`.
  - Keep `ownerAlias`/`defaultToAlias` on create; `provisionState: "issued"`.
- **Verify:** `pytest -q relay/tests/test_admin.py`.

### S2b.1 Setup codes and the bootstrap bundle
- **Read:** `DEVICE_PLAN.md` §3.1, §3.2 in full, §8 item 10 (boot keys).
- **Files:** new `relay/app/devsetup.py`, `relay/app/store/setup_codes.py`;
  `relay/tests/test_devsetup.py`; `relay/pyproject.toml` (ensure `cryptography`);
  `tools/authvectors.json` (add a token vector).
- **Do:**
  - `token()`: 8 random bytes → 13 Crockford base32 chars + 1 Crockford check char; `parse(code)`
    → `(token_bytes, host, port, apn)` accepting spaces/hyphens/case, `@`, `:port`, `;apn=`.
  - `derive(token) -> (bid, bpw, bkey)`: HKDF-SHA256 with info `b"id"`, `b"pw"`, `b"bundle"`;
    `bid = hex[:12]`, `bpw = base64url(32 B)`, `bkey = 32 B`.
  - `bundle(device) -> bytes`: CBOR map with boot keys 0,1,30–37; AES-256-GCM, 12-byte random
    nonce, output `nonce ‖ ct ‖ tag`.
  - `issue(device_id)`: derive → `emqx_admin.ensure_boot_user(bid, bpw)` → publish bundle
    **retained** to `pager/boot/{bid}/down` (add `retain` support to `broker.publish_raw`) →
    `setupCodes/{bid} = {deviceId, expiresAt: +10 min}` → return the code string. Never store the
    token.
  - `complete(bid)`: on `pager/boot/{bid}/up` = `{v:1, ok:1}`: publish an empty retained message to
    clear, `delete_user("boot-"+bid)`, delete `setupCodes/{bid}`.
  - `expire()`: called from `jobs.tick`; same cleanup for `expiresAt < now`.
  - Add a vector: `{token_b64, code, bid, bpw, bkey_b64, bundle_plain_obj, bundle_ct_b64}`.
- **Verify:** `pytest -q relay/tests/test_devsetup.py` including: check-character rejects a
  one-character typo; derive matches the vector; decrypting the vector bundle yields the object.

### S2b.2 `ca_resolve.py`
- **Read:** `DEVICE_PLAN.md` §3.3.
- **Files:** new `relay/app/ca_resolve.py`, `relay/app/config.py`, `relay/tests/test_ca_resolve.py`.
- **Do:** `BROKER_CA_PEM` from config wins; else at startup connect to `BROKER_HOST:8883`, take the
  served chain (use `ssl` + `cryptography` to parse; if the root is not served, find it in
  `certifi` by issuer name), cache the PEM, expose it on `GET /api/admin/settings` as
  `brokerCaSubject`. On failure log a warning and leave `ca` empty in bundles, which the device
  reports as "broker certificate not trusted".
- **Verify:** unit test with a self-signed chain fixture; `pytest -q relay/tests/test_ca_resolve.py`.

### S2b.3 Bootstrap topic through the webhook
- **Read:** `DEVICE_PLAN.md` §3.2 (relay, on `/boot/+/up`); `tools/emqx_setup.py`.
- **Files:** `tools/emqx_setup.py` (rule for `pager/boot/+/up`), `relay/app/routers/webhooks.py`,
  `relay/app/ingest.py`.
- **Do:** route `pager/boot/{bid}/up` to `devsetup.complete(bid)` after decoding the CBOR
  `{0:1, 29:1}`; `provisionState` → `"provisioned"` on the first **signed** `/status` from that
  device (in S1.3's path).
- **Verify:** `pytest -q relay/tests/test_webhooks.py`.

### T2b.4 `pager_client.py --bootstrap`
- **Files:** `tools/pager_client.py`.
- **Do:** `--bootstrap "<code>"`: parse, derive, connect as `boot-{bid}`/`bpw` with TLS
  verification off (or plain 1883 in the compose stack — mirror what the firmware will do: no CA
  check), subscribe `pager/boot/{bid}/down`, decrypt the retained bundle, publish `ok`, disconnect,
  then continue as a normal signed device with the bundle's credentials.
- **Verify:** manual run against `docker compose up -d`; output shows the four steps.

### S2b.5 e2e scenario `setup_code`
- **Files:** `tools/e2e_v2.py`.
- **Do:** admin creates a device → code → `--bootstrap` device comes online → `provisionState ==
  "provisioned"` (whitebox Firestore read) → the boot user is gone → an expired code (patch
  `expiresAt`) yields no retained message. Also run the scenario with `BROKER_MANAGES_AUTH=0`
  and the shared `boot` user documented in §3.2.
- **Verify:** `relay/.venv/bin/python tools/e2e_v2.py setup_code`.

### S4.1 Contact requests
- **Read:** `DEVICE_PLAN.md` §4.1–4.3; `relay/app/store/allow.py`; `relay/app/store/backends.py`
  verify path; `relay/app/routers/admin.py` allowlist section.
- **Files:** new `relay/app/store/contacts.py`; `relay/app/ingest.py`; `relay/app/routers/admin.py`;
  `relay/firestore.rules`; tests.
- **Do:**
  - Ingest `/up kind: contact_req` (`name`, `ph` or `alias`) → `contactRequests/{deviceId}_{id}`
    `{status: "pending", …}`; dedup by `id`; no-op if an equal `ph`/`alias` is pending or approved;
    reject beyond 5 pending with one `system` down `too many pending requests`.
  - `GET /api/admin/contacts?status=pending`, `POST /api/admin/contacts/{key}/approve`
    `{mode: "link"|"create", alias?, locate?: bool}`, `POST …/reject {reason}`.
  - Approve `create`: `users_store.create_user(alias, displayName=name)` + admin-created backend
    via new `POST /api/admin/users/{uid}/backends` (`verifiedAt` set, `adminVerified: true`,
    `phoneIndex` written) + two `allow` edges + `recompute_locatable_by_for_owner`.
  - Every approve/reject bumps `devices/{d}.bookVersion` and calls S4.2's `push_book`.
  - Rules: `contactRequests/*` readable by admin and by `resource.data.ownerUid`.
- **Verify:** `pytest -q relay/tests/test_contacts.py relay/tests/test_rules.py`.

### S4.2 `devcfg.py`: `book` and `cfg` down messages
- **Read:** `DEVICE_PLAN.md` §4.3, §5.8 (cfg), §8 items 2, 7, 10.
- **Files:** new `relay/app/devcfg.py`; `relay/app/routers/admin.py`; `relay/app/ingest.py`
  (`bv` on status); tests.
- **Do:**
  - `build_book(device_id) -> obj`: `d` = default recipient alias, `c[]` = approved contacts (`a`,
    `n`, `t`), `p[]` = the device's non-approved requests (`n`, `s`), `bv`; cap 10 + 4; assert the
    signed CBOR is ≤ 640 bytes.
  - `push_book(device_id)`: mark older unacked `book` messages for this device `expired`, store a
    message row with `kind: "book"`, publish via `broker.publish_down`. Acked `shown` like any
    message; included in the online-edge re-publish (newest only).
  - `push_cfg(device_id, lock: dict)` same shape with `kind: "cfg"`.
  - `POST /api/admin/devices/{id}/cfg {lock: {clear?: true, auto?: int}}`.
  - On `/status` with `bv < devices.bookVersion` → `push_book`.
- **Verify:** `pytest -q relay/tests/test_devcfg.py`.

### S4.5 e2e scenario `address_book`
- **Files:** `tools/e2e_v2.py`, `tools/pager_client.py` (handle `book`/`cfg` and send
  `contact_req`).
- **Do:** device requests `+15550001111 Grandma` → admin approves with `create`, alias `grandma` →
  device receives `book` with `grandma` and acks → device sends `to: grandma` → Twilio mock
  receives it; then admin sends `cfg lock.auto=2` → device acks.
- **Verify:** `relay/.venv/bin/python tools/e2e_v2.py address_book`.

---

## Track W — web

### W2.3 Devices page: Add device wizard, rotate, revoke
- **Read:** `DEVICE_PLAN.md` §3.2 (web), §6 web row; `web/app/admin/devices/page.tsx`.
- **Files:** `web/app/admin/devices/**`, `web/lib/api.ts` (or where API calls live).
- **Do:** wizard (label, owner, default recipient) → shows `setupCode` big, a copy button, a QR
  (`qrcode` npm package, render to canvas), the two-step instruction from §3.2, `expiresAt`
  countdown; polls `devices/{d}` via the existing Firestore listener and flips to "online" when
  `provisionState == "provisioned"`. `manualAcl` lines shown when `brokerPush == "manual"`.
  Rotate reuses the same panel. Revoke calls the mounted route.
- **Verify:** `cd web && npm run build && npx tsc --noEmit && npm run lint`; manual checklist
  entry added to `web/README.md`.

### W4.4 Contacts page and device cfg buttons
- **Read:** `DEVICE_PLAN.md` §4.3, §5.8.
- **Files:** new `web/app/admin/contacts/page.tsx`; `web/app/admin/devices/**`.
- **Do:** pending list (device, name, phone/alias, age) → approve dialog (link existing user
  suggested from `phoneIndex` match, or create with alias field pre-filled from the slug, empty
  for CJK; `locate` checkbox) → reject with reason. Device page: *Clear passcode* and *Auto-lock*
  select → `POST /cfg`.
- **Verify:** same commands as W2.3.

---

## Track F — firmware

Every F task ends with `idf.py build` clean and, where a host test exists, `cd firmware/host &&
make test` (create `firmware/host/` in F3.2: a plain `Makefile` compiling the listed `.c` files
with the host compiler against tiny stubs; no ESP-IDF needed).

### F3.1 Partition table, NVS, `ident.c`
- **Read:** `DEVICE_PLAN.md` §3.4, §2.7; `firmware/main/net.cpp` 33–94 (the constexpr block).
- **Files:** new `firmware/partitions.csv`, `firmware/main/ident.c/h`; `firmware/CMakeLists.txt`
  / `sdkconfig.defaults` (`CONFIG_PARTITION_TABLE_CUSTOM=y`, name the csv); `firmware/main/CMakeLists.txt`
  (`REQUIRES … nvs_flash`).
- **Do:** partitions: `nvs 24K`, `assets 1M`, `otadata`, `ota_0` (app, sized from the current
  build + 1 MB), `ota_1` optional. `ident_load() -> bool` reads namespace `ident` keys `dev_id`,
  `mqtt_pw`, `kdev`(blob 32), `host`, `port`, `ca`, `apn`, `flags`, `label`, `ca_hash`, `n_epoch`,
  `claimed`; validates `dev_id` with the §1 regex; `ident_get_*()` getters; `ident_store(const
  ident_t*)`, `ident_erase()`. `main.c` calls `nvs_flash_init()` then `ident_load()`; on false the
  boot goes to Setup (F3.5) — until F3.5 exists, log `IDENT missing` and halt in a loop.
- **Verify:** `idf.py build`; boot log prints `IDENT <dev_id> sig=<0|1> claimed=<0|1>` or
  `IDENT missing`.

### F3.2 `cbor.c` and the host test harness
- **Read:** `DEVICE_PLAN.md` §2.4 (Firmware codec), §8 item 10; `tools/authvectors.json`.
- **Files:** new `firmware/main/cbor.c/h`; new `firmware/host/Makefile`, `firmware/host/test_cbor.c`.
- **Do:** encoder over a caller buffer: `cbor_w_init(w, buf, cap)`, `cbor_w_map(w, n)`,
  `cbor_w_uint(w, key, u64)`, `cbor_w_nint`, `cbor_w_tstr(w, key, s, len)`, `cbor_w_bstr`,
  `cbor_w_bool`, `cbor_w_null`, `cbor_w_f64`, `cbor_w_array(w, key, n)`, nested map; all return
  false on overflow. Decoder: `cbor_r_init(r, buf, len)`, `cbor_r_map(r, &count)`,
  `cbor_r_key(r, &key)`, typed getters, `cbor_r_skip(r)` for unknown keys. No malloc. Reject
  indefinite lengths. The host test decodes every vector's `cbor_signed_b64` (minus the last 10
  bytes) and re-encodes it byte-identically.
- **Verify:** `cd firmware/host && make test`; `idf.py build`.

### F3.3 `auth.c`
- **Read:** `DEVICE_PLAN.md` §2.4, §2.5, §2.7.
- **Files:** new `firmware/main/auth.c/h`; `firmware/host/test_auth.c`; `firmware/main/CMakeLists.txt`
  (`REQUIRES … mbedtls`).
- **Do:** `auth_sign(topic, buf, &len, cap)` appends `0x0D 0x48 tag`; `auth_verify(topic, buf,
  &len)` checks and trims; `auth_next_up_n()` (epoch from `ident`, `lo` in RTC — expose a small
  `auth_rtc_t {up_lo, down_n, down_bits}` that `modes.c` embeds); `auth_accept_down_n(n)` with the
  32-wide window. HMAC via `mbedtls_md_hmac`. Host test: every vector verifies and re-signs to the
  identical bytes; a flipped byte fails; the window accepts/rejects per §2.5.
- **Verify:** `cd firmware/host && make test`; `idf.py build`.

### F3.4 `net.cpp` on `ident`, RSSI, second TLS profile
- **Read:** `DEVICE_PLAN.md` §3.2 step 3, §3.3, §5.4 (Signal); `net.cpp` 330–410, 540–556.
- **Files:** `firmware/main/net.cpp/h`.
- **Do:** replace the constexpr credentials with `ident_*()`; write the CA to slot 12 only when
  `ident`'s `ca_hash` differs from the stored one; `net_tls_profile_bootstrap()` configures
  profile 3 with `WALTER_MODEM_TLS_VALIDATION_NONE`; `net_get_rssi(int *dbm) -> bool` via the
  vendor signal-quality call (read `src/WalterModem.h`; name the call you used in a comment with
  its line number); `net_publish_raw(topic, bytes, len, qos)` for CBOR payloads (check the vendor
  publish path handles binary; if it is string-only, say so and use the JSON path — this is an
  `UNVERIFIED` in the plan).
- **Verify:** `idf.py build`.

### F3.5 `setup.c` — Setup mode
- **Read:** `DEVICE_PLAN.md` §3.1, §3.2 (device side), §3.7 (no SMS path — §3.6 is not built); keep the UI minimal here (a text
  prompt using the existing `ui.c` until F6 lands).
- **Files:** new `firmware/main/setup.c/h`; `firmware/main/main.c`; `firmware/host/test_setup.c`;
  `sdkconfig.defaults` (`CONFIG_MBEDTLS_HKDF_C=y` if not default); `firmware/main/CMakeLists.txt`
  (`REQUIRES … console`).
- **Do:** `setup_parse_code(str, out)` (Crockford + check char, host, port, apn);
  `setup_derive(token, bid, bpw, bkey)` (`mbedtls_hkdf`); `setup_run(code)`: attach → profile 3 →
  MQTT as `boot-{bid}` → subscribe `pager/boot/{bid}/down` → decrypt with `mbedtls_gcm` → decode
  CBOR bundle → validate → `ident_store` → CA to slot 12 → publish `{0:1,29:1}` → disconnect →
  `esp_restart()`. Console command `setup <code>` (esp_console over USB serial). Progress lines
  `SETUP network|broker|bundle|done` and the four error strings from §3.2. Host test: parse and
  derive against the vector; decrypt the vector bundle.
- **Verify:** `cd firmware/host && make test`; `idf.py build`; with the local stack up and a
  serial console, `setup <code>` reaches `SETUP done` (record the transcript in the PR).

### F3.6 Sign every publish, verify every `/down`, status fields, RTC bump
- **Read:** `DEVICE_PLAN.md` §2.5, §2.7, §5.4 (status); `msg.c` 488–534; `modes.c` 98–138,
  229–260; `net.cpp` on-message handler.
- **Files:** `firmware/main/msg.c`, `modes.c`, `net.cpp`.
- **Do:** all publishes build CBOR via `cbor.c` then `auth_sign`; `/status` adds `rssi`, `bv`
  (0 until F7.1), `n`, `sig`; the RX path calls `auth_verify` first when `ident` flag `req_sig`
  is set, then decodes CBOR (JSON path stays for `req_sig == 0` devices until F6.4 removes cJSON);
  `pager_rtc_t` embeds `auth_rtc_t`; bump `PAGER_RTC_MAGIC`; update the `_Static_assert` comment
  with the new size from the map file.
- **Verify:** `idf.py build`; against the local stack, the relay logs accept the device's signed
  `/status` and the device accepts a signed `/down` (serial transcript in the PR).

### T3.7 `tools/provision.py`
- **Read:** `DEVICE_PLAN.md` §3.7.
- **Files:** new `tools/provision.py`.
- **Do:** `--port` + `--code` types `setup <code>` over pyserial and streams the log until
  `SETUP done|error`; `--from-api --relay --admin-token --owner --default-to --label` calls the
  admin endpoint first. Exit non-zero on error.
- **Verify:** `python tools/provision.py --help`; a dry run with `--code` against a serial echo
  stub (document how).

### F6.1 `disp.c` / `gfx.c` split, Noto assets, host PNG test
- **Read:** `DEVICE_PLAN.md` §5.1, §5.2, §5.4; `ui.c` in full.
- **Files:** new `firmware/main/disp.c/h`, `gfx.c/h`; new `tools/mkassets.py`;
  `firmware/host/render_png.c`; `ui.c` shrinks to what F6.3 replaces.
- **Do:** `disp.c` = SSD1680 driver + BUSY + one mutex + partial/full refresh + cadence (moved,
  not rewritten). `gfx.c` = framebuffer, UTF-8 decode, glyph blit from the mmap'd `assets`
  partition (`esp_partition_mmap`; on the host, from the file), `text`, `text_width`,
  `text_wrap` (spaces for Latin, per-char for CJK), `hline`, `rect`, `invert_rect`, 12×12 icons.
  `mkassets.py` (FreeType via `freetype-py`): Noto Sans + Noto Sans CJK `{sc|tc|jp|kr}` at 12 and
  16 px, mono hinted, ranges per §5.2, output `assets.bin` with a header, a sorted codepoint table
  per size, glyph records `{w, adv, bearing, rows}`; prints the byte total. `render_png.c` renders
  a fixed set of strings (Latin, Cyrillic, a CJK sentence, a tofu case) to PNG.
- **Verify:** `python tools/mkassets.py --lang sc -o build/assets.bin` (default `sc`) under 1 MB;
  `cd firmware/host && make png` produces PNGs you look at; `idf.py build`.

### F6.2 `input.c` and the IME hook
- **Read:** `DEVICE_PLAN.md` §5.3; `modes.c` 510–615 (button FSM), `ui.c` 674–731 (CardKB).
- **Files:** new `firmware/main/input.c/h`, `ime.h`; `modes.c`.
- **Do:** key decode incl. `0xB4–0xB7`, `0x1B`, `0x09`; button FSM moved here with `BTN_STUCK`;
  one event queue; `input_awake()` window (30 s, compile-time); `ime_t` with the identity IME;
  `modes_run()` polls at 100 ms while awake, else the wake-and-drain cadence.
- **Verify:** `idf.py build`; host test for the decoder table.

### F6.3 Screen stack, status bar, Home / Chat / Device / Setup screens
- **Read:** `DEVICE_PLAN.md` §5.4, §5.5 (Home, Chat, Incoming, Device, Setup), §5.7;
  `firmware/README.md` R4, R5, R6, R9.
- **Files:** `firmware/main/ui.c/h` (rewritten), new `scr_home.c`, `scr_chat.c`, `scr_device.c`,
  `scr_setup.c`; `modes.c` (`render_pending`, render on the main task, `msg_mark_shown` after
  the render).
- **Do:** exactly the behaviours listed under each screen in §5.5; status bar buckets per §5.4
  with redraw only on bucket change; full refresh deferred to UI-sleep; the incoming-message
  rules (steal the screen only from asleep/Home). Host: `render_png.c` renders each screen with
  fixture data.
- **Verify:** `cd firmware/host && make png`; `idf.py build`; a serial transcript showing a
  message arriving, rendering on the main task, then `shown` publishing.

### F6.4 `msg.c`: ring 32, `to`, bodies to NVS, UTF-8 composer, cJSON out
- **Read:** `DEVICE_PLAN.md` §5.6, §5.2 (Input is Unicode); `msg.c` in full.
- **Files:** `firmware/main/msg.c/h`, `modes.c`, `firmware/main/CMakeLists.txt` (drop `json`).
- **Do:** as §5.6: `MSG_THREAD_DEPTH 32`; `to[17]`; namespace `msgq` for pending reply bodies and
  the unread mirror (full 320 B); RTC keeps `{id, state, attempts}` only; composer enforces 160
  code points **and** 320 bytes; accessors return copies under the lock; `msg_iter_peer()`. Remove
  cJSON; the JSON RX path goes with it (devices are CBOR from F3.6 on; `req_sig == 0` devices still
  decode CBOR).
- **Verify:** `idf.py build`; map file shows `sizeof(pager_rtc_t)` ≈ 480; host test for the
  composer caps with a 3-byte code point.

### F6.5 `lock.c` and the Locked screen
- **Read:** `DEVICE_PLAN.md` §5.8 in full, §5.5 (Locked mockup, Device screen bullets).
- **Files:** new `firmware/main/lock.c/h`, `scr_lock.c`; `modes.c` (RTC fields, restart → locked);
  `msg.c` (defer `shown` while locked); `scr_device.c`, `scr_home.c` (settings and *Lock now*).
- **Do:** PBKDF2 via `mbedtls_pkcs5_pbkdf2_hmac`, 10 000 iterations, 16-byte salt, NVS namespace
  `lock`; `lock_check_autolock(now)` called on every input event and UI wake; five attempts then
  30 s doubling to 10 min in RTC; masked passcode field bypassing the IME; the `cfg` `lock` map
  handler (`clear`, `auto`); **no `shown` for a body received while locked** — queue the ack at
  render time after unlock instead. Host test: hash round-trip, backoff schedule, PBKDF2 timing
  printed.
- **Verify:** `cd firmware/host && make test`; `idf.py build`; serial transcript: message arrives
  locked → relay shows it still `sent` → unlock → `shown` published.

### F7.1 `book.c` and `book`/`cfg` ingest
- **Read:** `DEVICE_PLAN.md` §4.2, §4.3, §5.8 (cfg).
- **Files:** new `firmware/main/book.c/h`; `msg.c` (kind dispatch), `modes.c` (`bv` in status).
- **Do:** namespace `book`: blob per §4.3 with nickname slots; `book_apply(obj)` full replacement,
  keeps nicknames by alias, sets `bv`, then acks `shown`; `book_request(name, ph_or_alias)` builds
  and queues a `contact_req`; `cfg` routed to `lock.c`. `/status` reports `bv`.
- **Verify:** `idf.py build`; against the local stack with S4.5's scenario driven by hand: the
  device's `bv` in `/status` follows the pushed book.

### F7.2 Pick, Address Book, Add, Nickname screens
- **Read:** `DEVICE_PLAN.md` §5.5 (New message → pick, Address book, Nicknames).
- **Files:** new `scr_pick.c`, `scr_book.c`.
- **Do:** exactly the mockups and key bindings; pending/rejected greyed and unselectable; Add form
  with the E.164 rule inline; Nickname field IME-enabled, ≤ 12 code points / 36 bytes.
- **Verify:** `cd firmware/host && make png` (fixtures for each screen); `idf.py build`.

### F7.3 Composer: `to` and `@nick`
- **Read:** `DEVICE_PLAN.md` §5.5 (Chat, Sending from a chat), Nicknames paragraph.
- **Files:** `scr_chat.c`, `msg.c`.
- **Do:** sending from a chat sets `to` to the peer alias, omitted for the default recipient;
  a leading `@word` resolves nickname → alias → picker error toast if unknown, and is stripped
  from the body.
- **Verify:** `idf.py build`; serial transcript of an `/up` with `to` accepted by the relay and
  routed (relay log line).

### F8.1 Bring-up checklist additions
- **Files:** `firmware/README.md`.
- **Do:** add to the measurement tables: M13 arrow codes and key-hold; M9 on Noto 12/16 px incl.
  CJK; battery thresholds; RSSI mapping; UI-awake current; PBKDF2 timing; NVS blob write time;
  `assets` mmap cost; and the `UNVERIFIED` rows of `DEVICE_PLAN.md` §10 that are hardware-only.
- **Verify:** docs only.

---

## Dependency graph (what may run in parallel)

```
D0.1 ──┬── S1.1 ── S1.2 ── S1.3 ── S1.4 ── T1.5 ── S2.1 ── S2.2 ── S2b.1 ── S2b.2 ── S2b.3 ── T2b.4 ── S2b.5
       │                                   │                                   └── W2.3
       │                                   └── S4.1 ── S4.2 ── S4.5 ── W4.4
       └── F3.1 ── F3.2 ── F3.3 ── F3.4 ── F3.5 ── F3.6 ── T3.7 ── F6.1 ── F6.2 ── F6.3 ── F6.4 ── F6.5 ── F7.1 ── F7.2 ── F7.3 ── F8.1
D0.2 (any time after D0.1)          F3.5 needs S2b.1's vector; F3.6's live check needs S1.3; F7.1's needs S4.2.
D8.1 last.
```

## Definition of done for the whole plan

- `ci.yml` green with `tools/e2e_v2.py` running `setup_code` and `address_book` in addition to
  the existing scenarios, in both wire encodings.
- `idf.py build` clean; `firmware/host` tests and PNG renders committed as artefacts in the PR.
- `docs/PROTOCOL.md` §14 exists and every `DEVICE_PLAN.md` §8 item is reflected.
- Every `UNVERIFIED` row in `DEVICE_PLAN.md` §10 that can be settled without hardware is settled,
  with the answer written next to it.
