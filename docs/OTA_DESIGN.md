# OTA firmware update — design (7 Oct 2026)

Owner brief: build/bench-logs/DESIGN_ota_brief.md. Tasks: build/bench-logs/TASK_ota_{backend,infra,firmware,web}.md.
Numbers marked *measured* came from build/images/*-release-app.bin on 7 Oct 2026 (scripts and the host
decode prototype are in build/bench-logs/ota_meas/); *estimate* means not measured, with the assumption stated.

## Status (8 Oct 2026)

- **Implemented:** `firmware/main/ota.c` and `otapipe.c`; relay `push_ota` and `cancel_ota` in
  `relay/app/devcfg.py`, with the index reader in `relay/app/firmware.py`; the web Devices "Update
  firmware…" dialog (`web/components/FirmwareUpdateDialog.tsx`); `tools/fwpub.py`; bucket
  `kid-pager-pager-fw` with index `fw/index.json`
  (https://storage.googleapis.com/kid-pager-pager-fw/fw/index.json).
- **Verified on hardware on proto3 (7 Oct 2026):** a delta OTA and a full OTA each reached `ota_st ok`,
  and an image that aborts at boot came back `rb` (`.overnight-handoff.md`, the 7 Oct 2026 entries
  titled "~10:25 am PDT, OTA ROLLBACK VERIFIED", "~12:15 pm PDT, FIRST OTA SUCCEEDED" and "~1:10 pm PDT").
- **Precondition, unchanged (D8):** each pager needs one USB flash of bootloader, partition table,
  otadata and app before its first OTA.
- **Release:** v1.1.0 (tag cut 8 Oct 2026) is the first tagged release published to the bucket, with
  `tools/fwpub.py publish build/images/v1.1.0-release-app.bin --bucket kid-pager-pager-fw`; deltas
  are built against the previous index entries (beta-80/81) and the v1.0.0-era bench images.
- Per-item status for the §7 bench list is noted under §7.

## 1. Decisions

| # | Decision |
|---|---|
| D1 Identity | An image is named by its **image id** = the 32-byte SHA-256 that esptool appends to every app `.bin` (= the file's last 32 bytes = SHA-256 of all preceding bytes; `hash_appended=1` is checked). The **short id** is its first **16 hex chars (64 bits)**: it only names objects; every security decision compares the full 32 bytes. 64 bits keeps the chance of two releases colliding below 10⁻¹¹ even at 10⁴ releases, and nobody gains by guessing a name because the objects are public anyway (D3). The device gets its own id for free: `esp_partition_get_sha256(running)` returns the appended digest after verifying it. |
| D2 Artifact | Each release publishes **one full object** `fw/<id16>/full.z` (zlib, level 9, 32 KB window) and **up to K=3 deltas** `fw/<id16>/from-<base16>.dz` (zlib level 9 of a detools *sequential* bsdiff patch, created with `--compression none`), plus `fw/<id16>/manifest.json`, and it rewrites `fw/index.json`. The base of a delta is always **the exact running image** of some device: the last 3 released images, plus any image passed with `--base` (for example a bench-built image on proto2/proto3). There is no special factory delta, since a factory image is just one more base, and deltas are never chained. |
| D3 Transport | The device makes **one direct HTTPS GET to `storage.googleapis.com`** on a **public-read** bucket over the existing cafetch socket (TLS profile 3, validation off), the same way the CA fetch already gets its trust from a hash and not from the transport. A broken transfer resumes with `Range: bytes=N-` within the same boot. The relay is not in the byte path, and there is no signed URL. |
| D4 Decoder | **ROM miniz `tinfl` (0 B of code) feeds either `esp_ota_write` (full) or the vendored detools C patcher (3.9 KB of code), which reads the base by memcpy from an `esp_partition_mmap` of the running slot.** zstd is rejected: its delta needs a decoder window as big as the base (§3). |
| D5 Command | The command is a signed `cfg` sub-map `cfg.ota` (§5). The device validates it, stores the job in NVS and acks `shown` **on acceptance**. Progress goes in `/status` fields, not in acks. |
| D6 When | The download starts in the first awake loop iteration where these all hold: not in airplane mode; the MQTT session is usable; no other cafetch is in flight; `batt_mv >= 3600`; `rssi >= -105`; no key pressed in the last 60 s. At most one attempt per 10 min and 6 per day. Install (the reboot) happens at the first moment with 5 min of no input while in sleep mode, or right away from "Install now" on the Device screen. The owner cares about bytes, not latency. |
| D7 Verify | SHA-256 is computed over the object bytes as they stream in, and checked against `osha` at EOF. `esp_ota_end()` re-verifies the image (appended digest). `esp_partition_get_sha256(target)` must equal `img` before `esp_ota_set_boot_partition()`. For a delta the base is checked before the first byte is fetched: the running id must equal `base`. |
| D8 Rollback | Set `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. The bootloader is not updated over the air, so **each pager needs one USB flash of bootloader + app** before its first OTA. The new image calls `esp_ota_mark_app_valid_cancel_rollback()` after its first successful `/status` publish on a connected MQTT session, or, in airplane mode, after first render plus 120 s of healthy loop. If it is not valid after 15 min of uptime (and not in airplane mode), it calls `esp_ota_mark_app_invalid_rollback_and_reboot()`. A panic or watchdog before that point rolls back in the bootloader. The old image then reports `ota_st:"rb"`, records the failed id in NVS, and refuses the same `img` again. |
| D9 Signature | **The HMAC-signed `cfg` is enough for v1, with no extra Ed25519 key.** The relay already holds K_dev and sees every message and location the pager has, so a relay compromise already exposes everything the pager knows. What OTA adds is persistent code execution on the pager. That risk is accepted while prod is family test data. The upgrade path is an Ed25519 release signature in `manifest.json`, checked by firmware against a public key compiled into the image. |
| D10 Who | Only a **super** admin can push or cancel. Family admins see the status column read-only. The relay picks a delta when the device's reported `img` matches a delta base in the index, and the full image otherwise. |
| D11 Publisher | `tools/fwpub.py` runs locally on the release build (CI does not build firmware today). It needs `detools` from pip and shells out to `gcloud storage`. |

## 2. Measured sizes (target main-release, 685,168 B)

| Object | Bytes | Note |
|---|---|---|
| full, zlib -9 (chosen) | **338,784** | gzip -9 338,803; zstd -19 312,975 (window 1 MB); zstd wlog=17 315,369; xz -9 293,288 |
| delta, 1 source change (shake→main), zlib(detools seq) | **20,774** | bsdiff/bz2 17,600; detools+heatshrink 31,540; zstd --patch-from 36,233 |
| delta, ~20 commits (keylat→main) | **42,513** | bsdiff 40,587; heatshrink 55,316; zstd --patch-from 59,772 |
| delta, 11 days apart (round5→main) | **51,884** | zstd --patch-from 67,029 |
| delta, 13 days apart (lockscreen-noleak→main) | **72,843** | zstd --patch-from 83,124 |

The host prototype (`ota_meas/src/proto.c`) reads 1500-byte input pieces into tinfl's 32 KB circular dictionary and from there into detools. All five objects decode to the target. The output SHA-256 was identical every time, and the full image and all four deltas took 2–3 ms on the host.

## 3. Decoder budget on the ESP32-S3 (owner: "within the boundaries of the chip")

**RAM free at runtime.** The `heap at boot` total was **2,170,496 B**. The PSRAM pool is 2048 KiB, and its largest free block after WiFi start was 1,998,848 B (= 2 MiB − flightrec's 96 KiB). So PSRAM has about 1.9 MB free, but WiFi's lwIP/driver allocations take about 92 KB of it when WiFi is on (*measured*: total free went from 2,170,496 to 2,078,099 B). Internal SRAM regions total 163 KiB, and 32 KiB of that is reserved for DMA. The only runtime internal floor on record is **68 KB free, 31.7 KB largest block**, with WiFi up (phaseAH-wifi.log; the PSRAM-heap figure is from phaseAJ-wifi.log). The OTA path therefore puts **all decoder state in PSRAM** (`MALLOC_CAP_SPIRAM`) and adds **< 1 KB of internal RAM** (stack depth inside the modes_run task, whose 16 KB stack is internal; flash writes require that). It reuses cafetch's existing static 1500 B receive buffer.

| Option | Working set | Code | Patch for keylat→main | Verdict |
|---|---|---|---|---|
| **tinfl + detools seq (chosen)** | tinfl state 8.4 KB (upstream struct, *measured*; ROM struct size is taken from `sizeof` at build) + 32 KB dict + detools 192 B + 4 KB write staging ≈ **45 KB PSRAM** | 0 B (tinfl is in ROM at 0x40000828) + **3,858 B** detools (*measured*, xtensa -Os) | 42,513 | Uses about 2 % of free PSRAM. The base comes from flash. |
| zstd --patch-from, uncapped | `ZSTD_estimateDStreamSize(685,168)` = **1,174,416 B** (*measured*, libzstd 1.5.7) | **29,066 B** (*measured*, xtensa -Os, decompress-only) | 59,772 | Rejected. It needs 62 % of free PSRAM, and its DCtx alone is 95,968 B. |
| zstd --patch-from, wlog 16/17/18 | DStream 358/620/751 KB | 29 KB | **299,170 / 283,002 / 279,197** (*measured*) | Rejected. A window smaller than the base cannot see the base, so the delta becomes a full image. |
| esp_delta_ota (detools + heatshrink) | similar to chosen | +heatshrink | 55,316 | Same patcher, 30 % bigger patches, and it would need a second codec for the full image |

**Can zstd read the base straight from flash?** Yes. `esp_partition_mmap()` of the running slot (gfx.c already maps assets the same way) gives `ZSTD_DCtx_refPrefix()` a flat pointer with no copy. The S3 MMU has 512 × 64 KB pages. The ~700 KB base needs 11, the code about 12, assets 16 and PSRAM 32, so about 71 are in use. The cost that rules zstd out is not the prefix but the **output window**, which must cover the whole base. The chosen path uses the same mmap: detools' `from_read` is a memcpy from that mapping, so there is no flash op and no cache disable on reads.

**CPU and flash time at 160 MHz** (the sdkconfig CPU clock, not 240; *estimate*). Inflate plus patch takes ~0.2–0.4 s for 685 KB of output: the host took 2–3 ms, scaled ×80. HW SHA-256 of the object and the image adds < 0.1 s. Flash is the real cost: 171 sector erases × 45 ms typ + 2,676 page programs × 0.7 ms ≈ **9.6 s**, spread over the download using `OTA_WITH_SEQUENTIAL_WRITES`. Each poll is capped at 16 KB of output (4 erases, ≤ ~230 ms stall), because a delta can inflate 1500 B of input into hundreds of KB of zeros. Cache stalls during erases are absorbed by the UART's hardware RTS/CTS flow control (enabled in WalterModem.cpp:5147).

**Streaming from the HTTP body.** Today cafetch buffers at most 4096 B (`CAFETCH_BODY_MAX`) and is fed in reads of ≤ 1500 B (`socketReceive`). The OTA path adds a **body sink**: the parser hands each body span straight to `otapipe_feed()`, and nothing is buffered beyond the 1500 B read. **Known vendor defect, PATCHES.md 1.7**: a read whose last byte is `\n` loses that byte. On binary data that is about 1 in 256 reads, which works out to about 60 % of full downloads. The firmware task exposes the modem's claimed byte count and treats any short copy as a transport error, then resumes with Range from the last good offset. A bench experiment (§7) decides whether the vendor parser itself gets fixed.

## 4. Bytes, cost and power per OTA

| Case | On-air bytes (*estimate*: object + 4.5 % TCP/TLS overhead + 6 KB handshake + 1 KB headers) | At $0.01–0.09/MB (SORACOM_EVAL.md) | OTAs inside the 3 MB left under the 10 MB/month bar (pessimistic 7.10 MB, §7.3) | in 100 MB |
|---|---|---|---|---|
| full | **≈ 361 KB** | $0.004–0.03 | 8 | ~277 |
| typical delta (keylat→main) | **≈ 51 KB** | $0.0005–0.005 | 58 | ~1,960 |
| worst job (budget cap) | 3 × object + 64 KB ≈ **1.08 MB** full / 0.19 MB delta | ≤ $0.10 | 2 | 92 |

Storage per release is about 339 KB plus 3 deltas of ≤ 73 KB, so ≤ 0.56 MB. GCS Standard costs < $0.001/month for 100 releases, and egress is $0.00004 per full OTA. Both are negligible.

**Power** (*estimate*). The modem pulls about 80 mA in RRC-connected receive and the ESP32 about 40 mA awake (§8.4), for about 120 mA. LTE-M plus the 115200-baud UART is assumed to deliver 4 KB/s; the UART ceiling is about 9.7 KB/s for 1500 B per AT round trip. A full OTA is 85 s download + 3 s TLS ≈ **2.9 mAh**. A delta is 14 s ≈ **0.5 mAh**. Add about 0.6 mAh for the reboot and reconnect (20 s at 100 mA). That makes a full OTA about 3.5 % of a sleep-mode day (95–107 mAh) and a delta about 1 %. TLS cost per connection is measured: the book fetch took 2,615 ms end to end (`bookpull: elapsed_ms=2615`), and net.h estimates about 5 kB per handshake.

## 5. Proposed protocol text (for PROTOCOL.md §3.2 / §5.1 / §10; not applied here)

**Applied to PROTOCOL.md §3.2, §3.3, §5.1, §5.4(e) and §10 on 6 Oct 2026.** The PROTOCOL text also records the JSON hex encoding of the hashes as an exception to §10's base64url rule, and the measured worst case (523 B signed JSON with the default bucket name).


**`cfg.ota`** (relay → device, CFG_KEYMAP `ota: 4`; OTA_KEYMAP `img 0, isz 1, url 2, osz 3, osha 4, fmt 5, base 6, psz 7, cancel 8`):
`{"img":"<64 hex>","isz":685168,"url":"https://storage.googleapis.com/<bucket>/fw/<id16>/full.z","osz":338784,"osha":"<64 hex>","fmt":"full"}`.
A delta adds `"fmt":"delta","base":"<64 hex>","psz":<detools patch bytes>`. `{"cancel":true}` aborts any job.
In CBOR the hashes are 32-byte bstr and `fmt` is 0 (full) or 1 (delta). The largest signed JSON is about 520 B, under the 640 B limit (§3.3).

**Device rules.**
- Ack `shown` once the job is stored, or once it is rejected. Rejection reasons are `base` mismatch, an image still pending verify, no rollback support, `img` already running, `img` on the failed list, `isz` > slot, or a URL that is not https. A rejection still sets `ota_st:"fail"` with an `ota_err`.
- A newer `cfg.ota` replaces the job.
- Airplane mode leaves the job parked and costs nothing.
- The relay re-publishes on an online edge like any other `cfg`.

**`/status` additions** (envelope keys 62–67; display only except `img`/`ota`):

| Key | Field | Meaning |
|---|---|---|
| 62 | `img` | 16 hex chars of the running image id |
| 63 | `ota` | Present as `1` when the firmware supports `cfg.ota` and its rollback bootloader has been confirmed (gate, like `bpull`) |
| 64 | `ota_t` | 16 hex chars of the job target |
| 65 | `ota_st` | `wait`, `dl`, `ready`, `inst`, `ok`, `fail` or `rb` |
| 66 | `ota_pct` | 0–100 |
| 67 | `ota_err` | Short code (≤ 8): `base`, `hash`, `osha`, `http`, `budget`, `flash`, `nobl`, `pv`, `size`, `expired` |

`fw` becomes `esp_app_get_description()->version`, truncated to 16 characters.
`/status` is published when the download starts, at 50 %, and at `ready`, `fail` and `rb`: four extra publishes, about 1 KB per OTA. A boot status carrying the new `img` means `ok`.

## 6. Failure modes and recovery

| Failure | Behaviour |
|---|---|
| Socket drop or stall (30 s with no bytes) | Resume with Range from the last good offset. Decoder state stays in RAM, and each resume costs one TLS handshake. |
| Reboot or power loss mid-download | The NVS job survives. The download restarts from 0 on the next window, counted against the budget. The half-written slot is harmless: otadata is untouched. |
| Total bytes > 3 × `osz` + 64 KB, or job older than 7 days | `fail/budget` or `fail/expired`. The job is dropped until a new `cfg.ota` arrives. |
| Hash mismatch (`osha` or `img`) | `esp_ota_abort()`, `fail/hash`, and one retry from 0 in the next window, then stop. |
| New image crashes, hangs, or never reaches the network | Bootloader or app rollback (D8). The old image reports `rb`. |
| Bootloader without rollback (a pager that was only app-flashed) | After the first OTA boot the state is not `PENDING_VERIFY`. The app marks itself valid, sets `ota_err:"nobl"` and drops the `ota:1` gate, so the relay stops offering OTA until the pager gets a USB flash. |
| Paging during the download | The MQTT socket runs alongside it; the book fetch already showed `mqtt_connected_before=1 after=1`. Each loop iteration does one ≤ 1500 B read plus ≤ 16 KB written, so paging is never blocked. |
| GCS unreachable over the modem's TLS (UNVERIFIED) | Gate experiment §7.1. If it fails, the relay serves `/fw/<id16>/<obj>` from the bucket with the same bytes and the same hashes, and only `url` changes. |

## 7. What to measure (bench, owner-run; no agent flashes)
1. **GCS reachability** (cheapest experiment, today's firmware): run `cafetch https://storage.googleapis.com/<bucket>/fw/probe-4k.bin <sha>` on the debug console. Expect `OK` and `mqtt_survived=1`. **Status 8 Oct 2026: in effect done.** The full and delta OTAs on proto3 (7 Oct) reached `ready` through the direct GCS path of D3. The 4 KB `cafetch` probe line itself is not in the handoff.
2. **Newline-loss defect**: run `otafetch` with a null sink on a 64 KB object made only of `0x0A` bytes, and count short-copy events. Then run it on the real 339 KB `full.z` and log B/s; that replaces the 4 KB/s assumption. **Status 8 Oct 2026: not recorded.** Patch 1.22 (short-copy detection with Range resume) is in the vendored tree. The handoff does not say whether any `short` resume line appeared, and the null-sink run and the B/s figure are not recorded.
3. **Current trace** of one full and one delta OTA, then compare against the 2.9 and 0.5 mAh estimates. **Status 8 Oct 2026: not recorded.**
4. **Rollback**: OTA to an image that panics at boot and confirm it comes back with `rb`. OTA a good image and confirm `PENDING_VERIFY` → valid after the first status. **Status 8 Oct 2026: done.** An image that aborts at boot (`PAGER_OTA_TEST_ABORT`) came back `rb` on proto3 at 10:22 am PDT, 7 Oct. A good image reached `ota_st ok` at 12:15 pm and 12:41 pm PDT the same day. The `esp_ota_mark_app_valid_cancel_rollback` log line is expected but is not quoted in the handoff.
5. **Airplane**: push `cfg.ota` while in airplane mode. Expect no socket opened and `ota_st` to stay `wait`. **Status 8 Oct 2026: waived by the owner on 7 Oct; not run.**
