# Font (assets) OTA — design (14 Oct 2026)

**Owner decisions, 14 Oct 2026.** There are two font partitions. `assets` is the read-only factory default
and is written only over USB. `assets_ota` is the only partition OTA writes. The app uses `assets_ota`
when it is valid and `assets` otherwise. The same one-time table change grows `msghist` from 128 KiB to
1 MiB. Tasks: `docs/ASSETS_OTA_TASKS.md`.

This design reuses `docs/OTA_DESIGN.md`: cafetch streaming, otapipe, the D6 gates, and the
status/pending-slot plumbing. *measured* = taken from `build/images/` on 14 Oct. **UNVERIFIED** = not
measured on hardware; §6 lists the bench check for each.

## 1. Partition table (one USB step per pager; nothing that exists moves)

```
nvs 0x9000 24K | otadata 0xf000 8K | assets 0x11000 1M readonly(flag only) | ota_0 0x120000 2M
ota_1 0x320000 2M | msghist 0x520000 1M (was 128K; last row) | assets_ota data/undefined 0x620000 2M
```
This is `gen_esp32part.py` output for the proposed CSV. The table ends at 0x820000; 7.9 MB of flash stays free.

**Old apps on the new table, new app on the old table.** v1.2.0–v1.2.4 find every partition by name or
type: `gfx.c:212` looks up `"assets"`, `msg.c:637` looks up `"msghist"`, the default label covers `nvs`,
and `esp_ota_*` and the bootloader find otadata/ota by subtype. A grep of `firmware/main` shows no
hard-coded offsets. Offsets appear only in USB flash recipes (`0x11000`, `0x120000`), and those do not
change. An app OTA writes only an ota_N slot and otadata. It never writes the table at 0x8000
(`fwpub.check_image` takes app `.bin` files only). So an app reached by OTA or rollback runs on whatever
table the pager has. The new app on an old table finds no `assets_ota`: it uses factory fonts, `/status`
reports `ast:0`, the relay offers no font OTA, and msghist stays 128 KiB.

**`readonly` (verified in IDF v5.2.1).** It is flag bit 1 in `gen_esp32part.py`. With it set,
`esp_partition_write`, `esp_partition_write_raw` and `esp_partition_erase_range` return
`ESP_ERR_NOT_ALLOWED` (`partition_target.c:67,109,127`). mmap is unaffected. The bootloader ignores flags
(`flash_partitions.c`). esptool writes raw flash, so a USB flash still works. The flag does not block a
direct `esp_flash_write()`, but `firmware/main` calls none.

**Size.** `assets_ota` is 2 MiB, not 1 MiB: flash is free, a partition can never grow once a row follows
it, and erase/air time scale with the image, not the partition. Largest image today 736,237 B (*measured*,
`assets-dejavu-emoji.bin`); largest allowed 2 MiB − 4 KiB (§2).

**msghist at 1 MiB.** NVS stores no partition size: page N is simply offset N × 4 KiB. Pages 0–31 keep
their data. Pages 32–255 load as free only if they read all 0xFF (`nvs_page.cpp:41-63`); any other
content loads as CORRUPT. `history_mount()` erases and remounts only on NO_FREE_PAGES/NEW_VERSION; any
other error leaves history RAM-only for that boot (`msg.c:654-676`). Losing history that way is
acceptable. No one can prove 0x540000+ was never written on rc1, sora1, proto2 or proto3, so the
**USB step erases 0x540000–0x81FFFF first**. Old apps open msghist by name and get the full 1 MiB too.

**msghist RAM (IDF 5.2 source).** NVS allocates with `malloc`. `PageManager` does one
`new Page[sectorCount]`. `sizeof(Page)` ≈ 84 B (estimate from the member list), so 256 pages need
≈ **21.5 KB in one allocation**. That is above `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, so it should land in
**PSRAM**, and today's ≈ 2.7 KB of internal heap is freed. Static DRAM (the debug build's
`dram0_0_seg` overflow of 2,488 B) does not change, because NVS uses only heap. HashList blocks scale with
the item count, which does not change. Mounting reads all ~224 empty pages (896 KB), once per cold boot in
`msg_init()` (`modes.c:2305`, after the boot frame). **UNVERIFIED:** `sizeof(Page)`, the PSRAM placement,
and the mount time (estimate 50–150 ms). MSG_THREAD_DEPTH and per-chat history are a separate follow-up
(8× the headroom).

## 2. Validity, atomic commit, selection

**Commit record.** 64 B, alone in the partition's last sector at `size − 4096`: `0 "PGAC" | 4 u32 rec_ver=1 |
8 u32 len | 12 sha256(image[0,len)) 32 B | 44 u8 fv | 45 15 B zero | 60 u32 crc32` (zlib-compatible, over
bytes 0..59; one table-free implementation shared with `tools/astpart.py`). SHA-256 is both the asset id
(like the app's D1) and the image check, computed in hardware; the CRC only catches a torn record write.
Rejected: writing the header last (a PGFA header has no checksum, and `parse_assets()` checks bounds, not
bitmaps, so a torn body would pass), and an NVS record (it can disagree with the flash after a USB reflash
or an NVS erase).

**Write order (in `ota.c`).** (1) If gfx uses `assets_ota`, `gfx_select(FACTORY)`, which unmaps it.
(2) Erase the commit sector; the partition is invalid from now on. (3) Stream sequentially, erasing each
4 KiB sector just before writing it, ≤ 16 KB out per `ota_service()` call (≤ 4 erases ≈ 230 ms, as for app
OTA). (4) At EOF: `osha` matches and `out_len == isz`. (5) mmap `[0,isz)`, SHA it ≤ 64 KB per call, and
require `== img`; require `gfx_assets_check()` (parses, `total == isz`, `version == fv`); munmap. (6) Write
the record (inside one 256 B flash page). (7) `gfx_select(OTA)`, full redraw, `ota_st ok`.
Power loss before step 6, or a torn record, leaves factory fonts. At boot the job goes DL→WAIT and restarts
from 0. Power loss between steps 6 and 7 is safe: at boot, an asset job whose target is the active id is
marked OK.

**Boot selection (`gfx_init`, before the first frame, estimated < 1 ms).** Use `assets_ota` only if the
magic, `rec_ver` and crc are correct, `16 ≤ len ≤ size−4096`, `fv ≤ GFX_ASSETS_VER_MAX` (1), and
`parse_assets(map,len)` passes with `total == len` and `version == fv`. Otherwise use factory and log the
reason once. There is **no re-hash at boot**: it would cost ~50–100 ms (UNVERIFIED) before the first frame,
to guard against bit rot that the factory partition is equally exposed to. A live `gfx_select` is safe
because `ui_render()` and `ota_service()` both run on the `modes_run` task. The exception is the
debug-console `ui_render` (`main.c:2551`): do not use it during an asset job.

**Format version and app rollback.** gfx accepts `version ≤ GFX_ASSETS_VER_MAX`. **Every future app must
keep reading v1**, because the factory partition is never rewritten. A rollback never touches `assets_ota`:
v1.2.x (no feature) maps `assets` (factory fonts) and its `/status` has no `ast`, so the relay offers
nothing; an app with a lower max version falls back to factory; rolling forward picks `assets_ota` up again.

## 3. Delivery: a separate `cfg.ast`, full image only

**Why a separate sub-map.** Naming the asset id in the firmware manifest would tie fonts to app releases.
Reusing `cfg.ota` with a `part` key would let an app rolled back to an old version treat a republished
pending job as an *app* image. Old firmware skips unknown cfg key 6 (`cfg.c:84`) and does not ack it.
**`cfg.ast`** (CFG_KEYMAP `ast: 6`) uses OTA_KEYMAP 0–8 plus a new **`fv: 9`**, which is required.
```
{"img":<64hex>,"isz":736237,"url":"https://storage.googleapis.com/kid-pager-pager-fw/as/<id16>/full.z",
 "osz":379008,"osha":<64hex>,"fmt":"full","fv":1}
```
It is about the size of a full `cfg.ota` (< 640 B). `{"cancel":true}` cancels an asset job. The device acks
`shown` on accept or reject. Rejection codes in `ota_err`: `nopart` (no row), `fmt` (`fv` > max, or a
delta), `size`, `run` (`img` is already active). The app gates `nobl`, `pv` and `bad` do not apply.

**Full image only (*measured*).** A full zlib-9 image is 379,008 B. A detools delta from `assets-dejavu`
is 111,747 B, and from the 18 Sep Noto image 370,937 B. New glyphs shift every offset, factory images
differ per pager, and the relay does not know a pager's factory id. A delta from factory fits otapipe
later if fonts change often; it would need the factory id in `/status`.

**One job slot on the device.** `ota_job_t` is unchanged; a new NVS key `ota/jpart` (u8, absent = app)
marks an asset job, so there is no blob version bump. A newer `cfg.ota`/`cfg.ast` replaces the job
(existing rule, including `revert_ready_boot`). Relay and web return 409 for a push of either kind while
`otaJob` is not terminal. Asset states are `wait → dl → ok | fail`, with no reboot.

**`/status`.** `ota_t`, `ota_st`, `ota_pct` and `ota_err` describe whichever job exists. New keys: **72
`ast`** (uint: 0 = feature but no row, 1 = factory active, 2 = `assets_ota` active; absent = old firmware)
and **73 `ast_id`** (16 hex, only when `ast` = 2).

**Bucket** (the same public bucket): immutable `as/<id16>/full.z` and `as/<id16>/manifest.json`; no-store
`as/index.json` = `{v:1,builds:[{id,label,lang,fv,size,published,full:{path,osz,osha}}],withdrawn:[]}`;
`id` = sha256(file). **fwpub.** `assets-publish <bin> --label` checks the PGFA magic, `total == len`,
`len ≤ 2 MiB−4 KiB` and a known version. `assets-withdraw <id16|label> --reason` withdraws a build.

**Relay.** An assets index loader, with `_PATH_RE` accepting `as/<16hex>/full.z`. `push_ast`/`cancel_ast`
use their own slot, `pendingCfgAst`, and set `otaJob.part = "assets"`. Ingest stores `assetsSrc`/`assetsId`;
an assets job finishes on a terminal `ota_st` with a matching `ota_t`, or when `ast_id == target16`. **Drop
`pendingCfgAst` when an online status lacks `ast`**, so a rolled-back app is not re-sent the job. Routes:
`GET /api/{family,admin}/assets?device=` and `POST /api/{family,admin}/devices/{id}/assets` (`{target}` or
`{cancel:true}`), gated on `ast ∈ {1,2}`. **Web:** an "Update fonts…" dialog, and the device card shows
`Fonts: factory | <label> (<id16>)`.

## 4. Power, data, latency

**No job:** +0 mA (the job is pushed; nothing polls). **One font OTA:** on air ≈ 379,008 × 1.045 + 7 KB ≈
**403 KB** (0.4 % of the 100 MB month). At OTA_DESIGN's 4 KB/s (**UNVERIFIED**; app-OTA B/s was never
logged) that is 98 s at ~120 mA ≈ **3.3 mAh**, plus flash work (180 erases + 2,876 programs ≈ 10 s ≈ 0.1 mAh),
no reboot: ≈ 3.4 mAh, ~3 % of a 95–107 mAh sleep-mode day. **Worst case:** the budget cap, 3·osz + 64 KB ≈
1.2 MB. **Latency:** paging continues during a download (each iteration reads ≤ 1500 B and writes ≤ 16 KB);
the job runs only under the D6 gates, after the render, and `ota_transfer_active()` already blocks light
sleep. From step 1 to step 7 the pager shows factory fonts (tofu for glyphs factory lacks), and a cancelled
or failed job leaves factory fonts until the next good job (*accepted risk; flagged to the owner*). Wake
sources and RTC memory are unchanged; job state lives in NVS.

## 5. Failure modes (power loss, torn record, too-new format, missing row: see §1–§3)

| Failure | Result |
|---|---|
| Corrupt transfer | `osha` mismatch → one retry from 0 (`fail_hash_count`), then `fail osha` |
| Readback ≠ `img` / header bad / `version ≠ fv` | `fail hash` / `fail fmt`; record never written |
| A bug writes `assets` | `ESP_ERR_NOT_ALLOWED` (readonly) → `fail flash` |
| App job forced over an asset job (API) | Newer job replaces it; partition invalid → factory |

## 6. Verification

**Host tests:** `test_astpart` (round trip, torn record, crc equal to `zlib.crc32`); `test_gfx_assets` (v1 ok,
v2 and `total` mismatch rejected); `test_ota` (`cfg.ast` with `fv` required and delta rejected, reject
reasons); `tools/test_fwpub.py`; relay pytest.

**Bench** (proto3 first; one owner on the port). (1) Migrate a pager on v1.2.3 (erase + table): fonts,
identity and history intact; log internal/PSRAM free heap and msghist mount ms before and after. (2) New app
on the old table: `ast:0`. (3) Push the emoji build: `dl`→`ok`, emoji on the glass, `ast:2`+`ast_id`; log B/s
and take a current trace. (4) Reset mid-download: factory, then restart from 0 to `ok`. (5) `tools/astpart.py`
with a bad crc, `fv=2`, or a corrupt magic byte → factory each time (a body byte is not detected at boot, by
design). (6) Corrupt transfer: shares `fail_job()` with app OTA; host-tested, and bench-tested only if a
raw-cfg push with an altered `osha` is cheap (optional). (7) With `ast:2`, roll the app back (USB
v1.2.3, or OTA of `PAGER_OTA_TEST_ABORT`) → §2 behaviour; back on the new app → `assets_ota`.
