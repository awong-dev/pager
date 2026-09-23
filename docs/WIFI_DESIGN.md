# WiFi as an alternate MQTT transport

**Status:** design, nothing implemented. Companion task file: `docs/WIFI_TASKS.md`.
**Goal:** carry the same MQTT session — same broker, same client id, same credentials, same four
topics — over the ESP32-S3's own WiFi station instead of the Sequans modem, so that **the relay and
the web app need no change**. LTE-M stays the default and the fallback.
**Owner framing (2026-09-23):** "wifi is a cost-saving option. It is okay to require the user
actively turn it on instead of searching." The saving being bought is **LTE-M data and SIM cost**,
not latency and — see §3 — not obviously battery either. Every number below says which.

Conventions follow `PROTOCOL.md`: `(estimate)`, `(IDF doc)` = a figure read out of
`~/src/esp/esp-idf/docs` on this machine, `UNVERIFIED` = a hardware fact with a named experiment.

---

## 0. The three facts this design turns on

1. **WiFi is not free on battery.** ESP-IDF's own ESP32-S3 numbers
   (`docs/zh_CN/api-guides/sleep-current/esp32s3_summary.inc`, included by the English
   `low-power-mode.rst`, which is untranslated): an associated station at 160 MHz costs
   **40.1 / 38.7 / 38.2 mA** (DTIM 1 / 3 / 10) in plain Modem-sleep, **20.7 / 19.9 / 19.5 mA** with
   DFS, and **2.45 / 1.33 / 0.93 mA** with *automatic* Light-sleep. Only the third column is
   survivable: 38 mA on a 1500 mAh cell is **1.6 days**. Automatic light sleep needs
   `CONFIG_PM_ENABLE`, which is **off today** (`firmware/sdkconfig:918`).
2. **Manual light sleep and an associated station are mutually exclusive** as ESP-IDF documents it:
   "Wi-Fi connections are not maintained in Deep-sleep or Light-sleep mode"
   (`docs/en/api-reference/system/sleep_modes.rst:49-51`); the supported way to keep a connection
   is Modem-sleep + *automatic* light sleep. `net_sleep()` (`firmware/main/net.cpp:1315`) is a
   manual `esp_light_sleep_start()`. **UNVERIFIED:** whether a manual light sleep with
   `esp_sleep_enable_wifi_wakeup()` and `ESP_PD_DOMAIN_MODEM` kept on preserves the association in
   practice on IDF 5.x. Experiment in §9, 30 minutes on the bench; it decides whether task W9 is a
   day or a week.
3. **WiFi removes the two ugliest parts of the LTE path.** esp-mqtt sends real PINGREQs, so
   `V02_DESIGN.md` §9's 300 s re-SUBSCRIBE (≈28.8 mAh/day, ≈49 kB/day) is not needed; and there is
   no RTS hold-off, so the 200 ms wake-and-drain window (≈1.6 mA) is not needed either. Delivery
   latency on WiFi is ~1 s, not 27 s.

---

## 1. Transport seam

`net.h` stays the API `modes.c` sees. **No function in `net.h` gains a transport parameter.** The
dispatch is internal, on a single `s_active_xport` variable that only one function writes.

**Split the MQTT session out of `net.cpp`, do not rewrite it.** New files:

| File | Contents |
|---|---|
| `net_xport.h` | `typedef struct { bool (*up)(void); void (*down)(void); bool (*publish)(const char*, uint8_t*, uint16_t, uint8_t); void (*service)(void); void (*status)(net_mqtt_status_t*); ... } net_xport_ops_t;` plus `net_xport_t { NET_XPORT_LTE, NET_XPORT_WIFI }` |
| `xport_lte.cpp` | **moved verbatim** from `net.cpp`: `net_session_up/down`, `net_publish*`, `pager_mqtt_event_handler`, `net_service_session`, `classify_mqtt_rc`, the `s_resub_*`/`s_connect_guard`/`publish_quiet` state. Same code, new home, one review diff that is a pure move. |
| `xport_wifi.c` | esp-mqtt over `esp_tls`/mbedtls on the STA. Plain C. |
| `wifi_sta.c/.h` | `esp_netif` + `esp_wifi` bring-up, association, RSSI, scan. No MQTT. |
| `wifi_policy.c/.h` | **pure** selection state machine (§2). Host-tested. |
| `wificred.c/.h` | **pure** parser + NVS store (§4). Host-tested. |

**Stay in `net.cpp`, LTE-only, untouched:** `net_init`, `net_recover_modem`, `net_check`,
`net_get_clock`, `net_get_battery_mv`, `net_get_rssi`, `net_sleep` (see below), every `net_gnss_*`,
`net_radio_*`, `net_sms_*`, `net_get_cell_info`, `net_ca_fetch_*`, `net_write_ca*`,
`net_tls_configure`, `net_bootstrap_*`, `net_check_sim`. WiFi never touches the modem.

**Dispatched through the seam (signatures unchanged):** `net_session_up`, `net_session_down`,
`net_publish`, `net_publish_raw`, `net_get_mqtt_status`, `net_ack_disconnect_edge`,
`net_ack_session_restart_edge`, `net_service_session`, `net_modem_busy`, `net_connect_in_flight`,
`net_connect_fail_streak_maxed`, `net_take_memfull_delta`, `net_take_oversize_delta`,
`net_publish_quiet_wait_ms`. `net_set_msg_cb` registers one callback that both transports call.

**New, small:** `net_xport_t net_xport_active(void)` (for `/status`), `bool net_wifi_set_enabled(bool)`,
`void net_set_lte_suppressed(bool)`.

**What `modes.c` must not know:** which transport is up, TLS profiles vs mbedtls, keepalive vs
re-SUBSCRIBE, association state, DHCP, scan. Its only edits are three lines:

1. `/status` gains `xport` (§6).
2. The `net_service_session()` call site (`modes.c:2305`) drops its
   `!s_coverage_owns_radio && !s_loc_suppress && !s_ca_apply_suppress` guard and instead calls
   `net_set_lte_suppressed(...)` with the same expression. Those three suppressions are about the
   *modem*; they must not silence the WiFi transport's service tick.
3. `net_sleep(interval_ms)` is unchanged at the call site but becomes transport-aware inside (§3).

**LTE semantics stay exactly as they are** when `s_active_xport == NET_XPORT_LTE`: the connect
guard, the 30 s no-SUBACK death detector, the 300 s re-SUBSCRIBE, `s_disconnect_edge`,
`s_session_restart_edge`. Nothing in `xport_lte.cpp` changes behaviour.

### esp-mqtt equivalents

| LTE mechanism | WiFi equivalent |
|---|---|
| `PAGER_MQTT_KEEPALIVE_S` 480 s, modem sends no PINGREQ | `esp_mqtt_client_config_t.session.keepalive = 60`, `disable_keepalive = false`. esp-mqtt really does ping. 60 s also survives any home NAT. |
| 300 s raw re-SUBSCRIBE liveness ping | **not implemented.** `service()` on WiFi only runs the policy tick and the connect timeout. |
| `net_connect_guard` 30 s "no CONNECTED/SUBSCRIBED" | keep it, verbatim — same struct, armed on `esp_mqtt_client_start()`, cleared on `MQTT_EVENT_SUBSCRIBED`. |
| `WALTER_MODEM_MQTT_EVENT_CONNECTED` → subscribe from the handler | `MQTT_EVENT_CONNECTED` → `esp_mqtt_client_subscribe(pager/{id}/down, 1)`. Same "connected is not usable, SUBSCRIBED is" rule: `mqtt_connected` is set only in `MQTT_EVENT_SUBSCRIBED`. |
| `NET_MQTT_RC_TRANSIENT` | `MQTT_EVENT_DISCONNECTED`; `MQTT_ERROR_TYPE_CONNECTION_REFUSED` with `connect_return_code` 0x01/0x02/0x03. |
| `NET_MQTT_RC_PERMANENT` | `connect_return_code` 0x04 (bad user/pass) / 0x05 (not authorized) — the `AUTH`/`ACL_DENIED` analogue. |
| `NET_MQTT_RC_TLS_FAIL` | `MQTT_ERROR_TYPE_TCP_TRANSPORT` with a non-zero `esp_tls_stack_err`/`esp_tls_cert_verify_flags`. Feeds `catrust_on_mqtt_tls_fail()` **only to report**, never to un-validate (§5). |
| `s_disconnect_edge` / `session_restart_edge` | identical flags, set from the esp-mqtt event handler. A WiFi reassociation that survives the TCP session is invisible; one that does not raises an ordinary disconnect edge. No "silent resume" case exists — esp-mqtt never reconnects behind our back because **`disable_auto_reconnect = true`**: `modes.c`'s F1/F3 backoff stays the single owner of reconnect policy, which is also what keeps two sessions from ever existing. |
| `publish_quiet` gate (display corruption) | same gate, armed in `publish()`, cleared on `MQTT_EVENT_PUBLISHED`. WiFi TX bursts are the same class of supply event as LTE ones (§8). |
| oversize drop (`> 640 B`) | esp-mqtt fragments payloads larger than `buffer.size`. Set `buffer.size = 1024` so a 640-byte envelope is never split; if `total_data_len != data_len` anyway, count it as oversize and drop, exactly like `net.cpp:455`. |

**LWT:** the modem cannot set one (`PROTOCOL.md` §6.1); esp-mqtt can. **Do not set it in phase 1.**
An LWT from a dropped WiFi session arrives at the broker's keepalive timeout (~90 s) and would
overwrite a newer retained `online` published from LTE. Making it safe needs a relay-side ordering
rule on `/status`; that is a separate change, recorded here, not taken.

---

## 2. Selection policy

Pure state machine in `wifi_policy.c`, no I/O, host-tested. Inputs: `user_enabled`, `have_creds`,
`sta_associated`, `rssi_dbm`, `now_us`, `wifi_fail_count`, `lte_session_ok`. Output: a desired
transport plus one action (`SCAN`, `ASSOCIATE`, `MQTT_UP`, `SWITCH_TO_LTE`, `NONE`).

- **Default is LTE.** WiFi is never preferred unless `user_enabled` is true (console, device menu,
  or `cfg.wifi.en`, §4). No background discovery at all in phase 1.
- **Prefer WiFi** when: `user_enabled && have_creds && sta_associated && rssi >= -70 dBm` and the
  CA is pinned (§5).
- **Fall back to LTE** on any of: 3 consecutive failures of {association, DHCP, TLS handshake, MQTT
  CONNECT}; 2 MQTT disconnects within 120 s; `rssi < -80 dBm` continuously for 30 s; loss of
  association for 20 s.
- **Hysteresis.** Entry needs −70 dBm, exit needs −80 dBm for 30 s (10 dB + 30 s of dwell). After a
  fallback, the next WiFi attempt is allowed only after a backoff of **10 / 30 / 60 minutes,
  capped**, reset by a successful 10-minute WiFi session or by any user toggle. This is the
  anti-flap: a switch costs one TLS handshake on each side (~5 kB of LTE data when going back).
- **The switch itself is one function** (`net_xport_switch(to)`): bring the *old* transport's MQTT
  session down and wait for its DISCONNECTED (bounded 2 s), then bring the new one up. A hard
  invariant, asserted: at most one MQTT client object exists at a time. EMQX kicks the older
  session on a client-id collision, which would show up as a delivery gap, so this is not merely
  tidy (§8).

**The modem while WiFi carries the session: stay attached, drop only the MQTT session.**
Numbers from `V02_DESIGN.md` §8.4/§9.3: the LTE terms are eDRX paging **0.2–0.5 mA**, modem idle
floor **0.01–0.05 mA**, and the host liveness ping **~1.2 mA**. Dropping the MQTT session removes
the 1.2 mA term (the big one) and all 49 kB/day of ping data. `CFUN=4` would remove a further
**0.2–0.5 mA (5–13 mAh/day)** but costs device-direct SMS (`V02_DESIGN.md` §6), the cell-change
location trigger (§5 trigger 1), the `/loc` `cell` sub-map, and turns fallback into a full
re-attach. **Decision: stay attached.** Revisit as a phase-3 flag only if a measured paging term
lands at the top of that range and the owner is not using SMS.

---

## 3. Power

All figures per day on a 1500 mAh LiFePO4 cell. Today's LTE-only baseline is **4.0–4.5 mA →
95–107 mAh/day → 14–16 days** (`V02_DESIGN.md` §9.3).

| Configuration | ESP32+WiFi | Modem | MQTT keepalive | Total | Days |
|---|---|---|---|---|---|
| (a) WiFi, no `CONFIG_PM_ENABLE`, Modem-sleep only | 38.2 mA (IDF doc, DTIM10) | 0.2–0.5 | in the 38 | **≈38.5 mA → 925 mAh/day** | **1.6** |
| (b) WiFi, `PM_ENABLE` + DFS, no light sleep | 19.5 mA (IDF doc, DTIM10) | 0.2–0.5 | in the 19.5 | ≈20 mA → 480 mAh/day | 3 |
| (c) **WiFi, `PM_ENABLE` + auto light sleep, `WIFI_PS_MAX_MODEM`, listen_interval 10** | 0.93 mA (IDF doc, DTIM10) | 0.2–0.5 | 0.17 (est.) | **≈1.3–1.6 mA → 31–38 mAh/day** | **39–48** |
| (d) same, `WIFI_PS_MIN_MODEM` (active mode) | 2.45 mA (IDF doc, DTIM1) | 0.2–0.5 | 0.17 | ≈2.8–3.1 mA | — |
| (e) LTE today | 2.6 | 0.2–0.5 + 1.2 | — | 4.0–4.5 mA | 14–16 |

Keepalive estimate: 1440 PINGREQ/day × ~100 ms of radio at ~100 mA = 4 mAh/day = 0.17 mA
(estimate — measure it). The IDF rows are Espressif devkit measurements, not this board: the Walter
module, the e-paper domain and the CardKB pull-ups are all extra and are **not** in those numbers.

**Conclusion.** (a) and (b) are not battery configurations. So:

- **Phase 1 (a): WiFi runs only while the pager is on USB/charger power, or on the bench.** The
  console turns it on; nothing turns it on by itself. This is enough to bench the whole transport.
- **Phase 2 (c): `CONFIG_PM_ENABLE` + automatic light sleep is what makes WiFi usable on battery**,
  and it is also a **2.5–3× win over LTE** (31–38 vs 95–107 mAh/day) — if the modem UART survives
  it. That is the risk, not the WiFi part: with `PM_ENABLE` the APB clock moves and the system
  light-sleeps on its own, so `net.cpp`'s hand-rolled RTS choreography must be replaced by an
  `ESP_PM_NO_LIGHT_SLEEP` lock held around every AT transaction and while `net_modem_busy()` or
  `net_connect_in_flight()` is true. `esp_sleep_enable_uart_wakeup()` on the modem UART is the
  other half.
- **`net_sleep()` becomes transport-aware.** On LTE: today's code, unchanged. On WiFi: never call
  `esp_light_sleep_start()` — phase 1 does `vTaskDelay(ms)` (CPU on, USB alive, ≈38 mA); phase 2
  does `vTaskDelay(ms)` too and lets the PM framework light-sleep underneath it. `modes.c`'s call
  site does not change either way.
- The 200 ms `PAGER_POST_WAKE_YIELD_MS` exists only for the modem's held URCs
  (`GOTCHAS.md`, "Pages never arrive while the pager sleeps"). On WiFi it can drop to ~20 ms.

### 3b. Is sniffing for a known SSID ever sensible?

Assumptions, all `(estimate)` and all to be measured: radio-on current during a dwell **100 mA**
(IDF's S3 tables report 113–120 mA as the *max* with the radio active, so 100 mA is the honest
middle); `esp_wifi_start()` + PHY init/cal + stop overhead **200 ms at 80 mA**; US country code =
**11 channels**; default dwell **120 ms/channel** (`docs/en/api-guides/wifi.rst:567`); a passive
single-channel dwell of **150 ms** (one beacon interval is 102.4 ms) once the home channel has been
cached in NVS from the first successful association.

Per scan: single cached channel, passive = **0.009 mAh**. All 11 channels, active = **0.041 mAh**.

| Trigger | Scans/day | Single channel | All channels | % of the 100 mAh/day budget |
|---|---|---|---|---|
| on every 5 s wake | 17280 | 149 mAh | 709 mAh | **149% / 709% — ruled out outright** |
| every 60 s | 1440 | 12.4 mAh | 59 mAh | 12% / 59% |
| every 5 min | 288 | 2.5 mAh | 11.8 mAh | 2.5% / 12% |
| **every 15 min** | 96 | **0.83 mAh** | 3.9 mAh | **0.8%** / 4% |
| every 60 min | 24 | 0.21 mAh | 0.98 mAh | 0.2% / 1% |
| on `+CEREG` cell change (10-min debounce, `V02_DESIGN.md` §5 trigger 1) | 10–30 typical, 144 cap | 0.09–0.27 (1.2 cap) | 0.4–1.2 (5.9 cap) | 0.1–0.3% / 0.4–1.2% |
| on motion-settle (LIS3DH, sustained motion then 60 s quiet) | 10–20 | 0.09–0.18 | 0.4–0.8 | ~0.2% / ~0.8% |

**Verdict.** Sniffing *is* sensible, at a <1% budget hit, but only in the trigger-driven shape:
**scan when the serving cell changes or motion settles — both already have debounced triggers in
the firmware — plus a 15-minute single-cached-channel fallback while un-associated.** Combined
≈1–2 mAh/day, 1–2%. Scanning on the 5 s wake costs more than the entire current budget and is
ruled out permanently. Use a **passive** scan: it emits no probe request, so it produces no TX
burst, which matters given §8's supply-sag history. All of this is **phase 2, off by default**,
behind `cfg.wifi.scan_s` (0 = never scan; the manual-only default the owner asked for).

---

## 4. Provisioning

Three ways in, one store.

**Store.** NVS namespace `wifi`, separate from `ident` so a credential rotation never risks the
identity. Keys: `en` (u8), `n` (u8, 0–2), `s0`/`p0`, `s1`/`p1` (str), `ch0`/`ch1` (u8, the cached
home channel from the last successful association). **Up to 2 networks** (home and one other —
more is a UI problem, not a firmware one). SSID ≤32 bytes, PSK 8–63 bytes. **WPA2-PSK only**:
`threshold.authmode = WIFI_AUTH_WPA2_PSK`. WPA3-SAE is compiled in already
(`sdkconfig:1097-1098`) and a WPA2/WPA3 transition AP will associate as WPA2, so this costs
nothing; open and WEP networks are refused outright. Enterprise is out of scope.

**(1) `cfg` push** (`PROTOCOL.md` §10, `firmware/main/cfg.c`). New sub-map key `wifi = 3`:

```
cfg.wifi  = { en = 0 (bool), nets = 1 (array, <=2 of { s = 0 (tstr), p = 1 (tstr) }) }
```

Whole list every time, newest-wins, acked `shown` once written to NVS — identical to `cfg.sms`'s
contract (`PROTOCOL.md` §3.6). Size: 2 maximal entries ≈ 210 B CBOR, ≈ 250 B JSON, against the
640 B envelope cap — fits with room. `nets` absent means "leave the stored networks alone, apply
`en` only", so the web app can toggle WiFi without re-sending the PSK. `nets: []` clears them.
Relay work is one route and one validator; web work is one panel (`WIFI_TASKS.md` W7/W8).

**Security rules, non-negotiable:**
- A PSK is **never** in `/status`, never in any log line at any level, never in a `/loc` or `/up`
  envelope. `wificred.c` has no printing function; the console prints `<set>`/`<unset>`.
- The relay **refuses to push `cfg.wifi.nets`** to a device whose last `/status` reported
  `tls: "broken"` or `tls: "unpinned"` (`PROTOCOL.md` §5.1) — that is exactly the state in which
  the `/down` path is not confidential. `en` alone may still be pushed. This is the one new relay
  rule this design asks for.
- NVS is not encrypted on this device today. Recorded as a known limit, same as the broker
  password already stored in `ident`.

**(2) Setup console**, debug build only (`main.c`'s REPL): `wifi set <ssid> <psk>`, `wifi clear`,
`wifi on|off`, `wifi status` (prints SSID, RSSI, IP, transport, never the PSK), `wifi scan`.

**(3) Device menu** (`scr_device.c`): a WiFi row showing `off` / `<ssid> −62 dBm` / `LTE`, and an
on/off toggle. No text entry — there is no on-device SSID/PSK editor, deliberately (`DEVICE_PLAN.md`
§3.0 already rejected a SoftAP portal at "~450 kB of Wi-Fi stack"; that cost is now being paid for
a different reason, but the portal still is not worth it).

---

## 5. TLS

- **Reuse the CA bytes `catrust` already stores.** `ident_get_ca()` returns the pinned PEM
  NUL-terminated (`firmware/main/ident.h:95`); hand it straight to
  `esp_mqtt_client_config_t.broker.verification.certificate`. No second copy in the image, no
  `BROKER_CA_PEM` constant, and a `cfg.ca` push therefore repoints *both* transports at once.
- **Verification is mandatory and there is no plaintext fallback, ever.** This is a deliberate
  asymmetry with LTE: `V02_DESIGN.md` §4.2 lets the modem fall back to an *unvalidated* connect so
  that pages keep arriving, because on LTE that is the only path. On WiFi there is always another
  path — LTE — so an unverifiable broker is a **reason to switch transport**, not to lower the bar.
- Consequently **WiFi requires `catrust_get_state() == CATRUST_PINNED`.** `unpinned` (no CA in the
  identity at all) and `broken` both make the WiFi transport unavailable and the policy stays on
  LTE. Rejected alternative: ship ESP-IDF's certificate bundle
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`, ~30–50 kB) as a fallback trust store — it would work
  against EMQX Cloud's public CA, but it introduces a second, weaker trust model and a `/status`
  `tls` value with two meanings. One rule is better.
- A WiFi TLS failure is reported (`NET_MQTT_RC_TLS_FAIL`, logged, counted) but **must not** call
  `catrust_on_mqtt_tls_fail()`'s state machine — that machine's job is to decide whether to give up
  validating on LTE, and a failure on a different transport is not evidence about the modem.
- TLS 1.2 minimum, SNI on (esp-tls does this from the hostname), no client certificate.

---

## 6. `/status`

Add **`xport`** (tstr, `lte` | `wifi`), **CBOR envelope key 52**, optional, `/status` only. Written
next to `link` in `build_status_cbor()` (`firmware/main/modes.c:691`).

`link` (key 50) already increments on every MQTT session within a boot and the relay already treats
a changed `link` like a changed `session` (`relay/app/ingest.py:649-658`). A transport switch *is* a
new MQTT session, so it bumps `link`, so **the relay's re-publish-on-link-change logic keeps working
with no change at all**. `xport` is display and diagnosis only — the relay stores it on
`devices/{id}` and the web app shows a chip. Reusing `link` alone was considered and rejected: it
cannot say *which* transport, which is the first thing anyone debugging a delivery gap wants.

Relay change: accept and persist `xport` (`relay/app/wire.py`, `relay/app/wirecbor.py` key 52,
`relay/app/ingest.py`), plus the `cfg.wifi` push route and its `tls == pinned` guard (§4). Per
`V02_DESIGN.md` §0, **the relay accepts the field before any firmware that sends it is flashed.**

---

## 7. Bench plan

1. **First WiFi session.** Debug build, pager on USB. `wifi set <ssid> <psk>` → `wifi on`. Expect,
   in order: `wifi: associating`, `wifi: got ip 192.168.x.y rssi -NN`, `xport: LTE -> WIFI`,
   `MQTT connect issued` from the WiFi transport, `MQTT session usable (subscribed to ...)`,
   `published /status online`. Then `tools/bench/send_test_page.py` → the page renders in **under
   2 s** (against 27 s worst case on LTE). Capture with `tools/bench/serial_capture.py`, foreground,
   bounded.
2. **Forced fallback.** Turn the AP off. Expect within 20 s: association lost → 3 failed re-associates
   → `xport: WIFI -> LTE` → LTE `MQTT connect issued` → `published /status online` with a bumped
   `link`. Send a page; it arrives over LTE. Turn the AP back on: nothing happens for 10 minutes
   (the policy backoff), then WiFi is retried.
3. **No double session.** `tools/bench/poll_emqx_client.py` every 5 s across both switches:
   `/clients/{id}` must never show two entries, and `connected_at` must change exactly once per
   switch. A collision shows as EMQX kicking the older client — the single most likely delivery
   bug in this design.
4. **Power.** Current trace on the battery rail, three 10-minute windows: LTE idle (baseline,
   expect 4.0–4.5 mA), WiFi idle phase 1 (expect ~38 mA — confirms §3(a) and why phase 2 exists),
   WiFi idle phase 2 with `PM_ENABLE` (expect 1.3–1.6 mA). Also one trace across a single scan
   (§3b) and one across a single WiFi publish (peak current, for §8).
5. **Data.** `poll_emqx_loop.py` byte counters over an hour on each transport: LTE ≈4.4 kB/h from
   pings alone, WiFi ≈0 on the SIM. That is the cost case, measured.

---

## 8. Risks

- **Supply sag / coexistence.** The e-paper controller reset three times during LTE uplink bursts
  on 2026-09-22. WiFi TX peaks (IDF's own tables show 113–120 mA maxima on a devkit; a real
  20 dBm TX burst is higher) are the same class of event. Mitigations, in order: reuse the existing
  `publish_quiet` gate for the WiFi transport so no panel SPI runs during a publish; never refresh
  the panel while associating or scanning; cap TX power with
  `esp_wifi_set_max_tx_power(52)` (13 dBm) — a real lever that trades range for peak current, and
  a home AP is close. **UNVERIFIED** whether the two radios on this module share an antenna path;
  no schematic statement was found in this repo. Cheapest check: associate and attach at once and
  watch RSSI on both.
- **Flash.** App is **0xa1730 (661 kB) of 0x200000 (2 MB)** today. WiFi libs + lwIP ≈350–450 kB,
  esp-tls/X.509/TLS record layer ≈120–180 kB (mbedtls is already linked for HKDF/PBKDF2/SHA but not
  the handshake), esp-mqtt ≈20 kB → **estimated 1.15–1.3 MB, ~40% headroom left**. Two 2 MB OTA
  slots and a 16 MB part, so no partition change. W0 measures the real delta before anything else
  is built.
- **RAM.** WiFi wants ~50–60 kB of heap on top of the ~23 kB of `.bss` already spent on the thread
  ring and the two frame buffers (`PROTOCOL.md` §9.5). 512 kB total, so it fits, but the current
  `sdkconfig` is generous (`ESP_WIFI_STATIC_RX_BUFFER_NUM=10`, `DYNAMIC_TX_BUFFER_NUM=32`) — trim
  to 6/16 if W0's heap watermark is uncomfortable.
- **Two clients, one client id.** EMQX kicks the older session on a collision, so a botched switch
  looks exactly like "pages stopped arriving" with both sides believing they are connected. The
  single-`s_active_xport`-writer rule, the bounded wait for DISCONNECTED, and
  `disable_auto_reconnect = true` are all there for this one failure. Bench step 3 is its test.
- **`CONFIG_PM_ENABLE` regressions.** It changes APB frequency and introduces automatic light
  sleep for *everything*, including the modem UART, the e-paper SPI and the CardKB I²C. This is the
  riskiest single change in the plan and is why it is a late, separately-benched task (W9/W10) with
  an A/B runtime flag rather than a prerequisite.
- **Latency is not the win.** WiFi delivery is ~1 s, but the pager's own e-paper refresh is
  0.45–1.5 s and the `shown` ack waits for BUSY (`PROTOCOL.md` §4). Do not sell WiFi as a latency
  feature; sell it as data cost, and as battery only after §9's experiment settles.

---

## 9. The one experiment that changes the plan

**Question:** does a manual `esp_light_sleep_start()` keep a WiFi association alive on IDF 5.x if
`esp_sleep_enable_wifi_wakeup()` is armed and `ESP_PD_DOMAIN_MODEM` is left on?

**Why it matters:** if yes, phase 2 is a small change to `net_sleep()` and `CONFIG_PM_ENABLE` is
never needed — the whole `PM_ENABLE` risk (W9) evaporates. If no, W9 is the only route to a
battery-viable WiFi.

**Cheapest experiment (~30 min, debug build, bench):** associate, connect MQTT, publish `/status`;
then `esp_sleep_enable_wifi_wakeup()` + `esp_sleep_pd_config(ESP_PD_DOMAIN_MODEM, ESP_PD_OPTION_ON)`
+ `esp_light_sleep_start()` for 5 s, ten times in a row. After each wake print
`esp_wifi_sta_get_ap_info()`'s return and the RSSI. Have the relay send a page during sleep cycle 6.
Pass = association survives all ten, the MQTT session is still up, and the page arrives. Do this
**before** W9 (it is task W5's last verify step in `WIFI_TASKS.md`).

Two smaller unknowns, same treatment: (a) whether the AP's DTIM is 1 or 3 (read it from
`wifi_ap_record_t` after association — it decides which row of §3's table applies); (b) the real
per-PINGREQ energy at keepalive 60 s, which is the only estimate in §3's winning row.
