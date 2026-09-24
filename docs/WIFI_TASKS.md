# WiFi tasks

Companion to `docs/WIFI_DESIGN.md`, which holds the reasoning and the decisions; this file holds
only what an implementing agent needs. Each task names its agent, the files to read first, the files
it may touch, what to do, and how to verify. Bench tasks obey `docs/HARDWARE_TESTING.md` and the
bench rules in `.overnight-handoff.md` verbatim: one reader on the port, one hardware agent at a
time, bounded foreground captures. Coding agents never open `/dev/cu.usbmodem*`.

Build (firmware): `export PATH=<pyshim>:$PATH; . ~/src/esp/esp-idf/export.sh; cd firmware;
PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build`. Host tests: `make -C firmware/host test`
(every firmware task must leave them green). Web: `cd web && npm run lint && npm run build`.
Relay: `cd relay && .venv/bin/pytest`.

Order: W0 → W1 → W2 → W3 → W7 → W4 → W5 → **W6 (phase 1 milestone)** → W8 → W9 → W10 → W11 → W12.

**Amended 2026-09-23** after W6's first bench attempt (`build/bench-logs/phaseAG-wifi.log`;
reading in `docs/WIFI_DESIGN.md` §10): W0 never ran on this branch and its go/no-go ("stop if the
heap watermark falls below 40 kB") was never applied — the real numbers are now in §10.1/§10.3, and
they fail that test. Order becomes
**W14 (10 min, no hardware) → W6-attempt-2 → [W13 if W6's step 1 shows < 10 kB of margin] → W15 →
W8 → W9 → …**, with W13 promoted ahead of everything else if W6 step 1 fails outright.

W7 (relay) is deliberately early: `V02_DESIGN.md` §0's rule is that the relay accepts a new field
**before** any firmware that sends it is flashed.

**Phase 1 milestone = W6:** STA + esp-mqtt + manual selection from the console, benched on USB
power, before any policy automation and before any power-management work. If W6 fails, nothing
after it is worth building.

---

## W0. Measure what WiFi costs the binary — firmware-dev, timebox 1 h

**Read:** `firmware/main/CMakeLists.txt` (`REQUIRES` list), `firmware/sdkconfig.defaults`,
`firmware/partitions.csv`, `docs/WIFI_DESIGN.md` §8 (the flash/RAM estimates this task replaces
with real numbers).
**Files:** `firmware/main/CMakeLists.txt`, `firmware/sdkconfig.defaults`, a throwaway
`firmware/main/wifi_probe.c` that is deleted again before the commit.
**Do:** add `esp_wifi esp_netif esp_event mqtt esp-tls` to `REQUIRES`; in `wifi_probe.c`, reference
`esp_wifi_init`, `esp_mqtt_client_init` and `esp_netif_init` from a function `app_main` never calls,
so the linker pulls them in. Build. Record: `idf.py size` totals before and after, the app image
size against `0x200000`, and `esp_get_minimum_free_heap_size()` at boot before and after (log it
from `modes_boot()`). Then delete `wifi_probe.c` and revert `REQUIRES` if the numbers are bad.
**Verify:** a table in the task report: flash delta, app size and % of the 2 MB slot, heap
watermark delta. Go/no-go: **stop and report** if the app exceeds 1.6 MB or the heap watermark
falls below 40 kB. Host tests green; nothing else changes.

---

## W1. Selection policy state machine — firmware-dev

**Read:** `docs/WIFI_DESIGN.md` §2, `firmware/main/coverage.c` and `firmware/host/test_coverage.c`
(the exact pattern to copy: a pure struct + a `_step()` function returning an action enum, driven by
an injected `now_us`), `firmware/main/net_connect_guard.h`.
**Files:** new `firmware/main/wifi_policy.c` / `.h`, new `firmware/host/test_wifi_policy.c`,
`firmware/host/Makefile`, `firmware/main/CMakeLists.txt` (SRCS only).
**Do:** pure C, no ESP-IDF include, no I/O, no logging.

```c
typedef enum { WIFI_XPORT_LTE = 0, WIFI_XPORT_WIFI } wifi_xport_t;
typedef enum { WIFI_ACT_NONE = 0, WIFI_ACT_ASSOCIATE, WIFI_ACT_MQTT_UP,
               WIFI_ACT_DROP_TO_LTE, WIFI_ACT_SCAN } wifi_policy_action_t;
typedef struct { bool user_enabled, have_creds, ca_pinned, sta_associated, mqtt_up;
                 int rssi_dbm; } wifi_policy_in_t;
wifi_policy_action_t wifi_policy_step(wifi_policy_t *p, const wifi_policy_in_t *in, int64_t now_us);
wifi_xport_t wifi_policy_desired(const wifi_policy_t *p);
void wifi_policy_note_failure(wifi_policy_t *p, int64_t now_us);   /* assoc/DHCP/TLS/CONNECT */
void wifi_policy_note_disconnect(wifi_policy_t *p, int64_t now_us);
void wifi_policy_reset(wifi_policy_t *p);                          /* user toggle */
```

Constants as named `#define`s so they can be retuned without touching logic:
`WIFI_RSSI_ENTER_DBM -70`, `WIFI_RSSI_EXIT_DBM -80`, `WIFI_RSSI_EXIT_HOLD_S 30`,
`WIFI_FAIL_MAX 3`, `WIFI_FLAP_WINDOW_S 120`, `WIFI_FLAP_MAX 2`, `WIFI_ASSOC_LOST_S 20`,
`WIFI_RETRY_BACKOFF_S {600, 1800, 3600}`, `WIFI_STABLE_S 600`.
**Verify:** `firmware/host/test_wifi_policy.c` covers: never leaves LTE when `user_enabled` is
false; never leaves LTE when `ca_pinned` is false (design §5); enters WiFi at −70 and not at −71;
does **not** leave at −75 (hysteresis); leaves after −81 held 30 s but not at 29 s; three failures
drop to LTE; two disconnects in 120 s drop to LTE, two in 121 s do not; the 10/30/60 min backoff
ladder and its cap; a 10-minute good session resets the ladder; `wifi_policy_reset()` clears
everything. `make -C firmware/host test` green.

---

## W2. Credential store — firmware-dev

**Read:** `docs/WIFI_DESIGN.md` §4, `firmware/main/ident.c` (the NVS namespace/read/write pattern
and the "never log a secret" discipline), `firmware/main/sms.c`'s contact-list NVS store (the
closest existing shape: a small bounded list applied wholesale).
**Files:** new `firmware/main/wificred.c` / `.h`, new `firmware/host/test_wificred.c`,
`firmware/host/Makefile`, `firmware/main/CMakeLists.txt`.
**Do:** split with `#ifdef ESP_PLATFORM` exactly like `ident.c`/`lock.c`. Pure half: validation
(`wificred_valid_ssid()` 1–32 bytes, no NUL; `wificred_valid_psk()` 8–63 bytes) and an in-memory
`wificred_set_t` of up to 2 entries with replace-wholesale semantics. ESP half: NVS namespace
`wifi`, keys per design §4, plus `wificred_load()`/`wificred_store()`/`wificred_clear()` and
`wificred_note_channel(idx, ch)`.
**Hard rule:** no function in this module formats a PSK into a string, and no `ESP_LOG*` call in
this file takes a PSK as an argument. A reviewer must be able to `grep -n psk firmware/main/*.c`
and see nothing printable.
**Verify:** host tests for every validation boundary (0/1/32/33-byte SSID; 7/8/63/64-byte PSK;
embedded NUL; 3 entries refused; empty list clears). `make -C firmware/host test` green. Grep the
new file for `ESP_LOG` and confirm by eye that no secret reaches one; say so in the report.

---

## W3. `cfg.wifi` sub-map parser — firmware-dev

**Read:** `firmware/main/cfg.c` (whole file — `parse_cfg_submap()` and its `skip_capture()`
contract), `firmware/main/catrust.c`'s `catrust_parse_cfg_submap()` and
`firmware/host/test_catrust.c` (the exact pattern for a sub-map parser and its host test),
`docs/PROTOCOL.md` §10 sub-map keys, `docs/WIFI_DESIGN.md` §4.
**Files:** `firmware/main/cfg.c` (add `CFG_KEY_WIFI 3` and the dispatch), `firmware/main/cfg.h`,
`firmware/main/wificred.c/.h` (add `wificred_parse_cfg_submap()`),
`firmware/host/test_wificred.c`, `firmware/host/test_cfg.c`.
**Do:** `cfg.wifi = 3`, sub-map `{ en = 0 (bool), nets = 1 (array ≤2 of { s = 0, p = 1 }) }`.
`nets` absent → apply `en` only, leave stored networks untouched. `nets: []` → clear. Unknown keys
skipped, never a parse failure (`cfg.c`'s existing rule). Apply-and-ack-`shown` immediately on
success, exactly like `cfg.sms` (`cfg.c:161-165`). Do **not** touch the relay in this task.
**Verify:** host tests: a full push with 2 networks; `en` only; `nets: []`; 3 networks rejected
(whole push refused, not truncated); an oversize SSID rejected; an unknown sub-key ignored; a
malformed array rejected without reading out of bounds. `make -C firmware/host test` green.

---

## W4. The transport seam — a pure move — firmware-dev

**Read:** `firmware/main/net.h` in full, `firmware/main/net.cpp` (especially 327-500
`pager_mqtt_event_handler`, 1193-1290 session/publish, 1501-1585 `net_service_session`),
`firmware/main/modes.c` 2050-2320 (every `net_*` call site), `docs/WIFI_DESIGN.md` §1.
**Files:** new `firmware/main/net_xport.h`, new `firmware/main/xport_lte.cpp`,
`firmware/main/net.cpp`, `firmware/main/net.h`, `firmware/main/modes.c` (three lines only),
`firmware/main/CMakeLists.txt`.
**Do:** **no behaviour change at all.** Move the MQTT session code listed in design §1 out of
`net.cpp` into `xport_lte.cpp` unmodified, behind a `net_xport_ops_t` vtable with a single
`s_active_xport` (LTE always, in this task). Keep every comment; a reviewer must be able to diff the
moved block and see zero semantic edits. Add `net_xport_active()`, `net_set_lte_suppressed(bool)`.
In `modes.c`: (a) call `net_set_lte_suppressed(s_coverage_owns_radio || s_loc_suppress ||
s_ca_apply_suppress)` and call `net_service_session()` unconditionally; (b) nothing else.
**Verify:** `idf.py build` clean in both configurations; `make -C firmware/host test` green
(`test_net_connect_guard`, `test_publish_quiet` must still pass untouched); `git diff --stat` shows
the move; report the line count moved vs. the line count changed. **Then hand to bench-tester for a
30-minute regression on LTE only**: boot, session up, one page delivered, one reply sent, one
liveness ping observed in the log at ~300 s. No WiFi code exists yet — this task must be invisible
from the outside.

---

## W5. esp-mqtt over the WiFi station + console control — firmware-dev

**Read:** W4's `net_xport.h`, `firmware/main/net.cpp` 327-500 (the event-handler semantics to
mirror), `firmware/main/publish_quiet.h`, `firmware/main/net_connect_guard.h`,
`firmware/main/ident.h`, `firmware/main/catrust.h`, `firmware/main/main.c` (the REPL command
registration pattern), `docs/WIFI_DESIGN.md` §1 (the event-mapping table), §5, §3.
**Files:** new `firmware/main/wifi_sta.c/.h`, new `firmware/main/xport_wifi.c`,
`firmware/main/net.cpp` (`net_sleep()` transport branch only), `firmware/main/net.h`,
`firmware/main/main.c` (console), `firmware/main/CMakeLists.txt`.
**Do:**
1. `wifi_sta.c`: `esp_netif_init()` + `esp_event_loop_create_default()` (guard: may already exist)
   + `esp_wifi_init()` + STA config from `wificred`, `threshold.authmode = WIFI_AUTH_WPA2_PSK`,
   `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)` with `listen_interval = 10` in sleep mode and
   `WIFI_PS_MIN_MODEM` in active mode, `esp_wifi_set_max_tx_power(52)` (13 dBm, design §8). Expose
   associate/disassociate/RSSI/`wifi_ap_record_t` DTIM period/IP-acquired state. **No MQTT here.**
2. `xport_wifi.c`: `esp_mqtt_client_config_t` with `client_id = ident_get_dev_id()`,
   `username = ident_get_dev_id()`, `password = ident_get_mqtt_pw()`,
   `broker.address.hostname/port = ident_get_host()/ident_get_port()`,
   `broker.verification.certificate = ident_get_ca()`, `session.keepalive = 60`,
   `session.disable_auto_reconnect = true`, `buffer.size = 1024`, **no LWT** (design §1).
   Refuse to start unless `catrust_get_state() == CATRUST_PINNED`. Map events exactly per design
   §1's table; reuse `net_connect_guard` and `publish_quiet` unchanged; `mqtt_connected` is set only
   on `MQTT_EVENT_SUBSCRIBED`.
3. `net_xport_switch(to)`: tear the old transport's session down, wait ≤2 s for its DISCONNECTED,
   assert no client object remains, then bring the new one up. This is the only writer of
   `s_active_xport`.
4. `net_sleep()`: on `NET_XPORT_WIFI`, `vTaskDelay(ms)` and return — never
   `esp_light_sleep_start()` (design §3). Log once per transport change which branch is live.
5. Console (debug build): `wifi set <ssid> <psk>`, `wifi clear`, `wifi on`, `wifi off`,
   `wifi status`, `wifi scan`. `wifi status` prints SSID, RSSI, IP, DTIM period, active transport —
   **never the PSK**. No automatic selection in this task: `wifi on` means "use WiFi", `wifi off`
   means "use LTE".
**Verify:** builds clean both configurations; host tests green; `grep -rn "esp_light_sleep_start"
firmware/main/` shows exactly one call site. Report the `idf.py size` delta against W0's baseline.
**Last step, before W6:** implement and run the light-sleep-with-association experiment from
`WIFI_DESIGN.md` §9 as a debug-only console command `wifisleeptest`, and report its result — it
decides whether W9 is needed at all.

---

## W6. Phase 1 bench: first WiFi session, fallback, no double session — bench-tester

**Read:** `docs/HARDWARE_TESTING.md` (bench rules), `docs/WIFI_DESIGN.md` §7, `.overnight-handoff.md`,
`tools/bench/serial_capture.py`, `tools/bench/poll_emqx_client.py`,
`tools/bench/send_test_page.py`.
**Do:** one flash. Pager on USB power throughout (phase 1 draws ~38 mA — do not run this on the
battery). Four captures, each bounded and foreground:
1. `wifi set <ssid> <psk>`, `wifi on`. Capture the association → IP → transport switch → CONNECT →
   SUBSCRIBED → `/status online` sequence. Send a page; record the send→render latency.
2. Turn the AP off. Capture the fallback: 3 failures → `xport: WIFI -> LTE` → LTE session → page
   delivered over LTE. Turn the AP back on; confirm nothing switches back for 10 minutes.
3. `poll_emqx_client.py` every 5 s across both switches. **Acceptance: `/clients/{id}` never shows
   two entries, and `connected_at` changes exactly once per switch.**
4. `wifisleeptest` (W5's last step): report pass/fail for all ten sleep cycles and whether the page
   sent during cycle 6 arrived.
**Verify:** the four captures saved under `build/bench-logs/` with the date in the name; a summary
table (latency on each transport, `link` values seen, EMQX client count) added to
`docs/HARDWARE_TESTING.md` "Seen working". Report any log line that prints a PSK as a **blocker**.

### W6, amended after the first attempt (2026-09-23)

The first attempt is `build/bench-logs/phaseAG-wifi.log` / `phaseAG-wifioff.log` (debug image
`ea05271`). It got further than expected and then failed for a reason that was not TLS: association
in 1.4 s, WPA3-SAE, DTIM 1, DHCP at +4.4 s, `xport: LTE -> WIFI`, then
`mbedtls_ssl_setup returned -0x7F00` (`MBEDTLS_ERR_SSL_ALLOC_FAILED`) — a heap failure. See
`docs/WIFI_DESIGN.md` §10 for the full reading. What W6 must do differently:

**Prerequisites before the next W6 run (all three, in order).**
1. **Regenerate the config.** `rm firmware/sdkconfig && idf.py reconfigure`. An existing
   `sdkconfig` overrides `sdkconfig.defaults`, which is why W5's own `SOFTAP_SUPPORT=n` /
   `WS_TRANSPORT=n` were never in the phaseAG image, and why §10.3's mbedtls/WiFi buffer values
   would not be either. **Paste the seven changed values out of the regenerated file into the
   report** — if they are not there, the run measures nothing.
2. W13 (below) is **not** a prerequisite for a first green W6, but it is a prerequisite for the
   ≥ 40 kB headroom the design asks for. If step 4 below shows a margin under 10 kB, stop and do
   W13 before calling W6 passed.
3. Build the **debug** image (`PAGER_DEBUG_NO_LIGHT_SLEEP=1`): the `wifi` console commands only
   exist there, and in a release build the whole WiFi/TLS/MQTT tree is garbage-collected out
   (`WIFI_DESIGN.md` §10.1). Do not try W6 on a release image.

**Amended acceptance sequence.** One flash, USB power throughout, four bounded foreground captures.

1. **Heap before anything else.** `wifi set <ssid> <psk>`, then `wifi on`. Record, in order:
   - `modes: heap at boot: free=… minimum_free=…`
   - `wifi_sta: wifi: station started …; heap free=… largest_8bit_block=…` ← new
   - `net: pre-TLS heap: free=… largest_8bit_block=…` ← new, one per connect attempt
   **Acceptance A (RAM):** the pre-TLS `largest_8bit_block` is **> 8,493 B** (that is
   `MBEDTLS_SSL_IN_CONTENT_LEN=8192` + ~300 B of record overhead) **and** `free` is **> 30 kB**.
   Report the three numbers as a table against `WIFI_DESIGN.md` §10.3's predictions (driver-up free
   ≈ 38.6 kB, handshake peak ≈ 27–32 kB). If A fails, W6 stops here and W13 runs — no amount of
   retrying fixes a contiguous-block shortage.
2. **First session.** Expect `MQTT connected, resubscribing to …` → `MQTT session usable
   (subscribed to 'pager/<id>/down')` → `published /status online` with `xport` = `wifi`. Send a
   page; record send→render latency (design §7 step 1 expects < 2 s).
3. **The failure path, on purpose** — this is the part the first attempt could not reach and the
   part the two 2026-09-23 fixes exist for. Easiest trigger with no AP fiddling: `wifi on` while
   the broker is unreachable (block 8883 on the AP, or `wifi set` a good SSID and point
   `ident`'s host at a dead one on a scratch device). Expect, and **assert on the absence of the
   old behaviour**:
   - `net: MQTT disconnected, rc=… (connect-fail streak=1)`
   - `modes: MQTT session lost (rc=…), transient - retrying with backoff` — and **never**
     `modes: TLS handshake failed while pinned`, **never** `SECURITY tls-broken`, and `/status`
     `tls` still `pinned` afterwards. (A WiFi TLS failure must not touch the modem's CA state:
     `WIFI_DESIGN.md` §10.4 item 1.)
   - retries at **~5 s, then ~15 s** — count the `retrying MQTT session (backoff idx=…)` lines:
     **at most one per attempt**, with `idx` incrementing. More than ~5 lines in 60 s is a
     regression of the storm (the first attempt logged 530+ in 75 s).
   - `net: WiFi MQTT client is up but not connected and no connect in flight - destroying it and
     issuing a fresh connect` once per retry.
   - after the third failure: `net: WiFi MQTT failed 3 times in a row - giving the session back to
     LTE`, then `xport: WIFI -> LTE`, then an LTE session usable within ~5 s, and a page delivered
     over LTE. Whole episode should be **25–45 s** from `wifi on`.
   - **Also time the main loop**: `wifi_down()` can block its caller up to ~5 s per retry
     (`WIFI_DESIGN.md` §10.4). Note the largest gap between consecutive `modes:` lines during the
     episode; if it exceeds 6 s, say so — that is the trigger for the
     `esp_mqtt_client_reconnect()` variant.
4. **No double session.** Unchanged from the original W6 step 3: `poll_emqx_client.py` every 5 s
   across both switches; `/clients/{id}` never two entries; `connected_at` changes exactly once per
   switch. The first attempt passed this (the broker showed exactly one client — the LTE one —
   throughout).
5. **`wifisleeptest`.** Unchanged from the original W6 step 4. Note that this AP's **DTIM is 1**,
   not 10, so the §3 row this decides between is 40.1 mA (no PM) vs 2.45 mA (auto light sleep).
6. **`wifi status`** still prints `dtim: n/a`; the real value is in the driver's own
   `AP's beacon interval = … DTIM period = …` line. Report it from there. Not a blocker.

---

## W7. Relay: `xport`, and the `cfg.wifi` push — backend-dev

**Read:** `relay/app/wire.py` (the `/status` model and its validators, ~200-270),
`relay/app/wirecbor.py` (`KEYMAP`, `CFG_KEYMAP`, `CA_KEYMAP` — the allocation pattern),
`relay/app/ingest.py:600-670` (the `link`/`session` re-publish edge), the `cfg.sms` push route and
its validator (the closest existing shape), `docs/WIFI_DESIGN.md` §4 and §6, `docs/PROTOCOL.md` §10.
**Files:** `relay/app/wire.py`, `relay/app/wirecbor.py`, `relay/app/ingest.py`, the admin/device
router that owns `sms-contacts`, `relay/tests/`, `docs/PROTOCOL.md` (§5.1 table, §10 keymap and
sub-map sections — **edit the protocol doc first, then the code**).
**Do:**
1. `/status` accepts optional `xport` (`lte` | `wifi`), **CBOR key 52**; persist on `devices/{id}`.
   No logic change: a transport switch already bumps `link`, which `ingest.py` already treats as a
   re-publish edge. Do not add a second edge condition.
2. `CFG_KEYMAP` gains `wifi: 3`; a new `WIFI_KEYMAP = {"en": 0, "nets": 1}` and
   `WIFI_NET_KEYMAP = {"s": 0, "p": 1}`.
3. `GET/PUT /api/devices/{id}/wifi` (owner or admin): `{en: bool, nets: [{s, p}]}`, ≤2 entries,
   SSID 1-32 bytes, PSK 8-63 bytes. PUT stores and pushes a `cfg` exactly like `sms-contacts` does.
   **Guard:** refuse a PUT that carries `nets` when the device's last `/status` reported
   `tls` other than `pinned` — 409 with a message naming the reason (`WIFI_DESIGN.md` §4). `en`
   alone is always allowed.
4. GET never returns a stored PSK; it returns `{s, set: true}`.
**Verify:** `.venv/bin/pytest`: `xport` accepted in both encodings and ignored when absent; an older
`/status` without it still validates; the `cfg.wifi` round-trips through `wirecbor` under the 640 B
cap with 2 maximal entries; the `tls != pinned` guard returns 409; GET never leaks a PSK
(assert on the response body). Ruff clean. `PROTOCOL.md` edited before the code.

---

## W8. Web: WiFi panel and transport chip — web-dev

**Read:** the device page's SMS-contacts editor (the shape to copy), `web/lib/types.ts`,
`docs/WIFI_DESIGN.md` §4, W7's API.
**Files:** the device page component, `web/lib/types.ts`, a new small `WifiPanel` component.
**Do:** a *WiFi* panel on the device page: an on/off toggle (`en`), up to two rows of SSID + PSK
(PSK is a write-only password field showing `••••••` when set, never a value from the server), Save,
and a Clear. Show the 409 from W7's guard as the sentence it returns ("this device is not reporting
a verified TLS connection; push a CA first"), not a generic error. Add a transport chip beside the
existing TLS chip: `WiFi` / `LTE` / nothing when `xport` is absent.
**Verify:** `npm run lint && npm run build`; by hand against a dev relay: toggle, save two networks,
see the chip change after the pager's next `/status`. Tolerant of `xport` being absent (older
firmware).

---

## W9. Battery viability: `CONFIG_PM_ENABLE` + automatic light sleep — firmware-dev

**Only if W5's `wifisleeptest` failed.** If manual light sleep kept the association, do the small
`net_sleep()` change instead and skip to W10.

**Read:** `docs/WIFI_DESIGN.md` §3, `firmware/main/net.cpp:1315-1375` (`net_sleep()`'s RTS
choreography — this is what `PM_ENABLE` replaces), `firmware/main/modes.c:1764-1905` (the
`skip_sleep` logic), `~/src/esp/esp-idf/docs/zh_CN/api-guides/low-power-mode.rst` (the
recommended-configuration table), ESP-IDF's `esp_pm.h` and `esp_sleep_enable_uart_wakeup()`.
**Files:** `firmware/sdkconfig.defaults`, `firmware/main/net.cpp`, `firmware/main/modes.c`,
`firmware/main/wifi_sta.c`, a new `firmware/main/pm_lock.c/.h`.
**Do:** `CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_FREERTOS_HZ=1000`,
`esp_pm_configure(max 160 / min 40 / light_sleep_enable = true)` **applied only while the WiFi
transport is active** — LTE keeps today's manual path, selected at runtime, so the two can be A/B'd
on one flash (`.overnight-handoff.md`'s rule). Hold an `ESP_PM_NO_LIGHT_SLEEP` lock whenever
`net_modem_busy()` or `net_connect_in_flight()` is true, around every AT transaction, and around
every e-paper refresh and CardKB read. Arm `esp_sleep_enable_uart_wakeup()` on the modem UART.
**Verify:** builds clean; host tests green; then **W10**. Do not merge on a build alone — this
change can only be judged on a current trace and a delivery test.

---

## W10. Power and data measurement — bench-tester, owner reads the trace

**Read:** `docs/WIFI_DESIGN.md` §3 and §7 step 4-5, `docs/HARDWARE_TESTING.md`, W6's captures.
**Do:** one flash, battery rail instrumented. Six 10-minute windows, in this order, with the A/B
flag flipped at runtime rather than re-flashing: LTE idle; WiFi idle phase 1 (no PM); WiFi idle
phase 2 (PM on); WiFi with one page delivered; one single WiFi publish (peak current, 1 s window);
one WiFi scan (design §3b's per-scan figure). Then an hour on each transport with
`tools/bench/poll_emqx_loop.py` for the byte counters.
**Verify:** a table of measured average mA and mAh/day per window against `WIFI_DESIGN.md` §3's
predicted rows, with the deltas called out; the measured per-PINGREQ and per-scan energy;
the measured LTE bytes/hour saved. Update `WIFI_DESIGN.md` §3 in place, replacing `(estimate)` with
the measured number and its date. **Go/no-go for WiFi on battery: the phase-2 idle window must come
in under 2.5 mA.**

---

## W11. Phase 2: trigger-driven discovery scan — firmware-dev

**Do not start before W10 passes.**

**Read:** `docs/WIFI_DESIGN.md` §3b, `firmware/main/net.h`'s `net_set_cell_change_cb()` and
`firmware/main/loc.c`'s 10-minute cell-change debounce, `firmware/main/accel.c` (the motion
trigger), W1's policy module.
**Files:** `firmware/main/wifi_policy.c/.h` (the scan scheduler, pure), `firmware/main/wifi_sta.c`
(the scan call), `firmware/main/wificred.c` (the cached channel), `firmware/host/test_wifi_policy.c`.
**Do:** add `wifi_policy_scan_due(p, in, now_us)` — pure, host-tested — implementing: scan on a
serving-cell change or on motion-settle, at most **one per 10 minutes**; plus a **15-minute**
single-channel fallback while `user_enabled && have_creds && !sta_associated`. Use a **passive**
scan on the cached home channel (design §3b: no probe request, no TX burst); fall back to an
all-channel passive scan at most once an hour when the cached channel has failed twice. Default
**off**: the knob is `cfg.wifi.scan_s` (0 = never, the default; otherwise the fallback interval in
seconds), added to W3's parser and W7's API.
**Verify:** host tests for the debounce, the rate limit, the interval, the cached-channel fallback
and `scan_s = 0` meaning never. Then bench: turn `scan_s` on, walk the pager out of range and back,
confirm it re-associates within one interval, and measure the scan's real cost (one more window in
W10's shape). **Acceptance: the scan adds less than 10% to the measured idle budget** — the design
predicts 1-2%.

---

## W12. Documentation and the gotcha — docs-writer

**Read:** W6's and W10's reports, `docs/GOTCHAS.md`, `docs/OVERVIEW.md`, `docs/ROADMAP.md`.
**Do:** add an OVERVIEW paragraph ("the pager can carry the same MQTT session over WiFi when the
owner turns it on; LTE stays the default and the fallback"); add whatever W6/W10 found the hard way
to `GOTCHAS.md` in that file's symptom-first shape; record the measured numbers in
`WIFI_DESIGN.md` §3 and the open items in `ROADMAP.md`.
**Verify:** no number in the docs is left as `(estimate)` when a measured one exists; every
remaining `UNVERIFIED` names its experiment.

---

## W13. Give 31 kB of `.bss` back to the heap — firmware-dev

**Why:** `docs/WIFI_DESIGN.md` §10.3. `.dram0.bss` is **198,768 B** and the whole dynamic heap is
**132,684 B**; the TLS handshake needs ~30 kB of that with one contiguous 8.5 kB block. The
`sdkconfig` changes alone leave a ~7–12 kB margin. This task is what turns that into ~37–42 kB, and
it hands the LTE-only release image the same 31 kB. Two items, no third.

**Read:** `firmware/main/msg.c:700-760` (`msghist_restore()` and its `decoded[]`),
`firmware/main/ident.h:12-40` (the "no dynamic allocation, one static `ident_t`" module contract —
this task does not change that contract, it stops four *copies* of it existing),
`docs/WIFI_DESIGN.md` §10.3.
**Files:** `firmware/main/msg.c`, `firmware/main/ident.c`, `firmware/main/ident.h`,
`firmware/main/modes.c` (one function), `firmware/main/setup.c` (one function),
`firmware/main/catrust.c` (two functions), `firmware/host/` tests that touch either.
**Do:**
1. `msg.c:712` `static msg_t decoded[MSG_THREAD_DEPTH]` — **13,056 B** for a buffer used once,
   inside the boot-time history restore. `heap_caps_malloc()` it at the top of that function and
   free it on every exit path, including the early-return ones. If the allocation fails, skip the
   restore and log it: a pager with no scrollback still delivers pages, and this runs at boot when
   the heap is at its emptiest anyway.
2. The five static `ident_t` scratch copies, **4,460 B each = 22,300 B**: `ident.c:133` (`tmp`),
   `modes.c:489` (`snap`), `setup.c:752` (`id`), `catrust.c:405` (`snap`), `catrust.c:514` (`snap`).
   Each is a scratch buffer inside one function; every one is on the main or console task and none
   is reentrant with another. Add **one** shared scratch in `ident.c` behind
   `ident_t *ident_scratch(void)` and use it in all five places. Keep **one** of them (the
   `ident.c` one is the natural home), so this is +17.8 kB, not +22.3 kB.
   **Guard the sharing, do not assume it:** a `static bool s_scratch_busy` with an
   `assert(!s_scratch_busy)` on entry and a clear on exit, so a future caller that breaks the
   non-reentrancy assumption fails loudly in a debug build instead of corrupting an identity.
**Do not touch:** `disp.c:57`/`disp.c:64` (`s_fb_old`/`s_fb_snap`, 9,472 B) — those are the e-paper
diff planes and they are load-bearing. `xport_lte.cpp:210` `s_mqtt_rx_buf` — hot path.
**Verify:** `make -C firmware/host test` green (21 suites). `xtensa-esp32s3-elf-size -A
build/school_pager.elf` before and after: report the `.dram0.bss` delta and the `heap at boot:
free=` delta from one boot log — **acceptance: `.dram0.bss` down by ≥ 30,000 B and the boot heap up
by the same, with no change in behaviour**. Then a 30-minute LTE-only regression (boot, session up,
one page, one reply, history restored across a reboot with the scrollback intact) — item 1 touches
the msghist restore path, so a reboot with a non-empty thread is mandatory, not optional.
**Next tier, only if a measured margin is still under 20 kB:** `cafetch.c:495` `s_parser` 4,644 B,
`setup.c:630 s_bundle_buf` + `setup.c:741 plain` 8,192 B, `catrust.c:320 s_apply_pem` +
`setup.c:772 fetched_pem` 8,194 B. All three are "live during one operation only".

---

## W14. Does EMQX honour `max_fragment_length`? — 10 minutes, no hardware

**Why:** if it does, `MBEDTLS_SSL_IN_CONTENT_LEN` stops mattering at any static footprint and W13's
urgency drops. `docs/WIFI_DESIGN.md` §10.3 lists this as **UNVERIFIED**.
**Do:** on the laptop, `openssl s_client -maxfraglen 512 -connect <broker>:8883 -servername
<broker> </dev/null 2>&1 | head -40`, and separately `-msg` to see whether the ServerHello echoes
the `max_fragment_length` extension. Also record the served chain's total DER size
(`openssl s_client -showcerts`) — that is the number `IN_CONTENT_LEN` actually has to clear.
**Verify:** one paragraph in `WIFI_DESIGN.md` §10.3 replacing the `UNVERIFIED`, with the measured
chain size. If the extension **is** honoured, propose the `esp_tls` knob that sets it (there may not
be one on IDF 5.2 — say so rather than assuming) and leave `IN_CONTENT_LEN=8192` alone until there
is.

---

## W15. `catrust_on_mqtt_connected()` is transport-blind too — firmware-dev, small

**Why:** the mirror image of `WIFI_DESIGN.md` §10.4 item 1, found by inspection, **not yet observed
on hardware**. `modes.c:2390` calls `catrust_on_mqtt_connected()` on whichever transport just became
usable. On WiFi that resets `s_validated_fail_streak` and, if the state is `broken`, heals it back to
`pinned` — i.e. a successful **ESP32** handshake would silently declare the **modem's** pinned CA
good again and publish `tls: "pinned"`. Same rule as §5: an outcome on one transport is not evidence
about the other. Related, lower risk: `modes.c:2491` calls `catrust_before_reconnect()` on every
retry regardless of transport, which is a no-op unless the state is `broken` but can issue an
`AT+SQNSPCFG` on the modem for a **WiFi** reconnect.
**Files:** `firmware/main/catrust.c` only (`modes.c` is not the right place — the guard belongs next
to the side effects it protects: an NVS write, a modem AT command and a `/status` field).
**Do:** in `catrust.c`, make `catrust_on_mqtt_connected()` and `catrust_before_reconnect()` return
early when `net_xport_active() != NET_XPORT_LTE`, each with a one-line comment naming
`WIFI_DESIGN.md` §5 and §10.4. Do **not** add a guard to `catrust_on_mqtt_tls_fail()`: the
2026-09-23 fix already stops WiFi reporting the class that reaches it, and a second guard for the
same path is the "defensive code nobody verified" this project deletes at review time.
**Verify:** `make -C firmware/host test` green; then on the bench, with the device deliberately in
`tls: "broken"` (an LTE-side CA mismatch), bring WiFi up successfully and confirm `/status` still
reports `broken` and the log does **not** say `TLS validated again: state broken -> pinned`.
Sequence it after a green W6 — it needs a working WiFi session to test at all.

---

## W16. `wifi_sta_start()` / `wifi_sta_stop()` are not a clean cycle — firmware-dev, small

**Why:** the 2026-09-23 §2 fallback (`xport_wifi.c`'s `wifi_service_session()`) makes
stop-then-start a routine path, not just something the console does by hand. Two defects by
inspection of `firmware/main/wifi_sta.c`, **neither observed on hardware yet** — the second one is
the RAM-budget-relevant one:

1. `wifi_sta.c:96-99` registers the WIFI_EVENT and IP_EVENT handler instances inside
   `wifi_sta_start()`, which runs again after every `wifi_sta_stop()` (`s_started` goes false, but
   nothing unregisters). `esp_event_handler_instance_register()` does not de-duplicate, so the
   second `wifi on` gets two instances of `wifi_event_handler` and every association/IP event is
   handled twice. Harmless today (idempotent flag writes and a duplicated log line) and unbounded in
   principle. Fix: a `static bool s_handlers_registered` guard, or unregister in `wifi_sta_stop()`.
2. `wifi_sta_stop()` (`wifi_sta.c:132-144`) calls `esp_wifi_stop()` but never `esp_wifi_deinit()`,
   and never destroys the netif. `esp_wifi_init()` returns `ESP_OK` immediately when already
   initialised (`esp-idf/components/esp_wifi/src/wifi_init.c:254-256`), so the cycle *works* — but
   **the driver's ~16 kB of static RX buffers, its 6,656 B task and the netif stay allocated for the
   rest of the boot**. The radio is off, so there is no power cost; the cost is that the LTE-only
   heap never comes back after WiFi has been started once, which is exactly the headroom
   `WIFI_DESIGN.md` §10.3 is counting. Fix: `esp_wifi_deinit()` (legal after `esp_wifi_stop()`) and
   `esp_netif_destroy_default_wifi(s_netif)` + `s_netif = NULL` in `wifi_sta_stop()`, with the
   handler unregistration from item 1 so the ordering is defined.
**Files:** `firmware/main/wifi_sta.c` only.
**Verify:** host tests green (this file has none — say so). Then on the bench:
`wifi on` → `wifi off` → `wifi on` → `wifi off`, capturing the two new heap lines
(`wifi: station started …; heap free=…`) each time. **Acceptance: the free-heap number after the
second `wifi on` matches the first within ~2 kB, and the number after `wifi off` returns to within
~5 kB of the boot value** — i.e. the driver's memory is genuinely released. Also confirm each
`got ip` line appears exactly once per association, not twice.
