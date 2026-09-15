# DEVICE_TASKS_LOG.md — execution log for docs/DEVICE_TASKS.md

One line per task: status (done / built-unverified / blocked), commit hash, and any
`DEVICE_PLAN.md` §10 UNVERIFIED item the task settled (with the answer).

| Task | Status | Commit | Notes |
|------|--------|--------|-------|
| D0.1 | done | 90f7b7b | PROTOCOL.md §8 items 0–14 applied; §14 Device authentication added. |
| D0.2 | done | ec2212e | SERVER_PLAN.md: deviceSecrets/setupCodes/contactRequests, devices fields, S2.2/S2b.1/S4.1/S4.2 endpoints. |
| S1.1 | done | a5dbd34 | deviceSecrets store + 64-wide replay window transactions; mqttPasswordHash migration off devices.py deferred to S2.2 per task's own Files list (correct per dependency graph). Window arithmetic: implemented full 64-wide accept (0 < upN−n ≤ 64) reconciling §2.5's stated "64 wide" with its literal inequality. |
| F3.1 | done | 721b266 | partitions.csv (nvs/otadata/assets/ota_0/ota_1) on 16 MB flash per DEVICE_PLAN.md's Walter hardware note; ident.c/h; main.c wired for nvs_flash_init()+ident_load() (touched though not in Files list — required by the task's own Do/Verify text, flagged). idf.py build green via espressif/idf:release-v5.2 Docker image. Boot-log format built-unverified: no hardware attached to check the actual serial line. IDENT_FLAG_REQ_SIG bit position inferred (not specified in plan) from bundle example. |
| S1.2 | done | 8298485 | devauth.py + wirecbor.py + 8 tools/authvectors.json vectors. **Flag for human review:** PROTOCOL.md §10 never assigns a numeric CBOR key to the `lock` field nested inside `cfg` (only `cfg`'s own inner fields `clear=0,auto=1` are keyed); agent filled the gap with `CFG_KEYMAP={"lock":0}`, documented in wirecbor.py's docstring, pending an explicit PROTOCOL.md allocation. |
| F3.2 | done | dae9fc5 | cbor.c/h encoder/decoder, no malloc, rejects indefinite lengths; firmware/host/test_cbor.c round-trips all 8 authvectors byte-identically. Added `cbor_w_map_key` beyond literal spec text for keyed nested maps (cfg/lock), documented in cbor.h. `make test` and `idf.py build` both green. |
| S1.3 | done | f5f608d | ingest verify-before-parse pipeline, authMode/wire fields, replay window, unsigned-LWT exception, sigFailures 10-min rolling window on devices/{d} (device_secrets.py's own counter is lifetime, left untouched). **UNVERIFIED settled:** PROTOCOL.md §14.6 parenthetical says unsigned LWT must omit `session`, but §5.2's own LWT example and DEVICE_TASKS.md's S1.3 brief include it — implemented per §5.2/task brief (session present), flagged in wire.py docstring; PROTOCOL.md §14.6 wording should be fixed. **TODO(orchestrator):** clear_auth_alarm/invalidate_secret_cache exist but nothing calls them yet — wire them up when a rotation endpoint (S2.2) lands. |
