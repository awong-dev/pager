# v0.2 design: bug fixes, wider replay counter, CA trust, location, device SMS

Status: **implemented on branch `v0.2-dev`** (2026-09-20, after tag `v0.1`); builds and tests pass;
**nothing has run on hardware and nothing is deployed**. §9 is the checklist for that. This is
the spec the firmware, relay and web work is built from. Where it disagrees with `PROTOCOL.md` or
`DEVICE_PLAN.md`, this file records an **owner decision** and those documents are to be edited to
match as part of the work. Read `BRINGUP_NOTES.md` first for what v0.1 learned on real hardware.

Conventions: `UNVERIFIED` means "could not be tested on hardware while writing this; the code must
fail safe and log clearly". Hardware facts are for the Sequans GM02SP `LR8.2.1.0-61488`.

## 0. Ground rules

- v0.1 works. Nothing here may break the receive path: attach → TLS → MQTT → verify → render →
  ack. Every new feature fails *open* for paging: if location, SMS or a CA push goes wrong, pages
  still arrive.
- The relay must keep accepting a v0.1 pager (no new field is required from the device; unknown
  `/status` fields from a newer device must not make an older relay reject the envelope, so **add
  the relay's acceptance of new fields before any firmware that sends them is flashed**).
- All new device→relay and relay→device messages are signed like every other envelope.
- New CBOR keys are allocated in §7 and nowhere else.

## 1. Vendored modem library

`dptechnics/walter-modem` v1.5.0 moves from the component manager (`managed_components/`,
git-ignored, hash-checked, cannot be patched) to a tracked local component,
`firmware/components/dptechnics__walter-modem/` (`src/`, `CMakeLists.txt`, `Kconfig`, `LICENSE`,
`README.md`, `CHANGELOG.md`; **no `examples/`**). The DPTechnics 5-clause licence permits
modified source redistribution with the notice retained and use restricted to Walter boards; keep
`LICENSE` and every file header. Remove the dependency from `main/idf_component.yml`, add the
component to `main`'s `REQUIRES`, delete `dependencies.lock` entries for it. Add
`firmware/components/dptechnics__walter-modem/PATCHES.md` listing every local change, each marked
with a `// PAGER PATCH:` comment in the source, so a future upstream update can re-apply them.

Patches:

1. **NULL command crash.** In `_processModemRSP()` after `after_processing_logic:`, never call
   `_finishModemCMD(NULL, ...)`. With no command pending, free the buffer and return.
2. **Receive bounds.** Wherever a response is copied into `cmd->payload`
   (`memcpy(cmd->payload, rspStr, cmd->payloadSize)` and siblings), copy at most the bytes
   actually present in the `WalterModemBuffer` and never more than `payloadSize`. `mqttReceive()`
   and `socketReceive` must not claim more than was delivered. If a payload larger than one
   buffer cannot be reassembled, fail the command cleanly.
3. **TLS profile range.** Kconfig `WALTER_MODEM_MAX_TLS_PROFILES`: allow up to 6 (the modem's real
   limit, `spId` 1-6). Profile 1 stays BlueCherry's, 2 is MQTT, **3 is the CA fetch** (§4.4).
4. **SMS** (§6): `smsSend()`, an SMS event handler for `+CMTI`, `smsRead()`, `smsDelete()`.

## 2. Firmware bug fixes

1. **Render queue.** `s_render_pending` (`modes.c`) is a single slot; a burst overwrites it and the
   earlier messages are never marked `shown`. After a successful `ui_incoming()` render, mark
   **every** `MSG_ACK_UNSHOWN` down message `shown` (`msg_mark_all_unshown()` already exists for
   the unlock path). The pending-ack queue is 8 deep (`MSG_PENDING_ACKS_MAX`); acks that do not
   fit must be retried on later cycles, not lost: keep marking until none are `UNSHOWN`.
2. **Epoch copy.** `ident_store()` must update the in-memory `s_ident` on success, so
   `ident_get_n_epoch()` is current immediately after a bump.
3. **Connect watchdog.** A connect that produces neither CONNECTED nor DISCONNECTED within 60 s:
   `mqttDisconnect()`, count it, back off as a transient failure. Three in a row:
   `net_recover_modem()`. The forum threads in `BRINGUP_NOTES.md` show this engine can wedge
   silently.
4. **Empty CA slot.** `UNVERIFIED` whether the MQTT engine does TLS when the named slot is empty.
   Make it not matter: when no CA is pinned, make sure slot 12 holds *a* certificate. Embed ISRG
   Root X2 (790 bytes PEM, public, EC, long-lived) in the firmware as a placeholder; on
   `net_init()` and in the bootstrap path, if nothing has been written this power session and the
   slot is not known to hold a cert, write the placeholder. It is never validated against
   (validation is off in that mode). One NVRAM write per device lifetime in practice: remember in
   NVS (`ident` namespace, key `slot12`) that the slot is populated.
5. **Oversize receive.** With patch 1.2, a `/down` or boot message larger than the library can
   deliver is logged at INFO with its size and counted as oversize; it must not corrupt memory.
6. **Log what is dropped.** "malformed down message dropped" becomes INFO (it was DEBUG and cost
   an evening). Include the length and first byte.
7. Keep `nettest`, `mqtttest`, the setup-stage logging and `PAGER_DEBUG_NO_LIGHT_SLEEP`. Add, in
   that debug build only, the console REPL in **normal** mode too (its own task, 16 kB stack), so
   diagnostics can run on a provisioned pager without erasing its identity. New debug commands:
   `gnsstest <seconds>`, `cafetch <url> <sha256hex>`, `smstest <number> <text>`.

## 3. Replay counter: keep it, widen the epoch (owner question, decided)

Question from the owner: is the counter needed at all, given TLS, and given that 4096 cold boots
is a realistic lifetime with a flaky battery?

- TLS covers only pager↔broker, and only when the server is authenticated. §4 makes that
  authentication optional and adds a deliberate fallback to *no* validation, so TLS cannot be the
  replay defence. The HMAC exists because the broker is a third party and the webhook is a shared
  secret; the counter is what stops that same party (or an on-path attacker during a fallback)
  replaying an old signed "come to the office now" page. The relay dedups `/up` by message id for
  ever, so `/up` replays are already harmless; the exposure is `/down`, where the pager's dedup
  ring is 16 ids in RTC memory and is lost on a cold boot.
- So the counter stays, but its failure mode must not be "pager silently dead". The 12-bit epoch
  was exactly that: guaranteed exhaustion.

**Change:** `n = (epoch << 20) | lo` stays, `lo` stays 20 bits, **`epoch` becomes 32 bits** and `n`
becomes a 52-bit unsigned integer (< 2^53, exact in JSON and in a Firestore int64). 4 billion cold
boots. CBOR encodes it at minimal length, so nothing grows until the epoch passes 4095.

- Firmware: `n` is `uint64_t` end to end on the up path (`auth_next_up_n`, signing, CBOR writer).
  `ident_t.n_epoch` becomes `uint32_t`; NVS key `n_epoch` (u16) is read for migration, the value
  is then kept in a new key `n_epoch32` (u32). On a wrap of `lo`, or on a cold boot, bump and
  persist (already implemented for u16). The down path may stay `uint32_t` internally (the relay's
  `downN` counts by one) but must *parse* a uint64 without rejecting it.
- Relay: `wire.py` accepts `0 <= n < 2**53`; the window code already uses Python ints; Firestore
  `upN` is an int64. `tools/authvectors.json` gains vectors with `n` above 2^32, and
  `pager_client.py` and the firmware host test consume them.
- `PROTOCOL.md` §3.1 (`n` row) and §14 (counter text) and `DEVICE_PLAN.md` §2.5 edited to match.
- Still parked: the signed resync handshake (`DEVICE_PLAN.md` §2.5 note). With a 32-bit epoch it
  is only needed if NVS itself is lost, and then the identity is gone too.

## 4. CA trust (implements `CA_TRUST_PLAN.md`)

### 4.1 Trust states (firmware)

`unpinned` (no CA in the identity; validation off) · `pinned` (CA set, last connect validated) ·
`broken` (CA set, validation failed, running with validation off). `broken` is one byte in NVS
(`ident` namespace, key `tls_broken`), cleared only by a connect that validates.

### 4.2 Fallback

On `NET_MQTT_RC_TLS_FAIL` while `pinned`: one more validated attempt, then reconfigure profile 2
with validation **off, slot 12 still named**, enter `broken`, connect. While `broken`, the first
connect after each 24 h boundary (and the first after a cold boot) is attempted validated; success
returns to `pinned`. `-8` also fires for non-certificate handshake failures, so the UI text is
"server not verified", never "attack".

### 4.3 Reporting

- `/status` gains optional `tls` (`unpinned`/`pinned`/`broken`) and `ca_fp` (first 16 hex chars of
  the SHA-256 of the pinned CA PEM, absent when unpinned).
- Pager status bar: a padlock icon when `pinned`, a **broken padlock** when `broken`, nothing when
  `unpinned`. Two new 12×12 icons in `gfx` (follow the existing icon mechanism in `gfx.h`).
- Relay: persist both on `devices/{id}`; log `SECURITY tls-broken device=…` on the transition into
  `broken`. Web: a chip per device on *Admin → Devices* (pinned / not verified / unpinned, plus
  `ca_fp`), tolerant of the fields being absent.

### 4.4 CA delivery: a pointer, fetched and hash-checked

The CA never travels inline again (a Let's Encrypt or Google root does not fit the modem
library's 1540-byte buffer; see `CA_TRUST_PLAN.md` §3.4).

- **Relay** serves `GET /ca/{sha256hex}.pem`, public, no auth, `text/plain`, immutable cache
  headers, 404 unless the hash is one it knows. It knows the CA from `BROKER_CA_PEM`, else from
  `ca_resolve.get_broker_ca_pem()` (wire that resolver in at last), and keeps every CA it has
  ever served in Firestore `cas/{sha256hex}` so an old pointer keeps resolving. The public base
  URL comes from a new setting `PUBLIC_BASE_URL` (Terraform: the Cloud Run service URL).
- **Bootstrap bundle** carries `ca_url` + `ca_sha` instead of `ca` (both absent = unpinned). The
  firmware still accepts an inline `ca` (a v0.1-era bundle), preferring the pointer.
- **Push:** `/down` `kind:"cfg"` with `cfg.ca = {url, sha}`; `url` = `""` means un-pin. Admin-only
  `POST /api/admin/devices/{id}/ca` with body `{"action": "push"|"unpin"}`; like `cfg.lock`, only
  the newest unacked cfg is re-published.
- **Fetch (firmware, `cafetch.c`)**: a TLS socket on **TLS profile 3** (validation off, slot 12
  named), `GET <path> HTTP/1.1` with `Host` and `Connection: close`, read in ≤1500-byte chunks,
  follow no redirects, cap the body at 4096 bytes, require status 200, verify SHA-256 with
  mbedtls, require the body to contain exactly one PEM certificate. Trust comes from the hash,
  which arrived signed (or inside the encrypted bundle), so the transport needs no authentication.
  `UNVERIFIED`: a second socket while the MQTT session is up.
- **Apply, two-phase**: write the new CA to **slot 13**, disconnect, point profile 2 at slot 13
  with validation on, connect. CONNECTED within the watchdog → commit (`ident_store` the PEM and
  hash, copy to slot 12, state `pinned`, ack the cfg `shown`). Anything else → restore the
  previous profile and state, reconnect, **do not ack** (the relay re-publishes on the next online
  edge; after 3 failed applies of the same `sha` the pager acks it anyway and logs, so a bad CA
  cannot loop for ever). Un-pin: clear the identity's CA, validation off, state `unpinned`, ack.
- Setup uses the same fetch, over the bootstrap attach, before the identity is stored. A fetch
  failure during setup fails setup with "cannot reach broker" (never silently unpinned).

## 5. Location (implements `LOCATION_PLAN.md` L1-L4)

Request-driven, short, always backed off. Owner guidance: the pager spends many hours a day inside
a building with no sky; even one attempt an hour is wasted there without evidence of movement; a
failed or wrong fix is an acceptable answer; battery matters more.

- **Dispatch**: `kind:"loc_req"` intercepted in `on_incoming_message()` after signature
  verification, before `msg_ingest_down_cbor()`; answered regardless of lock state; never a thread
  entry; never `shown`/`read`-acked. Works identically in sleep and active mode (it is just a
  `/down` message; do not switch the mode to active for it and do not wake the display).
- **Answer on `/loc`** exactly per `PROTOCOL.md` §13.2, signed, QoS 1, id `l_` + 8 hex.
- **Attempt budget**: `LOC_ATTEMPT_S = 20` normally; `LOC_FIRST_ATTEMPT_S = 40` for the first
  attempt after a cold boot or after assistance data was refreshed (no ephemeris in the receiver
  yet). Never longer. One attempt in flight; requests arriving meanwhile share its result.
- **Backoff**: after a failed attempt no new attempt is allowed for `backoff`: **5 min, doubling,
  capped at 12 h** (5, 10, 20, 40, 80, 160, 320, 640, 720 min). Reset to zero by a successful
  fix. A request inside the backoff is answered at once, without powering GNSS: the last fix with
  `cached:true` if one exists from this power session, else `loc:null, err:"no_fix"`. Worst case
  for a pager left in a drawer: about nine attempts on day one, then two a day, 20 s each.
- **Triggers that reset the backoff** (they never start an attempt by themselves):
  1. *Tracking-area or cell change* reported in `+CEREG` (the library's network event). Debounce:
     ignore changes within 10 min of the last one, because a stationary indoor modem flaps
     between cells.
  2. *Sustained motion* from the LIS3DH (I2C 0x18, INT1 on IO2): motion interrupts spanning at
     least 60 s within a 3 min window. Probe the chip at boot; if it is absent (it may not be
     wired yet) log once and run without it. Configure its low-power 10 Hz mode with the
     interrupt on a high-pass threshold; the ESP32 wakes on IO2 via `ext1` alongside the button's
     `ext0`.
  3. *Charger/USB power present*, if detectable: skip.
  A reset puts the backoff at zero but leaves a **floor of 10 min since the last attempt**, so a
  child walking between classrooms all day cannot turn every request into an attempt.
- **Battery floor**: below 3.3 V (LiFePO4, `batt_mv`), never power GNSS; answer cached/`no_fix`.
- **Route to the radio** (`UNVERIFIED`, Sequans forum thread 209: GNSS is non-concurrent with LTE
  but may run in LTE "off" periods, PSM or `CFUN=4`): try in this order and remember the first
  that works in RTC memory.
  1. *In place*: ask for a fix while attached, in the eDRX idle gap. If the modem refuses, →
  2. *`CFUN=4` window*: `mqttDisconnect()` cleanly, NO_RF, fix, FULL, re-attach, reconnect, publish.
     This session loss is deliberate: it must not count towards `handle_mqtt_loss()`'s backoff,
     the connect watchdog or the F4 health check.
  A PSM window is the better long-term answer and is left for a hardware session.
- **Assistance**: before an attempt, if `gnssGetAssistanceStatus()` says real-time ephemeris is
  due, `gnssUpdateAssistance()` first (needs LTE, so before step 2). Log the bytes it costs.
- **Accept only a good fix**: confidence ≤ 100 (the vendor demo's threshold); map it to `acc`.
- **`/status`**: `loc_min_s` = 600 (the floor), `loc_period_s` = 0 (no unsolicited fixes), and new
  `loc_backoff_s` = seconds until the next attempt is allowed (0 = now).
- **Relay/protocol**: §13.3 item 1 is amended: the device's window is the growing backoff above,
  reported in `/status`; the relay's own mirrored 60 s rule is unchanged. `tools/pager_client.py`
  answers `loc_req` the same way (a `--loc lat,lon` option, else `no_fix`) so the e2e suite covers
  the lifecycle.

## 6. Device-direct SMS

Owner decision (2026-09-20): the pager may send and receive SMS directly through the modem, to a
**parent-managed allow-list of phone numbers**, as a delivery path that does not depend on the
relay. This reverses `PROTOCOL.md` §7.3's "no device-side SMS path"; edit it.

- **Who controls the list**: only the device's owner (or an admin), in the web app. The pager has
  no UI to add, edit or remove a number. Max 8 entries `{name ≤ 16 chars, phone E.164}`.
  Stored on `devices/{id}.smsContacts`; delivered as `/down` `cfg` with `cfg.sms = [{n, p}, …]`
  (whole list each time, newest-wins, acked `shown` on apply). The pager keeps it in NVS.
- **Sending**: SMS contacts appear in the recipient picker and as `@name` in the composer, tagged
  `sms`. A message to one goes out with `smsSend()`; GSM 7-bit when every character is in the
  basic alphabet (≤160), otherwise UCS-2 (≤70 characters; the composer shows the tighter limit).
  No concatenated SMS. The thread entry shows `sent` / `FAILED` like any other reply.
- **Receiving**: `+CMTI` → read, delete from the SIM, then: sender on the list → insert into the
  thread as from that contact and alert like any message; sender not on the list → **never
  shown**, logged as `blocked`.
- **Audit, non-negotiable**: every SMS in either direction, including blocked ones, produces a
  signed `/up` envelope `kind:"sms_log"` with `id` (`s_` + 8 hex), `ts`, `peer` (E.164), `dir`
  (`out`/`in`), `st` (`sent`/`failed`/`recv`/`blocked`), `body`, `sms_ts`. QoS 1. If the pager is
  offline it is queued in NVS (same durability as an unsent reply, at least 16 entries, oldest
  dropped last resort with a counter reported in `/status`) and sent when the session is back. An
  SMS is **sent first, logged second**: the point of this path is that it works without the relay.
- **Relay**: `sms_log` is stored under `devices/{id}/smsLog/{logId}` (dedup on id), is not routed
  to anyone and is not a thread entry. Owner/admin API: `GET /api/devices/{id}/sms-log`,
  `GET/PUT /api/devices/{id}/sms-contacts` (PUT validates E.164, max 8, pushes the cfg).
  Firestore rules: readable by the device's owner and admins only.
- **Web**: on the device's page, an *SMS contacts* editor and an *SMS log* table (time, direction,
  number/name, status, text), blocked entries highlighted.
- `UNVERIFIED`: SMS on the production SIM at all (a Google Fi data-only SIM may not carry SMS);
  the modem's `+CMTI` behaviour under eDRX; text-mode UCS-2 on this firmware. `smstest` exists to
  find out. Everything fails safe: an SMS that cannot be sent is a `FAILED` thread entry plus an
  `sms_log` with `st:"failed"`.

## 7. Wire additions (the only place new keys are allocated)

Envelope keys:

| Key | Name | Type | Where |
|---|---|---|---|
| 39 | `tls` | tstr | `/status` |
| 40 | `ca_url` | tstr | bootstrap bundle |
| 41 | `ca_sha` | bstr(32) | bootstrap bundle |
| 42 | `ca_fp` | tstr(16) | `/status` |
| 43 | `loc_backoff_s` | int | `/status` |
| 44 | `peer` | tstr | `/up` `sms_log` |
| 45 | `dir` | tstr | `/up` `sms_log` |
| 46 | `st` | tstr | `/up` `sms_log` |
| 47 | `sms_ts` | int | `/up` `sms_log` |
| 48 | `sms_lost` | int | `/status` (audit entries dropped, normally 0) |

`kind` gains `sms_log` (`/up`). `cfg` sub-map: `lock=0` (existing), **`ca=1`** `{url=0 tstr,
sha=1 bstr(32)}`, **`sms=2`** array of `{n=0 tstr name, p=1 tstr phone}`. In JSON the same names
are used; `ca_sha`/`sha` are base64url without padding, like `sig`.

`n` (key 12): uint, now up to 2^53-1 (§3).

## 8. Verification

- Firmware: `make -C firmware/host test` (add tests: counter ≥ 2^32 vectors, location backoff with
  a fake clock, SMS GSM-7/UCS-2 encoding and allow-list, CA fetch HTTP parsing and hash check,
  trust-state transitions, cfg `ca`/`sms` decode). `idf.py build` clean in both normal and
  `PAGER_DEBUG_NO_LIGHT_SLEEP` configurations.
- Relay: `pytest` (emulators), ruff; new tests for every new route and ingest kind; e2e scenarios
  `locate`, `ca_push`, `sms_log` with `pager_client.py`.
- Web: `npm run build`, lint.
- Hardware, by a person: everything marked `UNVERIFIED`, using the debug console.

## 9. Bringing v0.2 up on hardware (for a person; nothing here has been done)

Images for a quick flash or a quick retreat are in the untracked `build/images/`:
`v0.2-dev-debug-app.bin` and `v0.1-debug-app.bin`, both the no-sleep debug build, both written to
the app partition only: `esptool.py --chip esp32s3 write_flash 0x120000 <file>`. Identity, assets
and messages are untouched by either, and the NVS changes are forward-only but harmless to v0.1
(new keys it ignores; the epoch simply keeps rising).

**Order matters.** Deploy the relay first, then flash. The v0.1 relay drops any envelope carrying
a CBOR key it does not know, and v0.2 firmware adds keys to `/status`, so new firmware against
the old relay loses its status and online edge (pages still arrive). The v0.2 relay accepts both.

1. **Relay**: merge `v0.2-dev` to `main` (CI deploys). Repo variables `BROKER_CA_PEM_FILE` and
   `PUBLIC_BASE_URL` are already set. Check `GET <relay>/ca/<sha256 of the DigiCert PEM>.pem`
   returns the PEM after one setup code or CA push has been issued (that is what registers it).
2. **Boot check** with the debug image: no panic, `IDENT … sig=1`, `n_epoch bumped`, `TLS profile
   2: CA pinned`, `MQTT session usable`, `published /status`; then in the relay log **no**
   `malformed`, `bad-sig` or `replay`. Expect `LIS3DH not found` and possibly `SMS init failed`
   (a data-only SIM): both are fine. Send a page; check the chip reaches delivered.
3. **The console now works on a provisioned pager** (debug build). Run, in this order, watching
   that pages still arrive after each:
   - `cafetch <relay>/ca/<sha>.pem <sha>`: answers whether a second socket can open while MQTT
     is up (`mqtt_survived_second_socket=`), and whether the TLS socket receive path works.
   - Web app → *Push CA* with the correct CA: expect fetch → scratch slot → validated reconnect →
     commit, and the chip stays "Server verified". Then set a **wrong** `BROKER_CA_PEM` on a
     test relay revision and push: expect rollback, pages still arriving, no ack; after three
     tries, an ack and a log line.
   - Fallback: there is no safe way to fake a bad broker certificate against production; test it
     by pinning a wrong CA through a setup code on a scratch device id and watching for
     `broken`, the broken padlock, and pages still arriving.
   - `gnsstest 40` indoors (expect a clean `no_fix` and a live session afterwards), then
     outdoors. The log says which radio route was accepted; if the in-place route is refused it
     falls back to the `CFUN=4` window, and the re-attach time is the number to write down.
   - `smstest <your number> hello`, then text the pager from a listed and an unlisted number.
4. **Only then** the normal (sleeping) build, and the test that is still the biggest unknown in
   the whole design and predates v0.2: does a page arrive while the pager is in light sleep
   (`PROTOCOL.md` §8.3, M5). The same question now applies to `+CMTI` and to GNSS events.

Known gaps carried forward: no retention sweep for `smsLog`; `ca_resolve.resolve_broker_ca()` is
still not called at relay startup (the env var is the only CA source in practice); no e2e scenario
for a CA push; SMS boot drain is an index scan, not `AT+CMGL`; a received SMS is held in RAM only;
the web app has no test runner. If the project moves to Soracom (`SORACOM_EVAL.md`), §4 is
unnecessary there and §6 cannot work as designed on a Soracom SIM.
