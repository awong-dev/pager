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
unmatched and the read timed out; SMS and SIM-file reads did not exist; the synchronous
command wait was untimed, so one slow command (`AT+COPS=0` during a search: up to 3 × 30 s)
blocked every other task's next command behind it and tripped the 60 s task watchdog (patch
1.10 feeds the watchdogs for up to 95 s while blocked, then lets a wedged modem reset the chip). **A single MQTT message
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
- **Never run `npm install` for the web app on a Mac.** It prunes Linux-only optional
  dependencies (`@emnapi/core`, `@emnapi/runtime`, sharp's wasm fallback) from the lockfile, and
  the deploy's `npm ci` on Linux then fails while the relay half of the same push deploys. Change
  dependencies inside the `node:24` image: `docker run --rm -v "$PWD":/work -w /work node:24 npm
  install <pkg>`. This bit twice.
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

## A stray 0xFF byte on every wake from light sleep

**Symptom:** pages delivered 191–237 s late or never; when delivered, probes answer in 4 ms or not at all; unpaired response lines appear as `rsp_no_cmd` counter.

**Cause (resolved 24 Sep 2026):** the modem UART emits exactly one 0xFF byte in the millisecond when the host reconfigures flow control on wake. The vendored parser assumes messages start with "\r\n" and searches for trailing CRLF from byte 2, so 0xFF glues to the next line: "\xff\r\nOK" (unmatched probe) or "\xff\r\n+SQNSMQTTONMESSAGE:..." (unrecognised notification, lost for good).

**Fix:** library patch 1.18 drops a leading 0xFF when the buffer is empty and no payload is expected, and counts dropped bytes in `glitch_dropped`.

**How to see it:** flash the debug build and run `sleeptest`. The flight recorder logs every UART byte: `flightrec` dumps the PSRAM ring, `flightrec clear` resets it.

## USB and light sleep

**USB dies in light sleep** and often does not come back afterwards, even across `esp_restart()`; on the bench it stayed dead for hours and needed a physical reset. The debug build's `sleeptest` now ends with a reset through the RTC watchdog, which resets the USB block too (UNVERIFIED that this brings the port back). A single serial capture spanning a restart shows nothing; open the port again afterwards. The port's name changes (`/dev/cu.usbmodem101`, `...1101`): always glob.

**15-minute hold on exit:** the debug build holds the CPU running for 15 minutes after a `sleeptest` window ends (printing a "still running" reminder every 60 s), so a USB replug can retrieve the flight recorder PSRAM dump with the `flightrec` console command before the next hard reset.

## Display

**Symptom:** the screen freezes on one image while the log shows screens changing and
`disp: BUSY: entry=0 exit=0 iters=0 elapsed=12 us` on every refresh. A healthy refresh shows
`entry=1` and 450 ms (partial) or 1.4-3.4 s (full). **Cause:** the panel's BUSY wire (IO18, Walter
pin 22, next to the keyboard's pins) was loose, so the driver believed each refresh had finished
at once and cut the panel's power before it drew. The driver now waits a fixed 700 ms / 3.5 s
when BUSY never rises, and logs "BUSY line never asserted" once per boot, so the symptom becomes
a slow display plus that line. Bench-found 22 Sep while wiring the CardKB.

E-paper partial refreshes ghost. A message that *changes the screen* (greeting to chat) takes a
full refresh; messages into an open chat stay partial.

## The SSD1680 loses registers under LTE bursts

**Symptom:** a torn full refresh, then every refresh garbled / half black, `dirty_rows=0` on the
host. A bare `disptest swreset` reproduces it.

**Cause:** the SSD1680 loses its register configuration during LTE uplink bursts (three events
22-23 Sep); RAM retained. The init sequence (SW reset 0x12 + registers) restores it.

**Rule:** re-send the init registers before every refresh (07995ff). Now a garbled panel that a
reboot fixes is register loss, not a driver bug. The firmware holds display writes while a
publish is in flight, bounded 1.5 s (disp.c `disp_pre_write_gate_hook()`), which gates the
uplink/write collision.

## Release build: publish payload replaced by command text

**Symptom:** relay logs `SECURITY bad-sig` with `first64=b'AT+SQNSMQTTPUBLISH...'` — the modem's
outbound payload is the text of the publish command instead of the message bytes.

**Cause (root-caused in `docs/RCA_SLEEP_PUBLISH.md`, release build only, one bench occurrence
"solved by accident" in `build/bench-logs/phaseW-prompt.log`):** NOT the device light-sleeping
mid-publish — the `/up` ack this bug hit is issued from `msg_pump()` on the modes task, and
`mqttPublish()` is synchronous, so that task is blocked inside it for the whole 30 s command
timeout and cannot reach `net_sleep()` at all (RCA §1). The real mechanism is in the vendored
walter-modem component: the modem's `"> "` data prompt arrives with its leading CRLF split from a
buffered residue by an interleaved URC or a light-sleep-boundary byte loss, so the library's parser
(`_parseRxData()`) never recognises it as the prompt (RCA §2) — the modem is left sitting at the
prompt, owed N payload bytes, with **no timeout of its own** on the prompt (confirmed on the bench:
it waited the full 30 s). The library's old retry logic then re-transmitted the same N-character AT
command line, which the modem consumed as the outstanding payload and published verbatim.

**Rule:** two vendored-component patches (`firmware/components/dptechnics__walter-modem/PATCHES.md`):
patch 1.11 recognises the bare, already-stripped `"> "` prompt so it is not orphaned in the first
place; patch 1.12 changes the timeout retry itself — a `DATA_TX_WAIT` command's first timeout now
sends the payload bytes (not the AT command line) in case the modem is still genuinely at the
prompt, and a second timeout fails the command outright rather than retrying blind a third time.
`net_publish_in_flight()`/`publish_quiet.h`'s skip-sleep hold (985a343, 23 Sep) is real but guards a
different, narrower window (the event-task publishes, e.g. `/status` on the incoming-page edge) —
see that function's own doc comment in `net.h` for what it does and does not explain. The debug
build cannot show this issue (`sleeptest`'s own light-sleep window is the closest analogue); catch
it in release soak tests, and watch the `datatx_retx`/`prompt_orphan` counters (PATCHES.md 1.12) if
it recurs.

## The SSD1680 two-plane differential update

**Symptom:** vertical bands about one character wide remain garbled after typing in the chat
composer stops — only a full refresh (at boot, or every 20th partial) resyncs them. Firmware-side
everything is right: keys arrive, the composer holds the text, the reply publishes. The panel
image diverges from the framebuffer on partial refreshes.

**Root cause:** the SSD1680 controller inside this panel has two image planes in RAM (new at 0x24,
old at 0x26). A mode-2 differential update compares the new plane against the old to decide which
pixels to refresh. The driver wrote the changed band to 0x24 and ran the update, but only
re-synced 0x26 afterwards — the two planes were left unequal. The next differential update on
an adjacent band then compared against a plane that never held the displayed image, so rows
unchanged by the host were never detected as changed again, and stayed wrong until the next full
refresh.

**Rule:** after a SSD1680 mode-2 partial update, both planes MUST be equal. Write the band to
plane 0x26 first, then to 0x24, each preceded by its own `set RAM window` (0x44/0x45) and
`set RAM counter` (0x4E/0x4F) sequence. Full refreshes must also re-set the window before
writing 0x26, because the address pointer has run to the end after 296 rows of 0x24 data.
Reference implementations: GxEPD2's `GxEPD2_290_T94.cpp` / `GxEPD2_290_T94_V2.cpp`,
function `writeImageAgain()`. Waveshare's own 2.9" V2 driver example documents why:

> there are 2 memory areas embedded in the e-paper display and once the display is refreshed,
> the memory area will be auto-toggled, i.e. the next action of SetFrameMemory will set the
> other memory area therefore you have to set the frame memory and refresh the display twice.

**Gotcha in bench testing `disptest seq`:** this harness does not touch any band below the
starting row, so with the default `seq 2 12` the two leftmost bars never change — easily
misread as "the leftmost band did not flip". Always start at `seq 0` when judging correctness
by eye, and predict the expected black/white bar pattern in advance (e.g. `w b w b w b ...`).
Reading the bars off the glass as a `b w b w ...` list and diffing that against the prediction is
what finally settled it; "looks right except one band" cost a whole round of chasing a defect that
did not exist.

## Tools and workflow

**`serial_capture.py` overwrites without warning:**
Old filename reused → appended a boot after an old one → the seam was read as a reset (d3ad364).
Always check the log file is fresh before starting a capture.

**esptool reset and `serial_capture.py` collide on the USB port:**
esptool's reset handshake and a running `serial_capture.py` cannot share the USB-Serial/JTAG
port; a collision parks the chip in the ROM bootloader (`boot:0x22 DOWNLOAD`). Recover with a
solo `esptool.py --after hard_reset chip_id`, then start captures in the same shell line as the
flash.

**`gcloud logging read` needs `--project kid-pager`:**
The shell's default project is another one; the read silently returns other services' logs and
misses relay output.

**An email-link sign-in deletes the account's password:**
Firebase removes a password from an account the moment that account completes an email-link
sign-in (documented: "any previous unverified mechanism of sign-in will be removed"). The 23 Sep
overnight group test signed in as the owner's two web accounts with
`generate_sign_in_with_email_link` + `signInWithEmailLink` to get ID tokens, and the owner's
password sign-in stopped working from that moment (`passwordHash` gone from the account export,
`validSince` = the test's sign-in time). Google's email pipeline is broken on this project, so a
password is the only way into the web app: never sign in as a real user with an email link. Test
with throwaway users created with a password, and restore a password with
`tools/set_web_password.py` (Admin SDK over ADC).

**Worktrees created by agents branch from origin/main:**
A worktree created by an agent branches from origin/main, not the local main; merge conflicts
against the cleanup and Makefile test lists followed (23 Sep). Push first, or create the
worktree by hand from main.

## Finding out what the modem really sends

Point it at a server you control and read the bytes:
`socat -v -x -u TCP-LISTEN:8883,fork,reuseaddr OPEN:/dev/null` on a host with a public name. A
TLS ClientHello is unencrypted, so its SNI is readable. Use `socat`, not `openssl s_server`, which
serves one connection at a time and gets parked on by port scanners. The console commands for
this are in `HARDWARE_TESTING.md`.
