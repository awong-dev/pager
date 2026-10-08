# Device config: disable GNSS (`cfg.loc.gnss`)

Owner request, 8 Oct 2026: a per-device setting, pushed from the web app like the WiFi enable
flag, that stops the pager from ever powering the GNSS receiver. First use: a module whose GNSS
antenna connection is damaged. Location keeps working from the serving cell.

## Decisions

| | Decision |
|---|---|
| D1 Wire | A sixth `cfg` sub-map, `loc` (CFG key **5**), map `{gnss}`; `gnss` is CBOR key **0**, bool. JSON `"cfg":{"loc":{"gnss":false}}`. `gnss` absent = no change. Unknown sub-keys ignored. Applied and acked `shown` immediately, like `lock`/`wifi`. Newest-only and re-published on an online edge like every `cfg`. |
| D2 Persistence | Device: NVS namespace `loc`, key `gnss` (u8); absent = 1 (enabled). RAM-cached, loaded in `loc_init()`. Relay: `gnss_enabled: bool` on the device document (default `True`, not a secret, admin-SDK writes only, no rules change). |
| D3 Effect | When disabled, `loc.c` never calls any `net_gnss_*()` function. Every attempt (on-demand `loc_req`, `gnsstest`, scheduled while-moving) short-circuits at the **top** of `LOC_PH_ASSIST_CHECK`, before `net_gnss_assistance_due()`, with the existing "any GNSS failure sends a cell report and stops" path (`finish_attempt(false)`). The scheduled GNSS-while-moving timer never fires while disabled (`loc_track_gnss_due()` is not consulted, same as `move_gnss_s == 0`), so no attempt is even started for it. `loc_move_s` in `/status` is left as configured. |
| D4 Status | `/status` gains `gnss` (CBOR key **69**, uint `0`/`1`; JSON `gnss`), always present once this firmware runs. Absent = firmware predating the field. Display only. |
| D5 Relay API | `GET`/`PUT /api/devices/{id}/gnss`, body/response `{en: bool, pending: bool, reported: int|null}` (`reported` = last `/status` `gnss`). Owner or admin, same `_require_owner_or_admin` gate as WiFi. `PUT` stores `gnss_enabled`, then `devcfg.push_loc(device_id, gnss=en, broker=…)` under its own pending slot `pendingCfgLoc`. |
| D6 Web | A `GnssPanel` beside `WifiPanel` on the device page: one switch "GPS (GNSS)", pending chip while unacked, and the device's last reported value. |
| D7 Console | Debug build: `gnss [on|off]` prints or persists the flag (NVS), same style as `loctrack`. (The `modem-bench` branch has an unrelated `gnss` command; it is not on main.) |

## Tasks

### F — firmware (`firmware-dev`)
Read: this file; `firmware/main/cfg.h`/`cfg.c` (the `wifi`=3 span capture + dispatch); `firmware/main/wificred.c` `wificred_apply_cfg_submap()` and `wificred_set_enabled()` (parse, NVS, `msg_mark_shown(id)` ack); `firmware/main/loc.c` lines 1240-1600 (phase machine, `begin_attempt`, `loc_service` IDLE case) and 860-930 (`s_track_enabled`/`s_move_gnss_s` accessors); `firmware/main/modes.c` lines 640-680 and 905-920 (`STK_*` and the `/status` writer); `firmware/main/main.c` `cmd_loctrack` and its registration; `firmware/host/test_cfg.c`, `firmware/host/test_loc.c`.
Files: `cfg.h`, `cfg.c`, `loc.h`, `loc.c`, `modes.c`, `main.c`, `firmware/host/test_cfg.c`, `firmware/host/test_loc.c`.
Do:
1. `cfg.c`: capture `loc`=5 into `have_loc`/`loc_off`/`loc_len`; dispatch to `loc_apply_cfg_submap(buf+off, len, d.id)`.
2. `loc.h`/`loc.c`: pure `bool loc_parse_cfg_submap(const uint8_t*, uint16_t, loc_cfg_t *out)` (`have_gnss`, `gnss`); device `void loc_apply_cfg_submap(...)` (parse, `loc_set_gnss_enabled()`, `msg_mark_shown(id)` on success exactly like wificred); `bool loc_gnss_enabled(void)`; `bool loc_set_gnss_enabled(bool)` (NVS `loc`/`gnss` u8 + RAM); load in `loc_init()` (absent = enabled).
3. `loc.c` phase machine: top of `LOC_PH_ASSIST_CHECK`: `if (!loc_gnss_enabled()) { ESP_LOGI(... "gnss disabled by cfg; sending a cell report and stopping"); finish_attempt(false); break; }`. In `loc_service()`'s IDLE case treat `move_s` as 0 when disabled so the scheduled attempt never starts.
4. `modes.c`: `#define STK_GNSS 69`, write `cbor_w_uint(&w, STK_GNSS, loc_gnss_enabled() ? 1 : 0)` next to `STK_LOC_MOVE_S`.
5. `main.c`: debug console `gnss [on|off]` (see `cmd_loctrack`), registered where `loctrack` is.
6. Host tests: `test_cfg.c` — a `cfg` carrying `loc=5` sets `have_loc` with the right span, and still parses alongside `wifi`; `test_loc.c` — `loc_parse_cfg_submap` accepts `{0:false}`, `{0:true}`, `{}` (have_gnss false), ignores unknown keys, rejects a non-map.
Verify: `make -C firmware/host test` passes; `cd firmware && idf.py build` passes (normal build, no env flags). Report the exact lines added to the `/status` writer and the phase short-circuit.

### R — relay (`backend-dev`)
Read: this file; `relay/app/wirecbor.py` (`CFG_KEYMAP`, the `wifi` encode/decode branches ~280-350, the status keymap ~140-160); `relay/app/wire.py` ~300-360 (`Status` model); `relay/app/devcfg.py` 150-170 and `push_wifi`/`wifi_pending` (~695-750); `relay/app/routers/devices.py` 140-240; `relay/app/store/devices.py` (device document fields); `relay/tests/test_wifi.py`, `relay/tests/test_devcfg.py`.
Files: `wirecbor.py`, `wire.py`, `devcfg.py`, `routers/devices.py`, `store/devices.py`, new `relay/tests/test_gnss.py`.
Do:
1. `wirecbor.py`: `CFG_KEYMAP["loc"] = 5`; encode `cfg.loc {gnss: bool}` → `{5: {0: bool}}` and decode back; status key `69 → "gnss"`.
2. `wire.py`: `Status.gnss: int | None = None`, validated `0|1`.
3. `store/devices.py`: `gnss_enabled` on the device document, getter/setter, default `True` when absent.
4. `devcfg.py`: `_PENDING_CFG_LOC_FIELD = "pendingCfgLoc"` added to `_ALL_PENDING_FIELDS`; `push_loc(device_id, *, gnss: bool, broker) -> bool` mirroring `push_wifi`; `loc_pending(device_id)`.
5. `routers/devices.py`: `GET`/`PUT /{device_id}/gnss` per D5; `reported` from `device.status.gnss`.
6. Tests: encode/decode round trip for `cfg.loc` and status `gnss`; `PUT` stores, publishes one `/down cfg` with `{"loc":{"gnss":false}}`, and `pending` flips true then false after the `shown` ack (copy `test_wifi.py`'s fixtures).
Verify: `cd relay && .venv/bin/python -m pytest -q` passes. Report the route signatures and the wire shapes.

### W — web (`web-dev`)
Read: this file; `web/components/WifiPanel.tsx`; `web/lib/wifi.ts`; `web/lib/types.ts` (the status type with `loc_move_s`); `web/app/devices/[id]/DevicePageClient.tsx` lines 60-70 and 300-312.
Files: new `web/lib/gnss.ts`, new `web/components/GnssPanel.tsx`, `web/lib/types.ts`, `DevicePageClient.tsx`.
Do: API client for `GET`/`PUT /api/devices/{id}/gnss` (`{en, pending, reported}`); `GnssPanel` with a switch labelled "GPS (GNSS)", helper text "Off: location comes from the cell network only. Use when the GPS antenna is damaged or unwanted.", a pending chip while `pending`, and "Device reports: on/off/unknown" from `reported`; mount it directly under `WifiPanel`; `gnss?: number` on the status type.
Verify: `cd web && npm run lint && npx tsc --noEmit && npm run build` pass. Report the files added.

### P — protocol doc (`server-architect`)
Read: this file; `docs/PROTOCOL.md` lines 131-145 (field table), 185-190 (examples), 249-269 (`kind:"cfg"`), 700-730 (`/status` fields), 1540-1552 and 1570-1592 (CBOR keys).
Files: `docs/PROTOCOL.md` only.
Do: add `loc` to the `cfg` field row and the `kind:"cfg"` paragraph (D1); add `gnss` to the `/status` field table (D4); add CFG `loc=5` and the `loc` map `gnss=0` to §10; add key `| 69 | gnss | uint 0/1 | /status (...) |`; one example row for `{"cfg":{"loc":{"gnss":false}}}`. Keep every existing allocation line unchanged; follow the `ota=4` "listed apart" style.
Verify: `grep -n "| 69 |" docs/PROTOCOL.md` shows exactly one row; no other key number changed.

## Implementation notes (8 Oct 2026)

- Relay `app/ingest.py` also changed (one line): `handle_status` passes `gnss` through to `update_status`, otherwise `reported` could never be populated.
- A `/status` `gnss` outside `0|1` is dropped to `None` by the relay (same as `bpull`), not rejected as a malformed envelope.
- Firmware `/status` writer: the declared map pair count went up by one for the new field; the 448-byte buffer still fits (399 B maximum before, about 3 B more now).
- A device that never received `cfg.loc` behaves as `gnss:true`. The flag survives reboots (NVS `loc/gnss`) and is reported in every `/status`.
- Verified: `make -C firmware/host test`, release and debug `idf.py build`, relay `pytest` (1104 passed), web lint/tsc/build. Not bench-tested on hardware.
