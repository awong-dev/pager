# Soracom bearer: execution tasks (8 Oct 2026)

Companion to `SORACOM_DESIGN.md` (section numbers below refer to it). Each task is self-contained
for an agent that has not read the design conversation: **Read / Files / Do / Verify**. Tracks run
in parallel; arrows are hard dependencies. Headless: no owner at the desk, no questions; when a
choice is open, take the design's option and note it in the commit message.

Shared rules (from the project memory): one serial reader at a time, never reopen the USB port
within a few seconds, caps on every wait (boot lines by ~20 s, console answers in 2-3 s), never poll
a port that has vanished (a sleeping release build), build with
`export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.2_py3.11_env; source ~/src/esp/esp-idf/export.sh`,
write logs under the session scratchpad, and verify the exact binary you flash. Bench unit:
Walter MAC 20:6e:f1:dc:cc:6c on `/dev/cu.usbmodem101`, flash fully erased on 8 Oct 14:42 PDT.

Production admin actions (create/revoke/delete a device, read a setup code) are done by the
orchestrator in the owner's signed-in browser session on `https://kid-pager.web.app` (Family →
Devices), not by minting tokens from scripts. Firestore leftovers are removed with
`tools/prod_device_purge.py`, which uses Application Default Credentials the same way
`tools/set_web_password.py` does.

```
P0 purge proto3 ─────────────────────────────┐
A identity (bench) ─► F1 carrier preset ─► F2 PDP auth ─► F3 beam session ─► F4 status/console ─► F5 host tests + builds ─► B/C/D bench ─► E beam hop (needs owner's Soracom step)
R1 relay status fields ─► R2 web ─► R3 PROTOCOL.md (server-architect)
T1 soracom_beam.py tool ─► (owner runs it or configures the console) ─► E
D1 docs sweep (after F5/R3)
```

## Track P: production clean-up

### P0 — Purge `proto3` from production
- **Read:** `relay/app/routers/admin.py` 700-830 (delete removes only `devices/{id}` and the
  owner's pager backend; revoke removes the EMQX user+ACL), the store modules named in
  `tools/prod_device_purge.py`'s docstring.
- **Do:** in the web UI: Revoke `proto3`, then Delete. Then
  `relay/.venv/bin/python tools/prod_device_purge.py proto3 --dry-run`, review, run with `--yes`.
- **Verify:** Devices page lists only `proto2`; a second dry run reports zeros.

## Track A/B/C/D/E: bench (one bench-tester at a time)

### A — SIM identity on the erased unit (no code change)
- **Read:** `docs/HARDWARE_TESTING.md` build/flash sections, `firmware/build/flash_args`,
  `tools/mkassets.py` (assets image for 0x11000), `firmware/main/carrier.c`, `main.c` Setup
  console (`carrier`, `at`).
- **Do:** check `ps`/`lsof` for stale serial users; debug build from `main`
  (`PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build`); full flash: bootloader 0x0,
  partition table 0x8000, otadata 0xF000, assets 0x11000 (build with mkassets.py), app 0x120000;
  one capture with scheduled console commands: `carrier`, `at +CIMI`, `at +SQNCCID`,
  `at +CRSM=176,28478,0,0,0`, `at +COPS?`, `at +CEREG?` (the `at` reply appears in the AT trace
  lines). Cap: 40 s after boot.
- **Verify/report:** IMSI (full), ICCID, GID1 raw, what `carrier` auto-detected (expected: nothing
  yet), registration state. These numbers feed F1.

### B — Attach + provision on the Soracom SIM, direct bearer (after F5)
- **Do:** flash the F5 debug image fully (as in A). Console: `carrier` must print `Soracom`
  (auto) with bearer `beam`; set `bearer direct`. The orchestrator creates device `sora1`
  ("Soracom Walter", family `default`) on the web Devices page and hands the setup code to this
  task; `setup <code>` on the same capture. Expect `pdp auth: PAP user=sora`, registration,
  `SETUP done`, reboot, `MQTT session usable`. Then `at +CEDRXRDP` and `at +CSQ`. Cap: 300 s
  for the attach (Soracom roaming registration is the unknown), 60 s after reboot.
- **Verify:** the Devices page shows `sora1` online with carrier `Soracom`, CA trust
  `unpinned`/`pinned` (direct). Report attach time, eDRX granted or not, RSSI.

### C — Messaging, direct bearer
- **Do:** the orchestrator sends a page to `sora1` from the web chat; expect the serial line for a
  shown page within the eDRX cycle (cap 60 s); reply from the console (`key` injection, see
  memory `bench-console-key-injection`) and the orchestrator confirms it in the web chat. Two
  round trips.
- **Verify:** both directions logged with timestamps; report latency.

### D — Release build, direct bearer (relay-side observation only)
- **Do:** flash the F5 release image (app + otadata); confirm the boot banner once, then **close
  the port and do not reopen it**. For 60 min watch production only (Devices page status, battery
  card), one page sent at +10 min and one at +40 min, delivery observed in the web chat (`shown`).
- **Verify:** both pages reach `shown`; no reboot counters climbing. Report.

### E — Beam hop (only when `tools/soracom_beam.py` has run or the owner configured the group)
- **Do:** debug image, `bearer auto`; boot; expect `mqtt: beam.soracom.io:1883 plain` in the log
  and `MQTT session usable`; Devices page CA trust reads "via Beam". Repeat C. Then erase NVS
  0x9000 and re-provision through Beam to prove the bootstrap path. If the connect is refused,
  run `mqtttest beam.soracom.io 1883 plain` and report the modem's return code; the most likely
  cause is Beam not configured for this SIM's group.

## Track F: firmware (firmware-dev)

### F1 — Carrier preset: bearer, PDP auth, SMS flag, Soracom entry
- **Read:** `SORACOM_DESIGN.md` §3.1 and §2 (APN and credentials); `firmware/main/carrier.[ch]`,
  `net.cpp:558-629` (`read_sim_identity`, `effective_apn`), host tests for carrier
  (`grep -l carrier firmware/host/*`).
- **Files:** `carrier.h`, `carrier.c`, `net.cpp`, host tests.
- **Do:** extend `carrier_preset_t` (`bearer`, `auth_proto`, `auth_user`, `auth_pass`, `sms_mo`);
  add `carrier_effective()`; add the `Soracom` preset with the PLMN prefix from bench step A
  (if A has not reported yet, use `29505` and mark `// VERIFY on bench`, then fix it when A
  reports); NVS `bearer` override (`carrier_bearer_override_get/set`); `effective_apn()` becomes a
  thin wrapper over `carrier_effective()`.
- **Verify:** `make -C firmware/host test` passes with new cases: Soracom IMSI → Soracom/beam/PAP;
  US Mobile IMSI+GID → unchanged; typed custom APN → direct/none; override wins.

### F2 — PDP authentication
- **Read:** §3.2; `WalterModem.h` around `setPDPAuthParams` (6065) and `definePDPContext` (6034);
  `net.cpp:700-730` and `:1000-1015`; the architect's notes in §3 if present.
- **Do:** after each `definePDPContext`, when the effective preset has credentials call
  `setPDPAuthParams(proto, user, pass, PAGER_PDP_CTX_ID)`; log once. Check whether the library
  requires the auth call before or after context definition (read the AT it emits) and order
  accordingly.
- **Verify:** release and debug builds compile; AT trace in bench B shows the auth command before
  activation.

### F3 — Beam session and bootstrap path
- **Read:** §3.3; `net.cpp:827-974` (`configure_session`, `net_tls_profile_bootstrap`),
  `net.cpp:1064-1089` (`net_bootstrap_connect`), `xport_lte.cpp:590-620`, `setup.c:656-857`,
  `catrust.c`, `cafetch.c`, `docs/GOTCHAS.md:73-78`.
- **Files:** `net.cpp`, `net.h`, `xport_lte.cpp`, `setup.c`, `catrust.c`.
- **Do:** `PAGER_BEAM_HOST`/`PAGER_BEAM_PORT` constants; a `net_bearer_t net_bearer(void)`
  (direct/beam) from `carrier_effective()` + override; when beam: skip CA slot/profile work,
  `mqttConfig(dev_id, dev_id, pw, 0)`, connect to Beam host/port; bootstrap connects to Beam with
  no TLS profile; `cafetch_run_blocking` skipped (store `ca_hash` empty, log why); catrust state
  reports `proxy`. Direct path byte-for-byte unchanged (diff it).
- **Verify:** builds; `grep` shows the direct path still names slot 12; bench E.

### F4 — Reporting and console
- **Read:** §3.4-3.5; `/status` encoder (grep `52` / `xport` in `modes.c` or the status
  builder), `main.c` console tables (Setup console 903-998, debug console 2041-2370),
  `net.cpp:2204-2280` (`mqtttest`), `scr_device.c:489` (Carrier row), boot splash status lines.
- **Do:** `/status` key 70 `car` (label, ≤24 B) and `tls: "proxy"`; `bearer [auto|direct|beam]`
  command in both consoles; `carrier` prints bearer/auth; `mqtttest ... plain`; splash "via Beam";
  skip modem SMS send attempts when `sms_mo` is false (log once).
- **Verify:** builds; host test for the status encoder if one exists; bench B shows `car`.

### F5 — Builds and images
- **Do:** `idf.py build` (release) and `PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build`;
  copy to `build/images/soracom-{release,debug}-app.bin` with sha256 in a sidecar; `make -C
  firmware/host test`.
- **Verify:** both binaries exist, release < 1 MB; report sizes and hashes.

## Track R: relay + web

### R1 — Relay accepts and stores `car` and `tls: proxy`
- **Read:** §4; `relay/app/wire.py:250-320`, `relay/app/store/devices.py:78-148`
  (`DeviceStatus`), `relay/app/wirecbor.py` keymap, tests `relay/tests/test_ingest*.py` for
  status parsing, `relay/app/apn_presets.py`.
- **Do:** key 70 `car` (str ≤ 24) → `status.car`; `tls` literal gains `proxy`; `apn_presets` adds
  `Soracom` / `soracom.io`; tests for both.
- **Verify:** `cd relay && .venv/bin/pytest -q` (emulators: `docker compose up -d firebase`);
  `ruff check`.

### R2 — Web shows the carrier and the proxy trust state
- **Read:** §4; `web/app/family/devices/page.tsx:276-286`, `web/app/admin/devices/page.tsx:322-330`,
  `web/app/devices/[id]/DevicePageClient.tsx:72` (xport chip), the SMS panel component, the
  device type definitions under `web/lib`.
- **Do:** Carrier chip from `status.car`; CA trust renders `proxy` as "via Beam"; SMS panel shows
  a one-line note "Device SMS unavailable on this carrier; relay SMS still works" when
  `tls === "proxy"`.
- **Verify:** `npm run lint && npx tsc --noEmit && npm run build` in `web/`.

### R3 — PROTOCOL.md (server-architect)
- **Do:** §5.1 rows for `car` and `tls: proxy`; §6.1 bearer row (Beam plain MQTT 1883, pass-through
  credentials, keepalive 480 inside Beam's 5…1200, 300 s host liveness inside 1.5 × keepalive);
  §10 key 70; one paragraph in §7 traffic noting the handshake saving on the Beam bearer.
- **Verify:** key 70 unused elsewhere (`grep -n '^| 70 ' docs/PROTOCOL.md`).

## Track T: tools and docs

### T1 — `tools/soracom_beam.py`
- **Read:** §5; Soracom API (Global coverage, base `https://g.api.soracom.io/v1`): `POST /auth`
  with an auth key id and secret returns an API key and token sent on later calls as
  `X-Soracom-API-Key` / `X-Soracom-Token`; `GET /groups`, `POST /groups {tags:{name}}`,
  `PUT /groups/{groupId}/configuration/SoracomBeam` with a list of
  `{key: "mqtt://beam.soracom.io:1883", value: {name, destination, enabled: true, version:
  "201912", useClientCredentials: true, useClientCert: false, addSubscriberHeader: false}}`,
  `GET /sims?limit=100` (match by `imsi` or `iccid`), `POST /sims/{simId}/set_group {groupId}`
  (check the exact path in the API reference).
- **Do:** `tools/soracom_beam.py --imsi <imsi> [--group pager-beam] [--destination
  mqtts://s1289801.ala.us-east-1.emqxsl.com:8883] [--dry-run]`; the auth key id and secret are
  read from the environment (`SORACOM_AUTH_KEY_ID`, `SORACOM_AUTH_KEY`), never stored or printed;
  idempotent; prints the resulting group config. Use `httpx` from the relay venv.
- **Verify:** `--dry-run` prints the exact requests with the secret redacted; unit-test the
  request builder with the relay pytest if cheap. It cannot be run live without the owner's key:
  say so. (Ran live 8 Oct 2026 with the key from `~/soracom.key`; field names accepted.)

### D1 — Docs sweep (docs-writer, after F5 and R3)
- **Do:** `docs/README.md` row for `SORACOM_DESIGN.md`/`SORACOM_TASKS.md`; `SORACOM_EVAL.md` status
  line pointing to the design; `docs/HARDWARE_TESTING.md` console table (`bearer`, `mqtttest
  plain`); `firmware/README.md` carrier section; `docs/GOTCHAS.md` entry "Beam bearer: profile 0 is
  deliberate plaintext"; ROADMAP: AEAD bodies now a precondition item under Soracom;
  `.overnight-handoff.md` top section rewritten with the state and the owner's Soracom step.
