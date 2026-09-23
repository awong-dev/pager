# Device: next tasks (group `sndr`, accelerometer, SMS)

Same shape as `docs/V03_TASKS.md`: each task names its agent, the files to read first, the files it
may touch, what to do, and how to verify. Bench tasks obey `docs/HARDWARE_TESTING.md` and the bench
rules in `.overnight-handoff.md` verbatim: one reader on the port, one hardware agent at a time,
bounded foreground captures. Coding agents never open `/dev/cu.usbmodem*`.

Build (firmware): `export PATH=<pyshim>:$PATH; . ~/src/esp/esp-idf/export.sh; cd firmware;
PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build`. Host tests: `make -C firmware/host test`
(every firmware task must leave them green).

Order: G7 (unblocked) · A1 → A2 → A3 → A4 · S0 → S1 → S2 → (S3 gated on S4) → S4 → S5.
Independent of each other; the three parts may run in parallel by different agents.

---

## Part 1 — group chat `sndr` (G7)

### G7 Device: parse and render `sndr` — firmware-dev

**Architect sign-off, 23 Sep 2026: the shape below is decided. Implement it literally; there is no
judgment call left in this task. If something here contradicts the code, stop and report.**

**Read:** `docs/PROTOCOL.md:133` (`sndr` row), `:270`, `:1292` (key 51); `docs/GROUP_CHAT_DESIGN.md`
§4; `firmware/main/msg.c:1340-1500` (down-parse loop, `MK_*` at `:438-446`, `default: cbor_r_skip()`
at `:1492-1498`), `msg.c:289-340` (`msghist_record_encode/_decode`), `msg.h:100-130,134-230`,
`firmware/main/cbor.c:418-469` and `cbor_r_tstr` (both restore `r->pos` on failure — this is what
makes the rule below safe), `firmware/main/scr_chat.c:284-340` (`chat_row_src_t`, `load_row_src()`).
**Files:** `firmware/main/msg.c`, `firmware/main/msg.h`, `firmware/main/scr_chat.c`,
`firmware/host/test_cbor.c`, `firmware/host/test_msg.c`. **Not** `scr_pick.c` (decision 5 below).

**Do — the six decisions:**

1. **Storage.** New field `char sndr[MSG_FROM_MAX];` on `msg_t` (`msg.h:107-131`), placed next to
   `to`. Cost: 17 B declared, **+16 B/entry after alignment** (392 → 408), × `MSG_THREAD_DEPTH` 32 =
   **+512 B of `.bss`**; `scr_chat.c`'s parallel `chat_row_src_t s_src[32]` gains the same field for
   another +512 B. Thread RAM 12.5 kB → 13.1 kB; whole-device RAM ≈23.1 kB → **≈24.1 kB** of ~512 kB
   (`docs/PROTOCOL.md:1183`). State that total in the report.
2. **Not in RTC.** `msg_unread_t` (`msg.h:256-261`) does **not** gain `sndr`. `pager_rtc_t`'s 1184-byte
   budget is nearly full (modes.c's own comment) and the only cost of omitting it is that the single
   unread row recovered by `warm_recover_unread()` *when `msghist` is unavailable* loses its author
   line. Accept that and say so in a one-line comment.
3. **msghist record.** Add `sndr` as a fourth length-prefixed string in `msghist_record_encode()`,
   written after `to`, before `body_len`. Worst-case record 393 → 410 B, so bump
   `MSGHIST_REC_MAX` 400 → **420** (`msg.h:190`). Bump `MSGHIST_REC_VERSION` 1 → **2**, and make
   `msghist_record_decode()` accept **both**: version 1 → read id/from/to then `body_len` as today and
   leave `sndr` empty; version 2 → read the extra field. Without that dual read every stored message
   is dropped on the first boot after the upgrade. Empty `sndr` costs 1 byte per record.
4. **Parse + validation.** `#define MK_SNDR 51` beside the other `MK_*`. New `case MK_SNDR:` in the
   down-parse loop. Rule — **absent is normal, bad is ignored, never malformed**:

   ```c
   case MK_SNDR: {
       const char *s; size_t slen;
       if (!cbor_r_tstr(&r, &s, &slen)) {          /* wrong CBOR type: ignore it */
           if (!cbor_r_skip(&r)) { msg_count_malformed(); return MSG_INGEST_MALFORMED; }
           break;
       }
       if (slen > 0 && slen < sizeof(sndr_local)) { memcpy(sndr_local, s, slen); sndr_local[slen] = '\0'; }
       break;                                       /* empty or >16 chars: stays "" */
   }
   ```

   Length-only, exactly the rule `from` gets at `msg.c:1408-1419` (this firmware never regex-checks an
   alias; do not add a charset check for `sndr` alone). Only a truncated/unreadable CBOR head is
   malformed, which is already true of every key. Copy `sndr_local` into the stored entry next to
   `from`; `sndr` is never set on an up message and never sent on `/up`.
5. **Render — prefix on row 0, no extra row.** In `load_row_src()` (`scr_chat.c:304`), the down branch
   becomes: `who = (m->sndr[0] != '\0') ? m->sndr : m->from`. That is the whole change. **Why not an
   extra row:** the thread shows 6 rows at normal and **4 at large** (`visible_rows()`,
   `scr_chat.c:257-264`); an author row would spend 17-25 % of the screen on a name that is already
   the row's leftmost word, and at large font would halve a group thread. Row 0's wrap width is
   measured from the `who` string (`wrap_width_row0()`), so substitution costs only the difference in
   alias lengths — no new layout maths, no `chat_build_rows()` change. The group's own name is **not**
   on the message row: it is constant for the whole thread and belongs on the pick row and (when a
   per-peer view is exposed) the header — today's merged thread does not show which conversation a DM
   belongs to either, and adding it for groups only would be an asymmetry paid for in pixels. No
   "different from `from`" test is needed: if the relay ever sets `sndr == from` the rendering is
   identical anyway.
6. **Pick screen — zero firmware change.** `scr_pick.c:170-171` already prints `book_contact_t.type`
   verbatim, right-aligned; `book.c:122` copies `t` verbatim with no enum check, and `BOOK_TYPE_MAX`
   is 5. So a group row labels itself the moment the relay sends **`t: "grp"`** instead of `"web"`.
   **This amends `docs/GROUP_CHAT_DESIGN.md` G3 and `docs/PROTOCOL.md:140`'s `t` enum (`web`/`sms`/
   `chat`/`grp`) — raise that with backend-dev; it is a one-string relay change and an un-updated
   pager renders "grp" harmlessly.** G7 itself touches no screen but `scr_chat.c`.

**Verify:** `make -C firmware/host test` green, with new cases: `test_cbor.c` — an unknown key 51
carrying a `tstr` is skipped by the old path, and `cbor_r_tstr` failure leaves `pos` unmoved.
`test_msg.c` — a page with `sndr` stores it and renders `who` as the author; a page without `sndr` is
byte-for-byte today's behaviour including the ack; `sndr` of 0, 17 and 40 chars and `sndr` encoded as
a uint are each **ingested NEW** with `sndr == ""`; a signed page with `sndr` still verifies; a v1
record round-trips through the v2 decoder with `sndr == ""`; a v2 record with a 16-char `sndr` fits
`MSGHIST_REC_MAX`. Build clean. No `/dev/cu.*`.
**Depends on:** nothing (G3 may land before or after). **Bench check rides along with G9.**

---

## Part 2 — accelerometer (LIS3DH)

State of play, established from the code, so no agent re-derives it: the motion path is **already
complete end to end**. `accel_poll()` (`accel.c:123`) → `loc_on_motion_event()` (`loc.c:877`) →
`loc_trigger_motion_event()` (`loc.c:203`, the pure 60 s-within-3 min classifier, host-tested at
`firmware/host/test_loc.c:168-200`) → `modes_note_motion_reset()` (`modes.c:407`) →
`coverage_on_motion()` (`coverage.c:120`, host-tested at `test_coverage.c:160`). **Both backoffs are
reset today**; `docs/ROADMAP.md:87-88` describes shipped behaviour, not a gap. ext1 is armed on IO2
only after WHO_AM_I answers (`net.cpp:1283-1300`, `net_enable_accel_wake()` at `:2032`). There is no
deep sleep anywhere in this firmware — light sleep only — so "wake the ESP" means ext1 + light sleep.
A1 is therefore the only real code gap, and it is a power gap, not a functional one.

### A1 Motion wake-storm guard — firmware-dev

**Read:** `firmware/main/accel.c` (all 135 lines), `accel.h`, `net.cpp:1270-1320` (`net_sleep()`,
`s_wake_sources_armed` — note it arms **once** and never disarms), `net.cpp:2032`, `net.h:610-620`,
`firmware/main/modes.c:2330-2345` (where `accel_poll()` is called).
**Files:** `firmware/main/accel.c`, `accel.h`, `firmware/main/net.cpp`, `net.h`,
`firmware/host/test_accel_rate.c` (new), `firmware/host/Makefile`.
**Do:** CTRL_REG5 latches INT1 until `INT1_SRC` is read (`accel.c:51-53`), and ext1 is
`ESP_EXT1_WAKEUP_ANY_HIGH` on that same pin. So while the pager is being carried, every LIS3DH sample
above threshold — **up to 10/s at the configured 10 Hz ODR** — ends light sleep. Estimate, stated with
its assumption: light-sleep floor ~1 mA (`net_sleep()`'s own vendor citation); one wake + loop body
~20 ms at ~35 mA ⇒ ~0.7 mAs each; 10 wakes/s ⇒ ~7 mA *extra* average while moving; 2 h/day of being
carried ⇒ **~14 mAh/day**, ~13 % of `docs/ROADMAP.md:40`'s 95-107 mAh/day. The classifier needs only
**2 events ≥60 s apart** (`loc.c:203-231`), so ≥99 % of those wakes buy nothing.
Add a refractory: a new pure helper

```c
/* Returns true if this INT1 edge should be reported to the classifier, given the
 * timestamp of the last reported edge (0 = none yet) and the refractory in µs. */
bool accel_edge_wanted(int64_t now_us, int64_t last_reported_us, int64_t refractory_us);
```

in a host-testable spot (top of `accel.c`, above any ESP-IDF include, the split `sms.c`/`loc.c` use),
`ACCEL_REFRACTORY_S = 20`. In `accel_poll()`: read `INT1_SRC` as today (this clears the latch); on IA,
consult the helper; only a wanted edge calls `loc_on_motion_event()`. Then disarm ext1 for the
refractory window — add `void net_set_accel_wake(bool on)` to net.h/net.cpp replacing the one-way
`net_enable_accel_wake()` (keep the old symbol as a `net_set_accel_wake(true)` wrapper so `accel.c:116`
and any caller still links), and make `net_sleep()` call `esp_sleep_enable_ext1_wakeup()` /
`esp_sleep_disable_ext1_wakeup()` per-sleep from the current flag instead of latching
`s_wake_sources_armed` for ext1 (ext0/button arming stays exactly as it is). Re-arm from `accel_poll()`
when the window expires. Expected result: ≤1 wake/20 s while moving, **<0.2 mAh/day**, with the 60 s
classifier still firing at t≈60 s (events at 0, 20, 40, 60 s ⇒ span 60 s ⇒ fires).
Also add an ext1 wake counter: increment on `esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1`
after `esp_light_sleep_start()` and expose it via a `net_get_ext1_wakes()` accessor — A2 prints it and
A4 measures with it. Nothing else changes; do **not** touch `INT1_DURATION` for this (at 10 Hz ODR it
demands *consecutive* above-threshold samples, which impulsive walking never produces).
**Verify:** `test_accel_rate.c` — first edge always wanted; an edge 19.9 s later is not; 20.1 s later
is; a sequence at 20 s spacing fed into `loc_trigger_motion_event()` still fires at ≥60 s span; a
negative/backwards clock never wedges. `make -C firmware/host test` green; build clean. No `/dev/cu.*`.
**Depends on:** nothing. **Blocks:** A4.

### A2 `acceltest` debug console command — firmware-dev

**Read:** `firmware/main/main.c:440-500` (the `PAGER_DEBUG_NO_LIGHT_SLEEP` REPL block and its
caveats), `:532-556` (`cmd_smstest`/`cmd_smslist`, the shape to copy), `:793-838` (registration),
`firmware/main/accel.c` (register map), `docs/HARDWARE_TESTING.md` "Console commands".
**Files:** `firmware/main/accel.c`, `accel.h` (a small `accel_debug_*()` API — main.c must not touch
I2C or LIS3DH registers directly), `firmware/main/main.c`, `docs/HARDWARE_TESTING.md` (one table row).
**Do:** debug build only (`#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP`), registered beside `smslist`:

| Form | Prints |
|---|---|
| `acceltest` | present?, WHO_AM_I, CTRL_REG1-5, INT1_CFG/THS/DURATION/SRC as `REG=0xNN`, the current THS in mg (`ths*16 mg` at ±2 g), the refractory setting, edges reported to the classifier so far, and `net_get_ext1_wakes()` |
| `acceltest samples <n>` | `n` (1..600) live X/Y/Z from OUT_X_L..OUT_Z_H (0x28, MSB auto-increment bit 0x80) at 10 Hz as `x=.. y=.. z=.. mg`, plus `INT1_SRC=0xNN` on every sample where bit 6 (IA) is set |
| `acceltest ths <0-127>` / `acceltest dur <0-127>` | writes INT1_THS / INT1_DURATION at runtime and echoes the read-back |
| `acceltest refr <seconds>` | sets A1's refractory (0 = off, to reproduce the storm deliberately) |

Re-probe on every invocation so the owner can wire the chip and type `acceltest` without a reboot; if
WHO_AM_I now answers and `s_present` was false, run `accel_init()`'s configuration and say so. Runs on
the console task (the IDF I2C driver is per-port mutexed, so concurrent `ui_poll_keyboard()`/
`accel_poll()` is safe) — say that in the help text, and note that `samples 600` blocks that task for
60 s. No host test (device-only register I/O); the only pure part, `accel_edge_wanted()`, is A1's.
**Verify:** builds clean with and without `PAGER_DEBUG_NO_LIGHT_SLEEP`; `make -C firmware/host test`
green; `acceltest` on a unit with **no** chip prints "not present" and returns non-zero without
hanging. No `/dev/cu.*` in this task.
**Depends on:** A1 (for the refractory and wake counter it prints).

### A3 Record what "wired" means — firmware-architect or firmware-dev, 15 min

**Read:** `docs/V02_DESIGN.md:185-192`, `firmware/main/pins.h` "Motion", `docs/HARDWARE_TESTING.md`
item 9. **Files:** `docs/HARDWARE_TESTING.md` only.
**Do:** add an "Accelerometer" subsection under "Bench tools": SDA IO9 / SCL IO8 (shared with the
CardKB, **not** the other way round — `pins.h`'s own bench note), address **0x18** (0x19 if SDO/SA0 is
pulled high — then `i2cscan` shows 0x19 and `accel_init()` will not find it), INT1 → **IO2**,
push-pull **active high**, no pull-up needed, and 3V3 from the peripheral rail (GPIO0 low).
**Verify:** `i2cscan` is the one-line pre-flight check. Doc-only, nothing to build.

### A4 Bench: first light on the accelerometer — bench-tester, owner wires the chip

**Read:** A3's new subsection, `docs/HARDWARE_TESTING.md` bench rules, A2's help text.
**Do (one flash of the A2 build, in this order):**
1. `i2cscan` → expect `0x5F` (CardKB) **and** `0x18`. Neither 0x18 nor 0x19 ⇒ stop, it is wiring.
2. Reboot and capture the boot log: expect `LIS3DH found (WHO_AM_I=0x33), configured 10Hz low-power
   + high-pass INT1 motion interrupt` (`accel.c:117`). `LIS3DH not found` ⇒ back to step 1.
3. `acceltest` → WHO_AM_I `0x33`, CTRL_REG1 `0x2F`, CTRL_REG3 `0x40`, CTRL_REG5 `0x08`, INT1_CFG
   `0x2A`, INT1_THS `0x10`, INT1_DURATION `0x00`. Any mismatch is a failed write, not a tuning issue.
4. `acceltest samples 100` with the unit flat and still → z ≈ 1000 mg, x/y near 0, and **no**
   `INT1_SRC` lines. Any IA hits at rest ⇒ threshold too low, `acceltest ths 20` and repeat.
5. `acceltest samples 100` while walking with the unit in a pocket → `INT1_SRC=0x4A`-ish lines
   (bit 6 IA set) on a good fraction of samples. None ⇒ `acceltest ths 8` and repeat; note the value
   that first produces hits while walking but none at rest. **That value is the deliverable.**
6. **Sustained-motion check.** `acceltest refr 20`, then walk for 90 s holding the unit. In the log,
   watch for `sustained motion: backoff reset, 10min floor since last attempt applies` (`loc.c:885`).
   It must appear **once**, roughly 60-70 s after the walking starts — not at t=0, not repeatedly.
   Then stop moving for 3 min and walk again: it must appear once more. A second line within the same
   60 s means the ring is not being cleared; no line at all after 90 s means fewer than 2 edges got
   through, so lower THS or the refractory.
7. **Power.** `acceltest refr 0`, walk 60 s, `acceltest` → note `ext1 wakes`. Then `acceltest refr 20`,
   walk 60 s, `acceltest` again. Expect roughly **two orders of magnitude** fewer in the second run
   (~600 vs ~3). Report both counts; that ratio is A1's whole justification.
**Verify:** report the chosen INT1_THS (and DURATION if changed), the two ext1 wake counts, and the
timestamps of the `sustained motion` lines. Record the outcome in `docs/HARDWARE_TESTING.md`
"Seen working" and move item 9's accelerometer half out of "Not yet seen working". If step 5's best
threshold differs from `LIS3DH_INT1_THS_DEFAULT`, open a one-line follow-up to change the default and
delete the "UNVERIFIED" wording in `accel.c:5-12` and `accel.h:17-21`.
**Depends on:** A1, A2, A3, and the owner having wired the chip.

---

## Part 3 — SMS

Correction to `docs/ROADMAP.md:46-47`, established from the code and settled here: **a received SMS
already persists across reboot.** `msg_insert_sms_in()` (`msg.c:1732-1771`) assigns a `hist_seq` and
calls `history_write_entry()`, so the text lands in the 128 kB `msghist` NVS partition like any other
thread row, and the audit ring is separately persisted (`sms.c:861-940`). The two real gaps are
multipart and the one-by-one boot scan. S0 fixes the doc and is the smallest task here.

### S0 Correct the SMS persistence claim — firmware-architect, 10 min

**Read:** `firmware/main/msg.c:1732-1771` (`msg_insert_sms_in`), `msg.h:134-190` (the `msghist`
design note), `docs/ROADMAP.md:46-47`. **Files:** `docs/ROADMAP.md` only.
**Do:** replace "A received SMS lives in RAM only" with the truth: it is persisted to `msghist` like
any other message, **but** it is stored with `ts = 0` (`msg_insert_sms_in()` sets it deliberately, the
row is never published) so it renders with no clock time and sorts only by `msghist` seq. Keep the
"multipart texts arrive as separate messages" and "boot-time scan reads slots one by one" clauses —
those are real (`sms.c:1055-1060`, `sms.c:1117-1145`). **Verify:** doc-only.

### S1 Timestamp a received SMS — firmware-dev, small. **Worth building before the SIM question.**

**Read:** `msg.c:1732-1771`, `sms.c:1061-1115` (`process_inbound` already calls `net_get_clock()` for
the audit entry at `:1085`), `firmware/main/msg.h` (`msg_insert_sms_in` doc comment).
**Files:** `firmware/main/msg.c`, `msg.h`, `firmware/main/sms.c`, `firmware/host/test_msg.c`.
**Do:** give `msg_insert_sms_in()` an `int64_t ts` parameter and pass `process_inbound()`'s already-
fetched `sms_ts` (§3.5: 0 when there is no clock, which is the existing, documented "no time" value —
do not invent a sentinel). No wire, RTC or `msghist` format change; `ts` is already in the record.
**Verify:** `test_msg.c` — an SMS row inserted with a real ts round-trips through
`msghist_record_encode/_decode`; with ts 0 it behaves exactly as today. Host tests green, build clean.

### S2 Multipart (UDH) reassembly — pure logic first. **Worth building before the SIM question.**

**Read:** `firmware/main/sms.h` (module comment, the pure-section rules), `sms.c:552-650`
(`cmgr_split_fields`, `sms_parse_cmgr_header`, `sms_decode_received`), `sms.c:1055-1115`
(`process_inbound`, whose comment states today's "each part is its own message" rule),
`docs/V02_DESIGN.md` §6, `firmware/host/test_sms.c`.
**Files:** `firmware/main/sms.c`, `sms.h`, `firmware/host/test_sms.c`. **Device wiring is S3's job —
this task adds pure code and tests only, plus the header's UDH accessor.**
**Do:** two pure pieces, both under the `#ifdef ESP_PLATFORM` banner's *pure* side:
1. `bool sms_parse_udh(const uint8_t *tpdu_udh, size_t len, uint8_t *ref, uint8_t *total, uint8_t *part)`
   — decodes IEI 0x00 (8-bit reference, 5-byte IE) and IEI 0x08 (16-bit reference, 6-byte IE),
   rejecting anything else, `total == 0`, `part == 0`, `part > total`, and any length mismatch.
2. A bounded reassembly ring: `SMS_REASM_SLOTS 2`, each holding `ref`, `total`, a bitmap of parts
   seen, `SMS_BODY_MAX` of accumulated text, and a first-part monotonic timestamp.
   `int sms_reasm_add(sms_reasm_t *r, uint8_t ref, uint8_t total, uint8_t part, const char *text,
   size_t len, int64_t now_us, char *out, size_t out_cap, size_t *out_len)` returning
   `COMPLETE` / `PENDING` / `DROPPED`. **Window: 120 s** from the first part (a long enough gap that a
   normal two-part text always lands, short enough that a stalled slot never blocks the next message);
   `sms_reasm_expire(r, now_us)` drops and reports expired slots so the caller can log and, per
   §6's own "never silently lose a text" rule, deliver what it has as a partial rather than nothing.
   A third concurrent reference evicts the **oldest** slot. Monotonic `esp_timer_get_time()`, never
   wall clock (the same deviation `msg.h` documents for `pending_up.created_us`).
   Fixtures: at least one real two-part and one real three-part UDH from a genuine concatenated SMS
   (`05 00 03 <ref> 02 01` / `06 08 04 <ref16> 03 02`), out-of-order arrival, a duplicate part, a
   missing middle part that expires, and a 16-bit-reference case.
**Verify:** `make -C firmware/host test` green with the new cases. Build clean. **Note in the report,
do not act on it:** whether the Sequans modem in text mode (`AT+CMGF=1`, `WalterModem.cpp:4358`) even
*exposes* the UDH bytes is UNVERIFIED — see S3.

### S3 Wire multipart into the receive path — firmware-dev. **GATED on S4 and on the S4b experiment.**

**Read:** S2's helpers, `sms.c:1061-1115`, `sms.c:585-610` (`sms_parse_cmgr_header` — it currently
keeps only `<stat>,<oa>,<scts>,<dcs>` out of eleven fields; `<fo>` is **field 5** and its bit 6
(0x40) is UDHI), `firmware/components/dptechnics__walter-modem/src/WalterModem.cpp:2253-2290` (the
patched `+CMGR` parser), `WalterModem.h:2476-2500` (`net_sms_read_t`'s source).
**Files:** `firmware/main/sms.c`, `sms.h`, `firmware/main/net.h`/`net.cpp` if `<fo>`/UDH must be
surfaced, `firmware/components/.../PATCHES.md` if a vendor patch is needed.
**Do:** **only after S4b has answered this question**: in text mode this modem may deliver a
concatenated part with the UDH stripped (nothing to reassemble from, and the parts are
indistinguishable), or with the UDH as leading hex inside the body, or not at all. Parse `<fo>` in
`sms_parse_cmgr_header()` (cheap and useful either way), then take the branch S4b's evidence picks:
UDH visible ⇒ split it off the body and feed `sms_reasm_add()`, delivering one `msg_insert_sms_in()`
on COMPLETE. UDH stripped ⇒ **stop and report**; the alternative is PDU mode (`AT+CMGF=0`), which
means a full PDU decoder and a new vendor patch, and that is a separate decision by the owner, not
this task. **Verify:** host tests still green; the device path is verified by S5 step 4.

### S4 Bench: does this SIM carry SMS at all — bench-tester, owner at the glass. **Gates S3 and S5.**

**Read:** `docs/HARDWARE_TESTING.md` items "Console commands" and "Not yet seen working" item 8,
`firmware/main/sms.c:1005-1045` (`sms_init` and the exact log lines), `net.h:620-690`.
**Do (one flash of a current debug build, no code changes):**
- **(a) Does the modem accept SMS at all.** Boot and capture. `SMS ready: charset=… storage
  used/total=u/t` (`sms.c:1035`) ⇒ the modem's SMS stack is up. `SMS unavailable this boot` ⇒ one of
  `AT+CMGF=1 / AT+CSCS / AT+CSDH=1 / AT+CSMP / AT+CNMI=2,1,0,0,0 / AT+CPMS` was refused; find which
  by hand with `at AT+CMGF=1`, `at AT+CPMS?`, `at AT+CNMI=2,1,0,0,0` **only after `MQTT session
  usable`** (`HARDWARE_TESTING.md`'s standing rule) and report the exact error. `+CME ERROR: 4`
  (operation not supported) or `+CMS ERROR: 302` is the modem refusing; **`+CMS ERROR: 30x` /
  `+CME ERROR: 3` on `AT+CMGS`, or a send that returns OK but never arrives, is the SIM having no SMS
  entitlement** — that is the answer we are looking for.
- **(b) Can it receive.** `at AT+CSCA?` — an empty service-centre address is a strong "this SIM has no
  SMS". Then text the pager's number from a phone and watch for `+CMTI` in the trace and
  `sms received from …` (`sms.c:1103`). Silence for 5 minutes with a good `AT+CSCA?` ⇒ try the other
  SIM before concluding.
- **(c) Can it send.** `smstest +1<your number> hello` → expect `smstest: OK` plus `+CMGS: <mr>`, and
  the text actually arriving on the phone. `smslist` to confirm the audit entry.
- **(d) S4b, the multipart question, only if (b) worked.** Text the pager **a 200-character message**
  (forces two parts). Capture the raw `+CMGR:` header lines and the body lines verbatim. Report: how
  many `+CMTI` arrived; whether `<fo>` (field 5) has 0x40 set; and whether the body starts with the
  UDH bytes (`050003…`) or with the plain text. **That transcript is what unblocks S3.**
**Verify:** paste the exact log lines for (a)-(d). Update `docs/HARDWARE_TESTING.md` item 8 with the
verdict for this SIM (and which SIM), moving it to "Seen working" if it works.
**Depends on:** nothing. **Do this first; S3 and S5 are wasted work until it answers.**

### S5 One-pass boot scan (`AT+CMGL`) — firmware-dev. **GATED on S4(a).**

**Read:** `sms.c:1117-1145` (the one-index-per-call boot drain, `s_boot_drain_*`),
`sms.c:1005-1045` (`AT+CPMS` used/total feeding the scan bound),
`firmware/components/dptechnics__walter-modem/src/WalterModem.cpp:4358-4490` and
`WalterModem.h:6090-6215`, `firmware/components/dptechnics__walter-modem/PATCHES.md` (patch 1.4).
**Files:** `firmware/main/sms.c`, `firmware/main/net.h`/`net.cpp`, the vendor component +
`PATCHES.md`, `firmware/host/test_sms.c`.
**Do:** **finding, so no agent re-searches:** the vendor library has **no `AT+CMGL` at all** — patch
1.4 added only `CMGF`/`CSCS`/`CSDH`/`CSMP`/`CNMI`/`CPMS`/`CMGS`/`CMGR`/`CMGD`, and the `+CMGR` parser
is a single-record, two-line state machine (`WalterModem.cpp:1872-1880,2253-2290`). A one-pass
`AT+CMGL="ALL"` therefore needs a **new vendor patch**: a multi-record response region that emits one
callback per `+CMGL: <idx>,<stat>,<oa>,…` header + body pair, bounded by `SMS_BOOT_DRAIN_SAFETY_CAP`
records so a full 50-slot SIM cannot overrun the response buffer. Do this **only if S4(a) says SMS
works on this SIM**, and keep the existing one-index-per-call scan as the fallback when `AT+CMGL`
returns an error (§0's fail-open rule). Budget the win honestly before building: the current scan
costs one `AT+CMGR` per slot, one per `sms_service()` call, at ~2 wake cycles per slot — for a
typical `used=0..2` SIM that is **under a second of extra awake time at boot only**, i.e. well under
0.1 mAh/day. It is a latency and code-cleanliness win, not a power win; say so and let the owner
decide whether the vendor patch is worth it. **If the owner declines, close this task and delete the
`AT+CMGL` clause from `docs/ROADMAP.md:47`.**
**Verify:** pure `+CMGL` header/record splitting host-tested in `test_sms.c` against captured
fixtures from S4; on the bench, boot with 3 texts pre-loaded and confirm all 3 drain and the log
reports one pass. Vendor changes documented in `PATCHES.md` in the existing patch format.
