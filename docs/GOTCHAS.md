# Gotchas

Each of these cost real time on real hardware. Symptom first, because that is what you will see.

## The silent pager

**Symptom:** the web app says "sent", the broker shows the pager connected, nothing appears.

The relay learns a pager's wire encoding from the pager's own traffic, and sends JSON until it has
accepted one envelope from it. The firmware reads only CBOR. So *anything* that makes the relay
reject the pager's `/status` also makes every page undeliverable, and they all look the same from
outside. Check the relay's log for these three lines; each names a different cause:

| Relay log | Cause |
|---|---|
| no `POST /webhooks/mqtt` at all | the broker rule is missing or its key is wrong (below) |
| `SECURITY bad-sig` | pager and relay disagree about signing; the bundle's `flags` must follow the device's `authMode` |
| `SECURITY replay` | the pager's counter went backwards; the epoch must rise on every cold boot |
| `malformed payload ... ts out of range` | the modem's clock was unset (next section) |

On the pager, a rejected `/down` message is logged as `malformed down message dropped (N bytes,
first byte 0x..)`. A first byte of `0x7b` is `{`: the relay is sending JSON.

## The carrier APN

**Symptom:** the pager attaches and gets an IP address; small plain TCP works; every TLS
handshake, to any host, stalls for 30 s. Looks exactly like "the carrier blocks TLS".

**Cause:** attaching with a blank APN lets the network choose one, and on a US Mobile "Dark Star"
SIM (an AT&T reseller) that default is a crippled data path. It also refuses the requested eDRX.
With the APN `ereseller` the same SIM connects in seconds and is granted eDRX.

**Rule:** never trust a blank APN. The pager picks the APN from the SIM
(`firmware/main/carrier.c`): network code from the IMSI plus a prefix of `EF_GID1`, the same
match Android's `apns-conf.xml` uses (US Mobile documents "MVNO type GID, value 20FF"). The SIM
itself stores no APN. A wrong explicit APN means no data at all, so a table entry must match
narrowly, and near misses are tested (`firmware/host/test_carrier.c`). Precedence: `;apn=` typed
with the setup code, a choice made on the pager (`carrier` console command, Device → Carrier),
detection, the bundle's APN, blank.

## Pages never arrive while the pager sleeps

**Symptom:** pages sent during a sleep window do not appear during that boot; they arrive in a later boot.

**Cause:** the ESP32 light-sleeps with RTS deasserted, and the modem holds `+SQNSMQTTONMESSAGE` URCs. But the URCs are held until the device is awake and ready to receive them; with a 50 ms wake window after each 5 s sleep cycle, the modem never hands them over because the window is too short.

**Rule:** the post-wake window must be long enough for the modem to deliver all held events. Testing showed 50 ms is too short (two pages were lost entirely in one 4-minute window), while 150 ms worked (pages delivered 35 s and 136 s after sending). The firmware now uses 200 ms (4% awake, against the 1% the design assumed).

UNVERIFIED: the exact minimum (not bisected between 50 and 150 ms), why one delivery took 136 s, and whether a bare `AT` right after wake would allow a shorter window.

## Pages stop arriving after about ten minutes, then resume much later

**Symptom:** the web app shows the pager online; new pages vanish; the pager's log shows no disconnect; the session comes back by itself several minutes later.

**Cause (measured 2026-09-21):** **the modem never sends PINGREQ**, whatever keepalive is configured. Three idle sessions at keepalive 480 s: the broker's `recv_pkt` for the client stayed at 1 (the CONNECT) for the whole session, so nothing arrived at the 8-minute mark (`build/bench-logs/r2-keepalive-poll.log`). The AT manual says the keepalive argument "controls the rate at which the client sends ping messages"; on `LR8.2.1.0-61488` it does not. So **lowering the keepalive is not a cure** — it only moves the broker's own 1.5 × keepalive deadline. On top of that, something in the carrier path kills an idle flow after 10-13 minutes regardless: at keepalive 1800 s sessions still died in 10-13 min, not 45 (assumed NAT, INFERRED).

Worse, after the modem resumes the session by itself (`+SQNSMQTTONCONNECT:0,0`, no connect command from us) the pager is **subscribed to nothing**: the AT manual requires the host to re-subscribe, and `mqttSubscribe()` silently does nothing when the topic is still in its local table. No `AT+SQNSMQTTSUBSCRIBE` was sent for either resume in a 40-minute log. No `+SQNSMQTTONDISCONNECT` is ever raised either, so the firmware never notices any of it.

**Rule:** the host keeps the flow warm. Every 300 s of uplink silence the ESP32 re-subscribes to `pager/{id}/down` with a raw `AT+SQNSMQTTSUBSCRIBE` — the SUBACK proves the round trip and re-arms the subscription after a silent resume. No SUBACK in 30 s means the session is dead: reconnect. Cost about 28.8 mAh/day (288 pings x ~0.1 mAh, estimate). See `V02_DESIGN.md` §9.

UNVERIFIED: the per-ping energy, the carrier's true idle timeout (and the largest safe interval), and behaviour on other carriers such as T-Mobile.

## The pager dies after losing signal

**Symptom:** the pager works normally; it walks into an area with no coverage; after coverage returns, the pager is not reachable and will not send or receive pages until a power cycle.

**Root cause (now fixed):** a health check used to treat "no signal" as "modem unresponsive" and hard-reset the modem, which erased its MQTT and TLS configuration. With no coverage, the re-init gave up and nothing redid it.

**Rule:** losing coverage must never reset the modem. Radio bring-up and session configuration are now separate. Regained coverage triggers an immediate reconnect (an existing session is torn down first, because it may be silently dead), and the modem is reset only if it stops answering or after 30 minutes with no network. Verified on hardware: radio off for 70 s and 12 minutes, then on; no reset, `network coverage regained - reconnecting the MQTT session now`, session usable 3 s later, page delivered afterwards.

## The modem's MQTT client and TLS

- **A TLS profile that names no CA slot makes the MQTT client send plaintext.** With
  `AT+SQNSPCFG=2,2,"",0,,,,` the modem sends an unencrypted MQTT CONNECT, password included, to
  port 8883. The broker waits for a handshake, the modem for a CONNACK, and no event ever fires.
  The generic socket layer does TLS with the same profile, so socket tests do not show it.
  **Every TLS profile used for MQTT names a CA slot, even with validation off**, and the firmware
  keeps a placeholder certificate in that slot so it is never empty.
- The third field of `AT+SQNSPCFG` is the cipher list, not SNI. The modem sends SNI by itself.
- TLS profile 1 belongs to the vendor's BlueCherry service. This project uses 2 for MQTT and 3
  for the CA fetch. An existing `sdkconfig` keeps an old `WALTER_MODEM_MAX_TLS_PROFILES`; it is
  pinned in `sdkconfig.defaults`.
- The modem reports `-8` for a bad certificate *and* for any other failed handshake, so the
  broken padlock means "server not verified", not "attack".
- A connect can wedge with no event at all. There is a 60 s watchdog; three in a row reset the
  modem.
- Right after attach `AT+CCLK?` can still answer `70/01/01`, which the library reads as 2070. The
  firmware accepts only 2024-2069, retries, and falls back to `ts: 0`.

## The vendored modem library

`firmware/components/dptechnics__walter-modem/` is a patched copy of v1.5.0; `PATCHES.md` lists
every change, each marked `PAGER PATCH` in the source. The ones to know about: a result line with
no command pending used to crash the pager; receive paths could read and write past the 1540-byte
buffer; only socket id 1 could be looked up; a payload ending in a newline left the final `OK`
unmatched and the read timed out; SMS and SIM-file reads did not exist. **A single MQTT message
larger than about 1.5 kB cannot be received**, which is why the CA travels as a URL and a hash
and not inside the setup bundle (a Let's Encrypt root is 1.9 kB).

## Secrets and the broker rule

- **Never store a secret with a trailing newline.** `echo value | gcloud secrets versions add`
  does. An HTTP header can never carry one, so the webhook key never matched and every webhook
  got 401. Use `printf '%s'`. The relay strips whitespace now, but fix it at the source.
- **The broker rule exists only in the EMQX console.** It must forward `pager/+/up`,
  `pager/+/status`, `pager/+/loc` and `pager/boot/+/up`, and its SQL **must** include
  `base64_encode(payload) as payload_b64`, because EMQX's JSON encoder destroys CBOR. The exact
  SQL and headers are in `infra/README.md` §10. A rebuilt deployment has to redo it by hand.
- CI applies Terraform with values from GitHub repo variables. `BROKER_CA_PEM_FILE` and
  `PUBLIC_BASE_URL` must be set there, or a deploy silently un-pins new setup codes or refuses to
  issue them.
- **Deploy the relay before flashing firmware that adds fields.** A relay older than v0.2 drops a
  whole envelope over one unknown CBOR key.

## Firmware build and flashing

- ESP-IDF's `export.sh` picks the newest `python3` on the path; the installed IDF environment is
  for 3.11. Put a 3.11 `python3` first on `PATH` before sourcing it.
- **A sleeping pager cannot be flashed**: its USB port is dead in light sleep. Hold BOOT and tap
  RESET, or retry until a wake catches. The debug build never sleeps.
- `PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build` makes the debug build. The define comes
  from an environment variable at configure time, so switching needs `reconfigure`.
- Opening the serial port with DTR and RTS forced low resets the chip. A plain open does not.
- Anything holding a 4 kB identity struct must be `static`: it overflowed the main task's stack
  the first time a pager actually had an identity.
- The console exists in Setup mode, and in the debug build. To return a pager to Setup mode,
  erase its settings: `esptool.py --chip esp32s3 erase_region 0x9000 0x6000`. Fonts are a separate
  partition at `0x11000`, built by `tools/mkassets.py`.
- Noto Sans is the text face. Literata was tried on the real panel and rejected.
- **USB and light sleep:** the USB-Serial-JTAG port dies in light sleep and often does not come
  back afterwards, even across `esp_restart()`; on the bench it stayed dead for hours and needed
  a physical reset. The debug build's `sleeptest` now ends with a reset through the RTC watchdog,
  which resets the USB block too (UNVERIFIED that this brings the port back). A single serial
  capture spanning a restart shows nothing; open the port again afterwards. The port's name changes
  (`/dev/cu.usbmodem101`, `...1101`): always glob.

## Display

E-paper partial refreshes ghost. A message that *changes the screen* (greeting to chat) takes a
full refresh; messages into an open chat stay partial.

## Finding out what the modem really sends

Point it at a server you control and read the bytes:
`socat -v -x -u TCP-LISTEN:8883,fork,reuseaddr OPEN:/dev/null` on a host with a public name. A
TLS ClientHello is unencrypted, so its SNI is readable. Use `socat`, not `openssl s_server`, which
serves one connection at a time and gets parked on by port scanners. The console commands for
this are in `HARDWARE_TESTING.md`.
