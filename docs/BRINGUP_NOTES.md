# First real bring-up: what was wrong and what fixed it (2026-09-19/20)

This replaces `BROKER_HANDOFF.md`, whose root cause (missing SNI) and whose plan (self-host EMQX
on an Orange Pi) were both wrong. Everything here was verified on the real device (ESP32-S3
Walter, Sequans GM02SP `LR8.2.1.0-61488`, Google Fi SIM) against the production broker (EMQX
Cloud Serverless). Tagged `v0.1`: a message typed in the web app reaches the pager's screen over a
CA-pinned TLS session, signed in both directions.

## The original symptom

`setup <code>` hung forever on `AT+SQNSMQTTCONNECT`: no completion URC, no error.

**It was never SNI.** The earlier note blamed an "SNI-carrying field" in `AT+SQNSPCFG` that the
vendor library hardcodes to `""`. That field is `cipherSpecs`; the command has no SNI field. The
`+CME ERROR: 50` from putting a hostname there just meant "not a cipher list". The modem sends
SNI on its own, from the hostname, on both the socket layer and the MQTT engine (seen on the
wire).

## How the real cause was found

Point the modem at a server we control and read the bytes. `socat -v -x -u
TCP-LISTEN:8883,fork,reuseaddr OPEN:/dev/null` on a machine with a public hostname dumps whatever
each client sends; a TLS ClientHello arrives unencrypted, so its SNI is readable as text. Use
`socat`, not `openssl s_server`: `s_server` serves one connection at a time, and port scanners
park on it. Two temporary console commands drive this: `nettest <host> <port> [udp|tls]` (socket
layer) and `mqtttest <host> <port> [ca|noneca|emptyca]` (the modem's own MQTT engine).

| TLS profile given to the MQTT engine | What it put on the wire |
|---|---|
| validation none, **no CA slot named** (`AT+SQNSPCFG=2,2,"",0,,,,`) | **plaintext MQTT CONNECT, credentials included, to port 8883** |
| validation none, CA slot 12 named | TLS 1.2 ClientHello with SNI |
| CA validation, CA slot 12 | TLS 1.2 ClientHello with SNI |

The bootstrap profile was the first row. A TLS-only broker waits for a ClientHello, the modem
waits for a CONNACK, and nothing ever fires. The generic socket layer does TLS either way, which
is why socket tests never reproduced it. **Rule: every TLS profile used for MQTT names the CA
slot, even with validation off.** `UNVERIFIED`: what happens when that slot is empty, as on a
factory-fresh modem (`mqtttest ... emptyca` exists to test it).

## Everything that stood between the web app and the screen

| # | Problem | Where | Fix |
|---|---|---|---|
| 1 | Plaintext fallback above | firmware `net.cpp` | name the CA slot in both profiles |
| 2 | Main-task stack overflow as soon as an identity existed (4 kB `ident_t` on a 3.5 kB stack; setup had never completed before, so it had never run) | firmware | static copies, 16 kB main stack |
| 3 | No broker rule forwarding device topics to the relay | EMQX console, by hand | see `infra/README.md` §10 step 3 for the exact SQL |
| 4 | `WEBHOOK_KEY` stored with a trailing newline, so every webhook got 401 | Secret Manager | re-added without it; relay strips whitespace now |
| 5 | Setup bundle said `flags=0` for an `hmac` device, so the pager never signed and the relay dropped everything as `bad-sig` | relay `admin.py` | flags follow `authMode`; regression test |
| 6 | Counter epoch not bumped on cold boot (`DEVICE_PLAN.md` §2.5 said it must be), so everything after a power cycle was a replay | firmware `modes.c` | bump on cold boot |
| 7 | Unset modem clock (`70/01/01`) read as the year 2070; relay rejected the timestamp | firmware `net.cpp` | accept 2024..2069 only, retry, fall back to `ts:0` |

Symptoms 5-7 all looked identical from the outside ("message sent, nothing on the pager"), because
the relay learns a device's wire encoding from the device's own traffic: until it accepts one
envelope it keeps sending JSON, which the firmware cannot parse and drops at debug log level.

## Decisions taken along the way

- **The CA is optional, default pinned for production.** A bundle with an empty `ca` runs with
  validation off; one with a CA pins it. See `DEVICE_PLAN.md` §3.3. Production pins DigiCert Global
  Root G2 via `broker_ca_pem_file` (`infra/envs/prod`), and CI needs the matching repo variable.
- **No self-hosted broker.** EMQX Cloud Serverless works.
- **Noto Sans stays.** Literata was tried on the real panel and rejected (`tools/mkassets.py`).

## Still open after v0.1

- Delivery while the pager is in light sleep has never been tested (`PROTOCOL.md` §8.3, M5). All
  testing used the `PAGER_DEBUG_NO_LIGHT_SLEEP` build. A sleeping pager's USB port is dead, so
  reflashing one needs BOOT+RESET or a lucky retry.
- ~~US Mobile "Dark Star" (AT&T) SIM: TLS sockets did not work.~~ **Explained 2026-09-20: it was
  the APN.** The firmware attached with a blank APN and the network picked a default one that
  gives a crippled data path: a 13-byte plain TCP send works, a 300-byte HTTP request gets no
  reply, and every TLS handshake (port 443 to Google included) stalls for 30 s. It also refused
  the requested eDRX. With the APN set to **`ereseller`** (US Mobile's documented Dark Star APN,
  lowercase) the same SIM connects MQTT over TLS in 5 s, is granted the requested 20.48 s eDRX,
  and fetches over HTTPS normally. Registration still takes about two minutes at this location.
  **Lesson: never attach with a blank APN and trust the result.** The APN is needed before anything
  can be fetched, so the choice lives **on the pager** (`firmware/main/carrier.c`), the way
  Android does it: a table in the firmware keyed on the SIM's network code (first six digits of
  the IMSI) plus a prefix of its `EF_GID1`. US Mobile documents "MVNO type GID, value 20FF" for
  Dark Star; the bench SIM reads network `310280`, GID1 `20FF`, and the pager now picks
  `ereseller` by itself (seen on hardware: `SIM: network 310280, GID1 20FF -> US Mobile Dark
  Star`). The SIM holds no APN of its own (no `EF_ACL`, no operator-name file). Precedence: an
  `;apn=` typed with the setup code, then a fixed choice made on the pager (`carrier` console
  command in Setup mode and in the debug console, or Device → Carrier), then automatic detection,
  then the APN from the setup bundle, then blank. The relay can also store an APN per device
  (`PUT /api/admin/devices/{id}/apn`, presets at `GET /api/admin/apn-presets`), which goes into
  that device's typed code and bundle. Adding a carrier = one line in `carrier.c`'s table, with a
  source for the APN and its match data.
- Vendor library: a stray `NO CARRIER` with no command pending dereferences a null command and
  reboots (`WalterModem.cpp`, `_finishModemCMD`); receive buffers are 1540 bytes and
  `mqttReceive()` copies out of them unchecked (the setup bundle with the DigiCert root is 1468).
- Parked plans: `CA_TRUST_PLAN.md`, `LOCATION_PLAN.md`.

## Practical notes

- ESP-IDF's `export.sh` picks Homebrew's newest `python3`; the installed IDF environment is for
  3.11. Put a 3.11 `python3` first on `PATH` before sourcing it.
- The pager's console exists only in setup mode. To get back there, erase NVS:
  `esptool.py --chip esp32s3 erase_region 0x9000 0x6000`. The assets partition is at `0x11000`.
- Relay logs: `gcloud logging read 'resource.labels.service_name="pager-relay"' --project
  kid-pager`. `SECURITY bad-sig`, `SECURITY replay` and `malformed payload` are the three lines
  that explain a silent pager.
