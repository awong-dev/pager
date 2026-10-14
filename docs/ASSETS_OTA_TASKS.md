# Font (assets) OTA — tasks

Companion to `docs/ASSETS_OTA_DESIGN.md`. The design doc holds the reasoning and decisions. This file holds
only what an implementing agent needs: for each task, the files to read first, the files it may touch,
what to do, and how to verify.

**Bench rules.** Bench tasks follow `docs/HARDWARE_TESTING.md` and the bench rules in
`.overnight-handoff.md` exactly: one reader on the port, one hardware agent at a time, and bounded
foreground captures. **Coding agents never open `/dev/cu.usbmodem*`.**

**Commands.**
- Firmware build (debug):
  `export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.2_py3.11_env; . ~/src/esp/esp-idf/export.sh; cd firmware; PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build`.
  Write build logs to the scratchpad, not `/tmp`.
- Host tests: `make -C firmware/host test`.
- Relay: `cd relay && .venv/bin/pytest`.
- Tools: `relay/.venv/bin/python -m pytest tools/test_fwpub.py` (this venv has detools).
- Web: `cd web && npm run lint && npm run build`.

Every task must leave all of these green.

**Order:** A0 → A1 → A2 → A3 (relay must accept the new fields before any firmware sends them) → A4 → A5 → A6
→ A7 → A8 (bench migration) → A9 (bench feature).

**Hard rules for every task.**
- Do not change any existing row's offset, or any size other than msghist's.
- Do not change `sizeof(ota_job_t)` or `sizeof(ota_cfg_t)`. `job_load()` drops a blob whose size differs,
  and that would lose an in-flight app job across the upgrade.
- Every power-relevant line keeps a `Power effect:` comment, as in `ota.c`.

---

## A0. Partition table, msghist comments, mount log — firmware-dev, timebox 30 min

**Read:** `firmware/partitions.csv`; `docs/ASSETS_OTA_DESIGN.md` §1; `firmware/main/msg.c:625-680`.

**Files:** `firmware/partitions.csv`, `firmware/main/msg.c` (comments and one log line only),
`docs/HARDWARE_TESTING.md`.

**Do:**
1. In `firmware/partitions.csv`:
   - On the `assets` row, set Flags to `readonly`. Nothing else on that row changes.
   - On the `msghist` row, change the size to `0x100000`.
   - Append a last row: `assets_ota, data, undefined, , 0x200000,`.
   - Update the comments: msghist is 1 MiB (owner, 14 Oct 2026); `assets_ota` is the font OTA target
     (design §1); appending never moves an earlier row.
2. In `msg.c`, change the "128 KB" comments to 1 MiB. In `history_mount()`, log one INFO line after a
   successful mount:
   `msghist mounted in <ms> ms, internal free <a>-><b>, psram free <c>-><d>`.
   Use `esp_timer_get_time()` and `heap_caps_get_free_size(MALLOC_CAP_INTERNAL / MALLOC_CAP_SPIRAM)`,
   sampled before and after `nvs_flash_init_partition`.
3. In `docs/HARDWARE_TESTING.md`, next to the `0x11000 assets.bin` recipe, add one line: font OTA images
   are never USB-flashed to `assets`; see A8 for the one-time table migration.

**Verify:** `python3 ~/src/esp/esp-idf/components/partition_table/gen_esp32part.py firmware/partitions.csv out.bin && python3 …/gen_esp32part.py out.bin` prints exactly:
`nvs 0x9000`, `otadata 0xf000`, `assets 0x11000 1M readonly`, `ota_0 0x120000`, `ota_1 0x320000`,
`msghist 0x520000 1M`, `assets_ota 0x620000 2M`. The firmware build and host tests pass.

## A1. Commit record module (C) and `tools/astpart.py` — firmware-dev, timebox 1.5 h

**Read:** design §2 (the commit record); `firmware/host/Makefile` (how `test_ota` is wired).

**Files:** new `firmware/main/astpart.{c,h}`, `firmware/main/CMakeLists.txt` (add the source), new
`firmware/host/test_astpart.c`, `firmware/host/Makefile`, new `tools/astpart.py`, new `tools/test_astpart.py`.

**Do:**
1. Create `astpart.h` with exactly this API. The pure part builds on the host; the device part goes under
   `#ifdef ESP_PLATFORM`.
   ```c
   #define ASTPART_LABEL "assets_ota"
   #define ASTPART_SECTOR 4096u
   #define ASTPART_REC_SIZE 64u
   typedef struct { uint32_t len; uint8_t sha[32]; uint8_t fv; } astpart_rec_t;
   uint32_t astpart_crc32(const uint8_t *p, size_t n);        /* == zlib.crc32(p) (init 0) */
   void astpart_rec_encode(const astpart_rec_t *r, uint8_t out[ASTPART_REC_SIZE]);
   bool astpart_rec_decode(const uint8_t in[ASTPART_REC_SIZE], uint32_t part_size, astpart_rec_t *out);
   #ifdef ESP_PLATFORM
   const esp_partition_t *astpart_find(void);                 /* by label; NULL = old table */
   bool astpart_read_rec(const esp_partition_t *p, astpart_rec_t *out);   /* 64 B read at size-4096 */
   esp_err_t astpart_invalidate(const esp_partition_t *p);    /* erase the last 4 KiB sector */
   esp_err_t astpart_write_rec(const esp_partition_t *p, const astpart_rec_t *r);
   #endif
   ```
2. Record layout (little-endian):

   | Offset | Field |
   |---|---|
   | 0 | `"PGAC"` |
   | 4 | u32 1 |
   | 8 | u32 len |
   | 12 | sha[32] |
   | 44 | u8 fv |
   | 45..59 | 0 |
   | 60 | u32 crc32 of bytes 0..59 |

   `decode` returns true only if all of these hold: magic matches, version is 1, crc matches, and
   `16 <= len <= part_size - 4096`. Write a table-free bitwise crc32 (reflected, poly 0xEDB88320, init and
   xorout 0xFFFFFFFF). Do not use the ROM crc.
3. Create `tools/astpart.py build <assets.bin> -o <part.bin> [--size 0x200000] [--fv N]
   [--set-version N] [--bad-crc] [--corrupt-magic]`. It writes a full-size partition image: the
   assets file (with the header version byte at offset 4 patched if `--set-version` is given), 0xFF
   padding, and the 64 B record in the last sector (the rest of that sector 0xFF). `fv` defaults to
   the header version.

**Verify:**
- `make -C firmware/host test` runs `test_astpart`, which checks:
  - crc32 of "123456789" == 0xCBF43926;
  - encode/decode round trip;
  - each single-byte flip in bytes 0..63 makes decode fail;
  - `len` 15 and `len` = size-4095 fail.
- `tools/test_astpart.py` builds a part from `build/images/assets-dejavu-emoji.bin` and checks:
  - the record decodes in Python;
  - `zlib.crc32` matches the stored crc;
  - the sha equals `hashlib.sha256(file)`.

## A2. gfx: version range, pure check, boot selection, live switch — firmware-dev, timebox 2 h

**Read:** `firmware/main/gfx.c:140-260`, `gfx.h` (asset loading section), design §2 (boot selection,
format version), `firmware/main/ui.c:1160-1175`.

**Files:** `firmware/main/gfx.{c,h}`, new `firmware/host/test_gfx_assets.c`, `firmware/host/Makefile`.

**Do:**
1. Add `#define GFX_ASSETS_VER_MAX 1`. Change `parse_assets()`:
   - accept `1 <= data[4] <= GFX_ASSETS_VER_MAX`;
   - fill a caller-supplied blocks array instead of globals.
2. Add a pure function, `bool gfx_assets_check(const uint8_t *d, size_t len, uint8_t *ver_out)`. It is
   true only if the data parses and the header `total == len`.
3. Device API:
   ```c
   typedef enum { GFX_ASSETS_NONE, GFX_ASSETS_FACTORY, GFX_ASSETS_OTA } gfx_assets_src_t;
   bool gfx_select(gfx_assets_src_t want); /* OTA: valid record + check, else factory */
   gfx_assets_src_t gfx_assets_source(void);
   bool gfx_assets_ota_present(void);      /* the assets_ota row exists */
   const char *gfx_assets_ota_id16(void);  /* 16 hex of record sha when source==OTA, else "" */
   ```
   - Keep the `esp_partition_mmap_handle_t` of the current mapping.
   - `gfx_select` maps the new source and parses it. For OTA, map only `[0, rec.len)` and also require
     `ver == rec.fv`. On success it swaps the globals and then `esp_partition_munmap`s the old handle.
     On failure it keeps or falls back to factory.
   - `gfx_init()` = `gfx_select(GFX_ASSETS_OTA)`. Log one INFO line naming the source, id16, or the
     fallback reason (`no row` / `no record` / `bad record` / `parse` / `version`).
   - Comment that `gfx_select` must be called only on the `modes_run` task (it is the renderer).
4. The host build (`gfx_init_from_file`) behaves exactly as before.

**Verify:**
- `test_gfx_assets` checks:
  - `assets-dejavu-emoji.bin` → true, version 1;
  - the same bytes with `[4]=2` → false;
  - `len-1` → false;
  - a truncated header → false.

  If CI cannot see `build/images`, use a tiny image built in the test.
- `render_png` output is byte-identical to before (run it and `cmp` its PNG against one rendered from
  HEAD).
- The firmware build passes.

## A3. Relay accepts the new fields (no push yet) — relay-dev, timebox 1.5 h

**Read:** `relay/app/wirecbor.py:150-260`, `relay/app/wire.py:225-375`, `relay/app/ingest.py:320-335,1050-1120`,
`relay/app/store/devices.py:200-245`, design §3 (`/status`, Relay).

**Files:** `relay/app/wirecbor.py`, `relay/app/wire.py`, `relay/app/ingest.py`, `relay/app/store/devices.py`,
`relay/tests/` (new or extended tests).

**Do:**
1. Key maps:
   - `KEYMAP`: add `"ast": 72` and `"ast_id": 73`.
   - `CFG_KEYMAP`: add `"ast": 6`.
   - `OTA_KEYMAP`: add `"fv": 9`.
   - Encode and decode `cfg.ast` with the same `_ota_to_int` / `_ota_to_names` that `cfg.ota` uses.
2. `StatusEnvelope`:
   - `ast: int | None` (0..2) and `ast_id: str | None` (16 lowercase hex).
   - A bad value drops to None, like `_drop_bad_ota_field`.
3. `devices.py`:
   - Add `assetsSrc: int | None` and `assetsId: str | None`.
   - Add `OtaJob.part: Literal["app","assets"] = "app"`.
   - Make `OtaJob.kind` accept `"full"` for assets (it already does).
4. Ingest:
   - Store `assetsSrc` / `assetsId` from every online status, and clear both when it lacks them.
   - An `otaJob` with `part == "assets"` is finished when the existing terminal rule fires or when
     `ast_id == target16`.
   - When an online status lacks `ast`, delete `pendingCfgAst`. A4 adds the slot constant; reference it
     by the string `"pendingCfgAst"` until then.

**Verify:** pytest round-trips a CBOR status with keys 72/73, a `cfg.ast` with `fv`, the bad-value drop,
the job-finish rule, and the pending drop. The existing OTA tests stay green.

## A4. Device: `cfg.ast` job in ota.c — firmware-dev, timebox 4 h

**Read:**
- `firmware/main/ota.{c,h}` (whole), `firmware/main/cfg.c:1-210`.
- `firmware/main/modes.c` lines 640-700 (`STK_*`), 895-1000 (`/status`), 2394 (`ota_boot`), 3680-3700
  (`ota_service`), 1209-1230 (`render_pending`).
- `firmware/host/test_ota.c`, `test_cfg.c`; design §2 (write order) and §3.

**Files:** `firmware/main/ota.{c,h}`, `firmware/main/cfg.c`, `firmware/main/modes.c`,
`firmware/main/scr_device.c`, `firmware/host/test_ota.c`, `firmware/host/test_cfg.c`.

**Do:**
1. **ota.h**
   - Add `#define OTA_PART_APP 0`, `#define OTA_PART_ASSETS 1`.
   - Append `OTA_REJECT_NOPART` and `OTA_REJECT_FMT`. Append `OTA_ERR_NOPART` (`"nopart"`) and
     `OTA_ERR_FMT` (`"fmt"`) at the END of `ota_err_t`; the values are persisted.
   - `ota_status_t` gains `uint8_t ast; char ast_id[17];`.
   - Add:
     ```c
     bool ota_parse_ast_submap(const uint8_t *, uint16_t, ota_cfg_t *, uint8_t *fv);
     ota_reject_t ota_check_accept_assets(const ota_cfg_t *, uint8_t fv, bool have_part,
                                          uint32_t part_size, const uint8_t *active_sha);
     void ota_apply_ast_submap(const uint8_t *, uint16_t, const char *id);
     bool ota_take_redraw_request(void);
     ```
   - `ota_cfg_t` and `ota_job_t` stay unchanged.
2. **Pure section**
   - Factor the existing decoder into a static function with a flag that accepts key 9 (`fv`, uint
     1..255). For assets, `fv` is required and `fmt` must be full (delta → `OTA_REJECT_FMT`, returned by
     `check_accept_assets`). The `cfg.ota` behaviour is unchanged; `fv` stays ignored there.
   - `ota_check_accept_assets` order:
     1. `!have_part` → NOPART
     2. `fv > GFX_ASSETS_VER_MAX` or delta → FMT
     3. `isz > part_size - 4096` → SIZE (reuse `OTA_ERR_SIZE`)
     4. `active_sha` non-NULL and equal to `img` → RUNNING
     5. otherwise OK
   - Put `GFX_ASSETS_VER_MAX` in `gfx.h` and include it; the pure section must still build on the host.
3. **NVS:** a new key `"jast"` blob `{u8 part; u8 fv;}`, written by `job_persist()` and erased with the
   job. `job_load()` reads it; if it is absent, part is app.
4. **Handoff:** `s_pend_part` and `s_pend_fv` sit next to `s_pend_cfg`. Cancel applies only to a job of
   the same part; otherwise it only acks.
5. **take_pending (assets)**
   - Same duplicate rule as app jobs.
   - Then `teardown_transfer()`, `s_install_now=false`, `revert_ready_boot()`.
   - Then call `ota_check_accept_assets(cfg, fv, astpart_find()!=NULL, part->size, sha-of-active-or-NULL)`.
     The active sha comes from `astpart_read_rec` when `gfx_assets_source()==GFX_ASSETS_OTA`.
   - Store the job, persist, ack.
   - Skip the app-only `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`/`next->size` block for assets.
6. **start_transfer (assets)**
   1. Target = `astpart_find()`.
   2. If `gfx_assets_source()==GFX_ASSETS_OTA`, call `gfx_select(GFX_ASSETS_FACTORY)` and set the redraw
      request.
   3. `astpart_invalidate()`.
   4. Do not call `esp_ota_begin`.
   5. `otapipe_init(full)` with a new `write_cb_assets`. It keeps `s_wr_off`/`s_erased_end`: before
      writing `[off, off+len)` it erases whole 4 KiB sectors up to `off+len`, then calls
      `esp_partition_write`. Any error sets `s_write_err` and returns -1.
   6. The pending-verify gate in `ota_should_start` stays as it is for both parts.
7. **complete_transfer (assets)**
   1. Check `osha`/`out_len` as now.
   2. Enter a verify sub-step (`s_verifying=true`): mmap `[0,isz)`; then each `ota_service()` call hashes
      ≤ 64 KB with mbedtls SHA-256 (`transfer_step` checks `s_verifying` first).
   3. At the end, require sha == `cfg.img` (else `fail_job(OTA_ERR_HASH)`), and
      `gfx_assets_check(map, isz, &ver) && ver == fv` (else `fail_job(OTA_ERR_FMT)`). munmap.
   4. `astpart_write_rec({isz, img, fv})`; on error `fail_job(OTA_ERR_FLASH)`.
   5. `gfx_select(GFX_ASSETS_OTA)`, set the redraw request, `set_state(OTA_ST_OK)`, persist,
      `request_status()`.
   6. Teardown must munmap if verify was interrupted.
8. **ota_boot (assets job)**
   - DL / READY / INST → WAIT.
   - But if `astpart_read_rec` is valid and its sha == `job.cfg.img` → OK.
   - Never run the app-only branch (`is_target`, `nobl`, last-invalid) for an assets job.
9. **Status**
   - `ota_get_status` fills `ast`: 0 = no row; 1 = factory; 2 = OTA (from `gfx_assets_source()`).
     `ast_id` comes from `gfx_assets_ota_id16()`.
   - In `modes.c`, add `#define STK_AST 72` and `#define STK_AST_ID 73`. Always write `ast`; write
     `ast_id` only when `ast==2`. Update `nfields`.
   - Poll `ota_take_redraw_request()` beside `ota_take_status_request()`, and on true force a full render
     the way `modes.c` already does for a mode change. Find that mechanism; do not invent one.
10. **cfg.c:** add `CFG_KEY_AST 6` → `have_ast/ast_off/ast_len` → `ota_apply_ast_submap`.
11. **scr_device.c:** one Device-info line: `Fonts: factory` or `Fonts: <id16>`.

**Verify:**
- `test_ota` covers:
  - `cfg.ast` decode (with fv, without fv → false, delta → FMT);
  - `check_accept_assets` for each reason;
  - `cfg.ota` decode unchanged, using the existing vectors.
- `test_cfg` dispatches key 6.
- `make -C firmware/host test` passes, and the debug and release builds pass.
- `size` of the release image is within +6 KB of HEAD; report the number.

## A5. fwpub `assets-publish` / `assets-withdraw` — tools-dev, timebox 1.5 h

**Read:** `tools/fwpub.py`, `tools/test_fwpub.py`, `tools/mkassets.py:110-120,410-420` (header), design §3
(Bucket, fwpub).

**Files:** `tools/fwpub.py`, `tools/test_fwpub.py`.

**Do:**
1. Add `check_assets(data)`. It requires:
   - magic `PGFA`;
   - `1 <= data[4] <= 1` (keep a constant `ASSETS_FV_MAX = 1` next to the comment "== gfx.h GFX_ASSETS_VER_MAX");
   - `u32 total @8 == len(data)`;
   - `len <= 0x200000 - 4096`.

   It returns `{id: sha256 hex, id16, fv, lang: data[5:8] stripped of NUL, size}`.
2. Add `assets-publish <bin> --bucket --label TEXT [--dry-run --out]`. It writes:
   - `as/<id16>/full.z`: `zlib.compress(data, 9)`, IMMUTABLE;
   - `as/<id16>/manifest.json`;
   - `as/index.json`: NO_STORE, shape `{v:1, builds:[{id,label,lang,fv,size,published,full:{path,osz,osha}}], withdrawn:[]}`.

   Refuse if the id is withdrawn, or if `full.z` exists with different bytes (same rules as `publish`).
3. Add `assets-withdraw <id16|label> --bucket --reason`, which mirrors `withdraw`.

**Verify:** `test_fwpub.py` adds a dry-run publish of `build/images/assets-dejavu-emoji.bin` into a temp
dir (or a synthetic PGFA image if build/ is absent). Assert `osz == 379008` for the real file, the index
shape, a refusal on a wrong `total`, and that withdraw moves the entry.

## A6. Relay: assets index, push/cancel, routes — relay-dev, timebox 3 h

**Read:**
- `relay/app/firmware.py` (whole);
- `relay/app/devcfg.py:150-180,590-650`;
- `relay/app/routers/admin.py:980-1120`, `relay/app/routers/family.py:385-430`;
- design §3.

**Files:** `relay/app/firmware.py`, `relay/app/devcfg.py`, `relay/app/routers/admin.py`,
`relay/app/routers/family.py`, `relay/tests/`.

**Do:**
1. `firmware.py`:
   - Extend `_PATH_RE` with `|as/[0-9a-f]{16}/full\.z`.
   - Add an `AsBuild` model: `id`, `label`, `lang`, `fv` (1..255), `size` (≤ 0x200000-4096), `published`,
     `full: FwObject`.
   - Add `load_assets_index(settings)`: fetch `settings.fw_bucket_base + "as/index.json"` with the same
     TTL cache and errors as `load_index`.
   - Add `build_cfg_ast(build, settings)` → `{img,isz,url,osz,osha,fmt:"full",fv}`.
2. `devcfg.py`:
   - `_PENDING_CFG_AST_FIELD = "pendingCfgAst"`, added to `_ALL_PENDING_FIELDS` so it is republished on
     the online edge.
   - `push_ast(device_id, cfg_ast, broker, job=)` stores `otaJob` with `part:"assets"`.
   - `cancel_ast` sends `{"cancel": true}` and clears `otaJob`.
3. Routes, in both admin and family, with the same auth and rate limits as firmware:
   - `GET /assets?device=` lists builds and marks the active one (`assetsId`).
   - `POST /devices/{id}/assets` with body `{target: id16}` or `{cancel: true}`.

   Return 409 when:
   - the last online status has `ast` not in {1, 2} (`"needs firmware with font OTA and the new partition table"`);
   - `otaJob` is non-terminal (`"another update is in progress"`).

   Also make the existing `/ota` push return the same 409 while an assets `otaJob` is live.

**Verify:** pytest covers:
- a pushed `cfg.ast` that encodes to CBOR under 640 B signed;
- both 409s;
- cancel;
- index validation (bad path, oversize);
- the existing firmware tests stay green.

## A7. Web: "Update fonts…" — web-dev, timebox 2 h

**Read:** `web/components/FirmwareUpdateDialog.tsx`, `web/components/FirmwareChip.tsx`,
`web/app/{family,admin}/devices/page.tsx` (where the firmware dialog is mounted).

**Files:** new `web/components/FontsUpdateDialog.tsx`, the two devices pages, and the device card
component that shows the firmware chip.

**Do:**
1. Clone the firmware dialog's structure against `/api/{family,admin}/assets`. List each build's label,
   id16, size and est. bytes (`osz × 1.045 + 7168`). Provide Push and Cancel.
2. Disable it with the server's 409 text shown.
3. On the device card, add `Fonts: factory` / `Fonts: <label> (<id16>)` / `Fonts: —` (for `assetsSrc`
   null). While `otaJob.part=="assets"`, show the job's `otaState`/`otaPct` next to it.

**Verify:** lint and build pass. Do a 390 px-wide check with the same-origin iframe method (memory:
chrome-phone-width-check) against a local relay with a fake device whose `assetsSrc` is 1.

## A8. Bench: one-time USB migration per pager — bench agent (owner present for rc1/sora1), timebox 30 min per pager

**Read:** `docs/HARDWARE_TESTING.md`; memory notes `esptool-usb-reset-sleeping-pager`,
`usb-jtag-port-open-resets-chip`; design §1.

**Files:** none (flash only). Logs go to `build/bench-logs/assets-ota-migrate-<pager>.log`.

**Do** (proto3 first, then proto2, then rc1 and sora1 only with the owner's go-ahead). The pager keeps
its current app.
1. `esptool.py --chip esp32s3 -p $PORT --before=usb_reset read_flash 0x8000 0xC00 pt-<pager>.bin`, then
   decode it with `gen_esp32part.py`. **STOP** unless it shows exactly the six old rows, with msghist at
   0x520000 128K.
2. Back up history: `read_flash 0x520000 0x20000 msghist-<pager>.bin`.
3. `erase_region 0x540000 0x2E0000`. This erases 0x540000–0x81FFFF and never touches 0x520000–0x53FFFF.
4. `write_flash 0x8000 firmware/build/partition_table/partition-table.bin` (from A0's build).
5. Let the pager boot and capture one bounded boot log.

**Verify:**
- Boot log: no `msghist unavailable`; `assets loaded` (old app) or the A2 source line (new app).
- The owner reads the glass: the recent messages are listed, and the fonts are normal.
- On the new app, record the A0 `msghist mounted in … ms, internal free …, psram free …` line, and
  compare internal free against the same line from a new app on the old table (proto2 before its
  migration). Expect ≈ +2.7 KB internal and ≈ −21 KB PSRAM, mount ms ≤ 150. Record the numbers in the
  design doc §1, replacing the UNVERIFIED estimates.

## A9. Bench: font OTA end to end — bench agent, timebox 2 h (proto3, release image of A4)

**Read:** design §6 (Bench) and §4.

**Do:**
1. Publish the emoji image with `fwpub.py assets-publish build/images/assets-dejavu-emoji.bin --label "dejavu+emoji"`.
2. Run design §6 bench steps 2–7 in order. For step 5, flash `tools/astpart.py` outputs at `0x620000`
   (the pager is on factory fonts afterwards).
3. Take one current trace of step 3 (the download).

**Verify:** for each step, record the exact log line, plus the `/status` `ast`/`ast_id`/`ota_st` as the
relay stored them. Measure B/s (`osz` / download seconds from the `download start` and `ok` timestamps)
and mAh, and replace the §4 estimates with them. The owner confirms emoji on the glass as a b/w list. Any
mismatch with the design → stop and report; do not patch on the bench.
