# Soracom bearer: plain MQTT through Beam, chosen by the inserted SIM (design, 8 Oct 2026)

Status: **decided by the owner on 8 Oct 2026; being built headless on the bench unit** (the proto3
Walter, MAC 20:6e:f1:dc:cc:6c, now carrying a Soracom SIM; flash erased 14:42 PDT). The evaluation
that led here is `SORACOM_EVAL.md`; this document is the shape that is actually being built: the
"Beam MQTT entry point in front of EMQX" option, not the broker-free UDP design. The eval's
step 3 (UDP downlink) is explicitly out of scope.

Terms: **transport** (`xport`) stays what it is today: `lte` or `wifi`, the radio. **Bearer** is new
and only meaningful on `lte`: *how the MQTT session reaches the broker*, `direct` (TLS from the
modem to EMQX, today's path) or `beam` (plain MQTT from the modem to `beam.soracom.io:1883`;
Soracom Beam opens the MQTTS leg to EMQX). The bearer is a property of the SIM, so it is picked by
the carrier detection the firmware already does (`firmware/main/carrier.[ch]`), not by a build.

## 1. What the owner asked for

"Enable the firmware and relay to support this as well based on what SIM is inserted." One image
runs on both SIMs. A US Mobile Dark Star SIM behaves exactly as v1.0.0 does today. A Soracom SIM
attaches with Soracom's APN and credentials and carries its MQTT session through Beam with no TLS
on the pager. No owner interaction during development; the bench unit is driven entirely over USB
and through the production relay.

## 2. Facts the design rests on (verified in the code or on the web, 8 Oct 2026)

- The MQTT host and port come from the setup bundle and live in NVS `ident` (`host`, `port`);
  `xport_lte.cpp:611` connects to them; `net.cpp:827-930` `configure_session()` always builds TLS
  profile 2 naming CA slot 12 and calls `mqttConfig(dev_id, dev_id, mqtt_pw, 2)`. The vendored
  library only appends the TLS profile to `AT+SQNSMQTTCFG` when the id is non-zero
  (`WalterMQTT.cpp:71-118`), so `tls_profile_id = 0` is the library's own "no TLS" spelling.
- `carrier.c` already detects the SIM from the IMSI's first six digits plus EF_GID1 and persists a
  fixed choice in NVS namespace `carrier` (`mode`, `apn`). `effective_apn()` (`net.cpp:596-629`)
  resolves: typed `;apn=` → fixed choice → auto-detect → bundle `apn` → blank. The greeting screen
  in Setup mode already runs `net_check_sim()` (AT+CIMI at NO_RF).
- The library has `setPDPAuthParams(proto, user, pass, ctx)` (`WalterModem.h:6065`) and the
  firmware never calls it (`definePDPContext` only, `net.cpp:724`, `:1011`).
- Production broker is EMQX Cloud Serverless, `s1289801.ala.us-east-1.emqxsl.com:8883`, certificate
  chained to DigiCert Global Root G2 (`infra/envs/prod/terraform.tfvars`). Beam validates with a
  normal root store, so it accepts it with no extra configuration.
- Devices authenticate to EMQX with a per-device username/password (username = device id;
  `relay/app/emqx_admin.py`). Beam's MQTT entry point (version 201912) has a credential
  **pass-through** mode (`useClientCredentials`): the pager's own username/password go through to
  the broker unchanged. Keepalive must be 0 or 5…1200 s (ours is 480; the host-side liveness
  re-SUBSCRIBE every 300 s is inside Beam's 1.5 × keepalive idle cut). Beam does not limit payload
  size; it keeps no session state (we already assume clean session).
- The relay sees only `topic`, `payload`, `qos` from the webhook and authenticates the device by the
  HMAC in the envelope (`ingest.py:225-362`). A Beam-relayed session that publishes the same topics
  is indistinguishable from a direct one. **The data path needs no relay change.**
- Soracom APN for Air for Cellular (owner, 8 Oct 2026): `soracom.io`, user `sora`, password
  `sora` (PAP or CHAP). Soracom's documented IMSI prefix for Global SIMs is not confirmed on the
  web; the bench reads it from the SIM (§6, step A) and the preset is written from that reading.
- Device-originated SMS on a Soracom SIM can only reach Soracom's own endpoint, never a phone.
  The pager's SMS feature is already dead on the US Mobile line and the relay's Twilio path
  (`RELAY_SMS_DESIGN.md`) is the SMS design of record, so on the Beam bearer the firmware simply
  never attempts modem SMS.

## 3. Firmware design

### 3.1 Carrier presets grow a bearer and PDP credentials

`carrier_preset_t` gains `bearer` (`CARRIER_BEARER_DIRECT` | `CARRIER_BEARER_BEAM`), `auth_proto`
(none/PAP/CHAP), `auth_user`, `auth_pass`, and `sms_mo` (bool, may the pager send SMS through the
modem). New preset:

```
{ "Soracom", "soracom.io", "<PLMN prefixes read on the bench, e.g. 29505x>", NULL,
  CARRIER_BEARER_BEAM, PAP, "sora", "sora", sms_mo=false }
```

Existing presets: bearer `direct`, auth none, `sms_mo=true`. "Automatic" resolves to whatever the
SIM detects; "Carrier default" and a typed custom APN are `direct`/none, exactly today's behaviour.

A new pure function `carrier_effective(const char *imsi, const char *gid1, const char *typed_apn)`
returns the preset in force (fixed choice, else detection, else NULL) and is the single place
`net.cpp` asks "which carrier?". `effective_apn()` keeps its precedence but reads the APN, auth and
bearer from the same answer, so they can never disagree.

A debug/Setup console command `bearer [auto|direct|beam]` stores an override in NVS `carrier`
(`bearer`, absent = auto) for A/B on one flash (owner's standing rule: experimental behaviour
behind a runtime flag). `direct` on a Soracom SIM means TLS straight to EMQX over Soracom data,
which is how the Soracom radio side (attach, eDRX, paging) is verified before Beam is configured.
The override can force `direct` on any SIM but `beam` only when the carrier in force is a Beam
preset: `beam.soracom.io` resolves to 100.127.x.x (carrier-grade NAT space), so on another
operator's network a forced `beam` would send the MQTT password in clear text to whatever host
answers there. The override is read once at boot (no live bearer switch). `net_bringup()` /
`net_bootstrap_attach()` resolve the carrier once and cache the bearer in RAM; `configure_session()`
(re-run after every modem reset) reads the cache and never re-reads the SIM.

### 3.2 PDP context authentication

**Do not use the library's `setPDPAuthParams()`: it never emits anything.** It returns OK early
when the context's *stored* `auth_proto` is NONE (`WalterModem.cpp:5860`), that field starts at NONE
(`WalterModem.h:1671`) and nothing else writes it, so `AT+CGAUTH` (`WalterModem.cpp:5868`) is
unreachable. Instead, right after `definePDPContext(PAGER_PDP_CTX_ID, apn)` (`net.cpp:724` and
`net.cpp:1011`), while the modem is still at CFUN=4 and before `setOpState(FULL)` (the LTE attach
sets up the default bearer, so credentials must already be in place), send the raw command through
`WalterModem::sendCmd()` (already used at `net.cpp:742`): `AT+CGAUTH=1,1,"sora","sora"`
(1 = PAP, 2 = CHAP). Log `pdp auth: PAP user=sora` once.

The Sequans keeps `+CGDCONT`/`+CGAUTH` in its own NVM across resets (expected; verify with
`AT+CGAUTH?` after a reboot, §6). So a pager moved from a Soracom SIM back to US Mobile would attach
with stale `sora` credentials. Keep the direct path unchanged on a pager that never saw a Soracom
SIM: set NVS `carrier/auth=1` when CGAUTH is written, and only when it is set and the carrier in
force has no credentials send `AT+CGAUTH=1,0` and clear the flag. A typed `;apn=soracom.io` on a
Soracom SIM still takes auth and bearer from the detected preset (typing the preset's own APN is not
a custom APN).

### 3.3 Session configuration on the Beam bearer

In `configure_session()` and the bootstrap path:

| | direct (today) | beam |
|---|---|---|
| Host, port | NVS `ident` host/port (from the bundle) | `PAGER_BEAM_HOST "beam.soracom.io"`, `PAGER_BEAM_PORT 1883` (compile-time constants) |
| TLS profile | 2, slot 12, VALIDATION_CA or NONE per catrust | **none**: `mqttConfig(dev_id, dev_id, mqtt_pw, 0)`; no `tlsConfigProfile`, no CA slot write |
| Client id / username / password | dev_id / dev_id / mqtt_pw | identical (pass-through) |
| Keepalive | 480 | 480 |
| catrust / cafetch | as designed in V02 §4 | skipped; a `ca_url`/`ca_sha` in the bundle is ignored (`ident` has no field for the pointer, so it is not stored; the identity is stored unpinned); `/status.tls` = `proxy` |
| `cfg.ca` push | two-phase apply | refused with a failure ack, nothing fetched: the trial reconnect (`catrust.c:493-592`) would "validate" the scratch-slot CA over a plaintext hop that never uses it and commit it |
| Bootstrap (`setup <code>`) | TLS none-validation to `host:port` from the code | plain MQTT to Beam as `boot-<bid>`/`bpw`, keepalive 60; same topics; the code's host is logged and ignored (Beam's group destination decides the broker) |

Branch points (each `if (bearer == BEAM)` with the existing code untouched in the `else`):
`configure_session()` skips the CA/`tlsConfigProfile` block (`net.cpp:887-923`) and passes profile 0
to `mqttConfig` (`:922`); `xport_lte.cpp:611` picks host/port; `setup.c:692` skips
`net_tls_profile_bootstrap()`, `setup.c:702` passes the Beam host/port, `net_bootstrap_connect()`
passes profile 0 (`net.cpp:1074`); `setup.c:771` skips the CA fetch; `catrust_service()` refuses
`cfg.ca`. `catrust_before_reconnect()`/`catrust_on_mqtt_tls_fail()` need no branch: they only touch
profile 2, which the beam session does not name, and a plain-TCP connect cannot return
`WALTER_MODEM_MQTT_TLS` (`xport_lte.cpp:344`). UNVERIFIED: that `AT+SQNSMQTTCFG` without `<sp_id>`
clears a previously configured one without a modem reset; check `AT+SQNSMQTTCFG?` on the bench.

The bundle's `host`/`port` are still stored in `ident` so a later SIM swap back to a direct bearer
works without re-provisioning; on `beam` they are simply not used. The GOTCHAS.md hazard ("a TLS
profile naming no CA sends plaintext") is unaffected: the direct path still always names slot 12.

The WiFi transport (`xport_wifi.c`) is unchanged: TLS to `ident` host/port regardless of SIM.

### 3.4 Reporting

Encoder: `modes.c` `build_status_cbor()` (`STK_*` at `modes.c:635-656`, field count at
`:864`, `tls` from `catrust_state_name()` at `:848`, which must return `proxy` on the beam bearer).
Key 70 is free (PROTOCOL.md §10 ends at 69; `wirecbor.py` likewise). The relay's `tls` is a
`Literal["unpinned","pinned","broken"]` (`relay/app/wire.py:282`), so the relay change (§4) must
deploy before a firmware that can send `proxy`.

`/status` gains key **70 `car`** (tstr ≤ 24, the preset label, e.g. `Soracom`, `US Mobile Dark
Star`; absent when no preset is in force) and `tls` gains the value **`proxy`** (TLS terminated by
Beam, not on the pager). `xport` stays `lte`. The Device screen's Carrier row shows the label as
today; the boot splash's network status line says `via Beam` when the bearer is beam.

### 3.5 Debug console additions

- `mqtttest <host> <port> [ca|noneca|emptyca|plain]`: `plain` configures with profile 0 (the
  Beam-shape connection) so the hop can be probed independently of the session code.
- `bearer [auto|direct|beam]` (§3.1), `carrier` prints the bearer and auth in force.

### 3.6 Sleep, eDRX, latency

Unchanged in design. Liveness: the modem sends no PINGREQ (`xport_lte.cpp:40-47`); the host
re-SUBSCRIBE every 300 s of uplink silence (`xport_lte.cpp:57`, `:871`) runs from the bounded
light-sleep timer wake and crosses both Beam legs, inside 1.5 × 480 = 720 s. The bench confirms it
with a 1 h idle soak through Beam (`link` key 50 must not increment). Modem MO SMS is gated in one
place, `net_sms_send()` (`net.cpp:2690`), which returns false when `sms_mo` is false; both callers
(`sms.c:1379` send queue, `sms.c:1600` `smstest`) already audit a failure. The eval's open question is what the visited network grants a roaming LTE-M
device; the bench measures registration time, granted eDRX (`AT+CEDRXRDP`), and page latency on
the Soracom SIM with the direct bearer first (§6 step C) and then through Beam (step E). If eDRX
is refused, that is reported as a finding, not fixed in this work.

## 4. Relay and web

Minimal: the data path is unchanged (§2). What changes:

- `wire.py` `/status` model: `car: str | None` (key 70), `tls` accepts `proxy`. Stored on
  `devices.status.car` / `status.tls` like the other device-reported fields; display only.
- `apn_presets.py`: add `Soracom` → `soracom.io` so the admin APN picker offers it (the firmware's
  own detection is what actually sets PAP credentials; the preset keeps the UI honest).
- Web: Devices tables and the device page show a **Carrier** chip from `status.car`; the CA-trust
  column renders `proxy` as "via Beam"; the SMS panel is hidden/annotated for a device whose
  `tls` is `proxy` (device cannot originate SMS; relay SMS still works).
- PROTOCOL.md §5.1 (`car`, `tls: proxy`), §6.1 (bearer row: Beam plain MQTT, keepalive 480 inside
  Beam's 5…1200), §10 keymap (70). Server-architect owns this edit.
- Tools: `tools/pager_client.py` already speaks plain MQTT on 1883 and is the local stand-in for a
  Beam-side client; no change. New `tools/soracom_beam.py` (§5) and the production admin helpers
  `tools/prod_token.py`, `tools/prod_device_purge.py` (used to remove proto3; kept for the next one).

## 5. Soracom-side configuration (the one step that needs the owner's account)

Beam is configured per **Group** in the Soracom console (Global coverage, `g.api.soracom.io`):

1. Create a group, e.g. `pager-beam`.
2. Basic settings → SORACOM Beam → add **MQTT entry point** (`mqtt://beam.soracom.io:1883`):
   destination `mqtts://s1289801.ala.us-east-1.emqxsl.com:8883`, version **201912**, credentials
   **pass-through** (`useClientCredentials: true`; no username/password of Beam's own, no client
   certificate), enabled.
3. Add the pager's SIM to that group.

`tools/soracom_beam.py` does the same through the API when `SORACOM_AUTH_KEY_ID` and
`SORACOM_AUTH_KEY` (a SAM user auth key) are in the environment:
`auth → list/create group → PUT /v1/groups/{id}/configuration/SoracomBeam → PUT /v1/sims/{simId}/set_group`.
Nothing on this machine holds Soracom credentials and the console session in Chrome is signed
out, so **the headless run cannot do this step**. Everything up to the Beam hop is verified with the
direct bearer on the Soracom SIM; the Beam hop itself (§6 step E) runs as soon as the owner either
configures the group or exports the two variables and reruns the tool.

## 6. Headless bench plan (bench unit over USB; no owner at the desk)

Constraints from the bench memory: one serial reader per session, never reopen the port quickly,
never poll a sleeping release build, short bounded captures, debug builds never light-sleep so USB
stays up. The unit is fully erased, so every step starts with a full flash
(bootloader, partition table, otadata, assets at 0x11000, app at 0x120000).

- **A. Identity.** Flash the current `main` debug build. In Setup mode run `carrier` and
  `at +SQNCCID`, `at +CIMI`, `at +CRSM=176,28478,0,0,0` → IMSI, ICCID, GID1. Writes the Soracom
  PLMN prefix into the preset. (No code change needed first.)
- **B. Attach.** New debug build. `carrier` must auto-detect `Soracom`. `bearer direct`; provision
  from the production relay (`POST /api/admin/devices` → setup code → `setup <code>` over USB; the
  bootstrap goes TLS-direct over Soracom data). Expect `SETUP done`, reboot, `MQTT session usable`,
  `/status online` visible in production. Record attach time and `AT+CEDRXRDP`.
- **C. Messaging, direct bearer.** Send a page from the relay to the device, expect it on the
  serial log as shown; reply from the console; confirm both in Firestore. Measure delivery latency.
- **D. Release build, direct bearer.** Flash release; watch only from the relay side (status
  cadence, page delivery into sleep) for one hour; no serial polling.
- **E. Beam hop** (needs §5). `bearer auto` (= beam on this SIM): plain MQTT to Beam; same checks
  as B–C; `/status.tls` must read `proxy`, `car` `Soracom`. Then `mqtttest beam.soracom.io 1883
  plain` as the independent probe if anything fails. Re-provision through Beam once (bootstrap over
  Beam) to prove the setup path.
- **F. Regression.** The US Mobile SIM is in proto2, which is not on the bench; the direct path is
  covered by step B–D running the same image with `bearer direct`, plus host tests for
  `carrier_effective()`.

**Results, 8 Oct 2026 (bench unit, debug then release image, `bearer direct`):**
- **B.** `setup <code>` → `AT+CGAUTH=1,1,"sora","sora"` after `AT+CGDCONT`, registered roaming
  (`+CEREG: 5`) on 310410 (AT&T, LTE-M) in about 17 s, bootstrap MQTTS to EMQX usable 4 s later,
  CA fetch 2.6 s, `SETUP done` 26 s after the command. After the reboot the normal session was
  usable 32 s after boot (release build: 25 s). eDRX **granted** while roaming:
  `+CEDRXRDP: 4,"0010","0010","0001"` (20.48 s cycle, 2.56 s PTW). RSSI `+CSQ: 22..24`
  (about -69..-65 dBm). Production: `sora1` online, CA trust "Server verified", Carrier chip
  "Soracom" (`/status` key 70 end to end).
- **C.** Two web→pager pages arrived 1..6 s after Send, each with `shown` published within 1 s
  and "read on pager" in the web; two console-typed replies reached the web chat ("sent to app")
  within seconds. (Console note: Enter is `key \\n` on the wire, bursts ≤ 8 keys; see
  HARDWARE_TESTING.md.)
- **D.** Release image (718,368 B) flashed over the debug one (app + otadata); boot banner showed
  carrier Soracom / PAP, `boot complete, entering sleep mode` at 19.8 s, session usable at 24.6 s,
  `/status online (mode=sleep)`; port closed. One-hour relay-side watch (15:42-16:42 PDT):
  pages at +10 and +40 min both "on pager" within a minute of Send (delivered into eDRX
  sleep), battery samples every ~25 min at 3700 mV, device page after the hour online with
  sleeps 152, reconnects 2 (the two deliberate reboots), ext1 wakes 0.
- **E.** Not run: the Soracom console is signed out on the bench Mac and no API key is present,
  so Beam is not configured for the SIM's group (§5 is the owner's step).

Definition of done: steps A–D pass on the bench unit; relay tests, web lint/tsc/build, host tests
and both firmware builds pass; the Beam step E is either passed or left with the exact owner
action written in `.overnight-handoff.md`. Commit and push to main (owner's standing rule for
overnight work; production holds test data only).

## 7. Flagged decisions (owner can reverse any of these later)

1. **Pass-through credentials, not IMSI-injected.** Keeps EMQX users and the relay unchanged. The
   eval's "credentials can leave the pager" idea is not taken; nothing is lost by keeping them.
2. **AEAD bodies (eval's condition for Soracom) are not in this work.** They are a protocol
   change on both ends; tracked in ROADMAP. Until then, Soracom and the roaming carrier can read
   page bodies on the Beam bearer. The HMAC still protects integrity.
3. **Bench unit re-registers as a new device id `sora1`** ("Soracom Walter", family `default`,
   owner `admin`) instead of reusing `proto3`; proto3 and all its Firestore/EMQX leftovers are
   purged (`tools/prod_device_purge.py`).
4. **Beam host and port are compile-time constants.** They are Soracom's fixed entry point, not
   per-deployment data.
5. **Soracom detection is by IMSI prefix read on the bench**, like the existing preset; if a later
   Soracom SIM has a different prefix the owner picks `Soracom` from the Carrier menu, which is the
   fallback the carrier design already has.
