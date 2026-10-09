# How the system works

A kid carries a small e-paper pager with a cheap data SIM. Family members message it, and get
replies and its location, from a web app (or by SMS or Google Chat). The design goal is weeks on a
charge and about a megabyte of data a month.

## The pieces

```
 web app ──HTTPS──▶ relay ──REST publish──▶ MQTT broker ──MQTT over TLS (LTE-M)──▶ pager
 (Next.js on       (FastAPI on            (EMQX Cloud                              (ESP32-S3 +
  Firebase          Cloud Run,             Serverless)                              Sequans GM02SP,
  Hosting)          Firestore)  ◀──rule engine webhook──┘                           e-paper)
```

The pager leg is MQTT over TLS from the modem, or on a Soracom SIM plain MQTT via Soracom Beam, which
opens the TLS leg to the broker (`docs/SORACOM_DESIGN.md`).

- **Pager** (`firmware/`): a DPTechnics Walter board (ESP32-S3 plus a Sequans GM02SP LTE-M/GNSS
  modem), a 296×128 e-paper display, a keyboard and an accelerometer. ESP-IDF, C with one C++ file
  (`net.cpp`) wrapping the vendor's modem library, which is vendored and patched in
  `firmware/components/`.
- **Broker**: EMQX Cloud Serverless. It authenticates each pager by username and password, limits
  it by ACL to its own four topics, and forwards what pagers publish to the relay through a
  rule-engine webhook. **The relay is not an MQTT client**: it publishes through the broker's REST
  API and receives through that webhook.
- **Relay** (`relay/`): the only thing with business logic. Users, allow-lists, routing, delivery
  backends, device provisioning, signing, location, retention. State is in Firestore.
- **Web app** (`web/`): a static Next.js export on Firebase Hosting. Firebase Auth for sign-in,
  the relay's API for writes, Firestore listeners for live updates.
- **Infra** (`infra/`): Terraform for GCP. The broker is configured by hand; there is no Terraform
  provider for it.

## Addressing

People are addressed by **alias**. A pager is not an addressee: it is one of its owner's
*delivery backends*, alongside the web app and Google Chat; the pager's texts go through the relay (see *Texting from the pager*). A message to a user fans
out to every enabled backend they have. An **allow-list** decides who may message, and separately
who may locate, whom; the relay enforces it, never the pager.

## A message to the pager

1. The web app calls `POST /api/conversations/{alias}/messages`.
2. The relay resolves the alias, checks the allow-list, stores the message and creates one
   delivery per backend.
3. For a pager backend it builds an envelope (`PROTOCOL.md` §3), adds a replay counter `n`, signs
   it with that pager's key, and publishes it QoS 1 to `pager/{id}/down`.
4. The modem's own MQTT client receives it and raises an event. The firmware reads the payload,
   verifies the signature and the counter, stores the message and draws it.
5. Only after the e-paper refresh completes does the pager publish a `shown` ack on
   `pager/{id}/up`; `read` follows when the person opens it. The broker's rule forwards both to
   the relay, which advances the delivery state the web app is watching.
6. Anything unacked is re-published when the pager next reports itself online, so a pager that
   was off catches up. Duplicates are recognised by message id and only re-acked.

Replies go the other way on `/up`, addressed with `to`. The pager also publishes `/status`
(online, battery, signal, firmware, trust and location state; an hourly heartbeat) and `/loc`.

## Wire format

Pagers speak **CBOR with integer keys** (`PROTOCOL.md` §10); the relay also accepts JSON, which
the Python test pager can use. The relay answers a pager in whichever encoding that pager last
used, and **defaults to JSON until it has accepted one envelope from it**. The firmware reads
only CBOR. Envelopes are at most 640 bytes. Unknown fields are ignored in both directions, which
is what lets firmware and relay be upgraded separately.

## Identity and setting a pager up

Nothing is flashed per device. An admin creates the device in the web app and gets a one-time
**setup code**, valid for ten minutes: a random token plus `@ broker-host`. It is typed into the
pager (`setup <code>` on the USB console today). From the token both sides derive, with HKDF, a
temporary broker login, a topic and an AES-256-GCM key. The pager attaches, connects to the broker
with that login, and receives a retained, encrypted **bundle**: its device id, broker password,
32-byte signing key, broker host and port, flags, label, and a pointer to the broker's CA (a URL
and a SHA-256). It stores these in flash, restarts, and comes up as itself. Rotating credentials
is the same flow with a new code.

## Security

- **Transport**: MQTT over TLS 1.2 from the modem to the broker. The pager pins the broker's root
  CA if it was given one. If validation then fails it retries once, falls back to an unvalidated
  connection so that pages keep arriving, shows a broken padlock and reports `tls: "broken"`; it
  tries validating again after every cold boot and once a day. A new CA can be pushed from the web
  app; the pager fetches it by URL, checks its hash, and only commits it after a validated
  connect (`V02_DESIGN.md` §4).
- **Messages**: every envelope in both directions carries an 8-byte HMAC-SHA256 tag under the
  pager's key, and a counter `n = (epoch << 20) | lo` (`PROTOCOL.md` §14). The epoch is 32 bits,
  lives in flash and rises on every cold boot; `lo` lives in RTC memory. This is what protects
  pages from the broker, from the webhook path, and from anyone on the path while the pager is
  running unvalidated. It gives integrity, not confidentiality.
- **Edges**: the broker's per-device ACL; a shared secret on the webhook; Firebase Auth on the
  API; Firestore rules that deny clients everything sensitive.

## Power

The modem stays registered with **eDRX** (a 20.48 s paging cycle, PSM off), so a page arrives
within about 20 s without the radio being on. The modem's built-in MQTT client owns the session
but sends no keepalive pings of its own, so the ESP32 re-subscribes to its topic every 300 s of
silence from a wake it takes anyway (`GOTCHAS.md`, `V02_DESIGN.md` §9). The ESP32 light-sleeps,
waking every 5 s (or on a deliberate shake) for about 200 ms to collect events held by the modem. The
modem does queue events; a 50 ms wake window is too short to receive them (pages vanish), while
150 ms succeeds and 200 ms is in use (4% awake, against the 1% the design assumed).

After losing coverage, the radio runs a duty cycle: 3 minutes unregistered, then off for 2/5/10/15
minutes (capped) with a 150 s search window between. Any registration resets the backoff; sustained
motion resets it too.

The data budget is dominated by TLS reconnects (about 5 kB each), not by messages:
roughly 1-2 MB a month nominal (`PROTOCOL.md` §7).

## Carrier APN

The pager chooses its APN itself, from the SIM, the way Android does: a small table in the
firmware (`firmware/main/carrier.c`) matches the SIM's network code and its `EF_GID1` group
identifier to a carrier. A person can override it on the pager. A Soracom SIM also selects PAP APN
auth and the Beam bearer (`SORACOM_DESIGN.md`). See `GOTCHAS.md` for why a blank
APN is not a safe default.

## Location

A `loc_req` from the relay is answered on `/loc`, always, without waking the display. GNSS and
LTE cannot run at once on this modem, and a school pager is usually indoors, so attempts are
short (20 s), request-driven, and followed by a growing backoff after a failure (5 minutes
doubling to 12 hours). Inside the backoff the pager answers immediately from its last fix or with
`no_fix`. A change of cell, or sustained motion from the accelerometer, resets the backoff
(`V02_DESIGN.md` §5, `PROTOCOL.md` §13). On a `no_fix` answer the pager also sends its serving
cell, and the relay turns that into a coarse position through a pluggable third-party lookup
(`relay/app/cellgeo.py`); a `loc.src: "cell"` fix is always relay-produced, never something the
pager itself computes.

## Texting from the pager

Texts go through the relay and the member's bridge phone (below), which carries the member's SIM or
Google Voice number (`RELAY_SMS_DESIGN.md`). Twilio was removed 9 Oct 2026 (last at 05ec3ed), so a member
without a bridge phone has no SMS. A text to or from a known, approved contact is delivered. A text from anyone else is held and the
family's admins are alerted; a held text reaches the pager only after an admin approves the sender.
The modem's own SMS path is dead on the US Mobile line (7 Oct 2026), so it is not used there.

## Bridge phones

A bridge phone is a headless Android phone running the app in `bridge-android/`. It carries a
member's texts and Google Chat (`BRIDGE_PHONE_DESIGN.md`). It is signed into the
member's own Google account and nobody uses it. The relay holds no connection to it: the phone
polls the relay for outbound items and posts inbound messages.

- **SIM.** The phone's SIM number becomes the member's `smsNumber`. Texts to and from it go through
  the phone as ordinary SMS, which needs the phone to be the default SMS app.
- **Voice-only.** A phone with no SIM can carry the member's Google Voice number instead. Texts then
  go through Google Voice, and the relay can start a new thread to any number, because it sends the
  Voice thread link. A bridge may carry both numbers; the SIM is then the member's `smsNumber`.
- **Google Chat.** A message from a conversation nobody has subscribed to is held, and the admins
  get one `chat_unknown` alert. Subscribing (Family → Google Chat) puts the conversation on the
  pager: a group becomes a group with each writer's roster nickname as the sender, and a DM becomes
  a contact. Waiting messages are delivered in order, and later ones follow. The owner sets whether
  the kid may reply, and can rename, pause, unsubscribe or ignore the conversation. A conversation
  can also be added by pasting its link.
- **Replies, two tiers.** Tier 1 fires the reply action on the Chat or Voice notification, which
  needs no screen. If that action is gone (after a reboot, or after the notification was dismissed),
  tier 2 opens the conversation's stored link, types the reply through the phone's accessibility
  service, and goes home.
- **Delivery.** A pager text to a bridged contact shows as waiting for the phone until the phone
  acks it. An item the phone has not taken within 24 h fails.
- **Status.** Built and tested against a simulator. The app has not run on a phone yet. Open items
  are in `docs/ROADMAP.md`.

## Firmware updates

A super admin pushes a build from the web app's Devices page. The relay sends the
pager a signed `cfg.ota` job, and the pager downloads the image over HTTPS from a public bucket,
checks its SHA-256 and switches to it. If the new image does not confirm itself after boot, the
pager rolls back to the old one (`OTA_DESIGN.md`).

## Where to look

| To change | Look in |
|---|---|
| A wire field | `PROTOCOL.md` first; then `relay/app/wire.py`, `wirecbor.py`; `firmware/main/msg.c`, `cbor.c` |
| Routing, allow-lists, backends | `relay/app/routing.py`, `relay/app/backends/` |
| Provisioning | `relay/app/devsetup.py`, `routers/admin.py`; `firmware/main/setup.c`, `ident.c` |
| Modem, TLS, MQTT, sleep | `firmware/main/net.cpp` (the only file that touches the modem library) |
| The main loop and modes | `firmware/main/modes.c` |
| Screens | `firmware/main/ui.c`, `scr_*.c`, `gfx.c`; fonts are built by `tools/mkassets.py` |
| Deployment | `infra/README.md` (runbook), `.github/workflows/deploy.yml` (push to `main` deploys) |
| A simulated pager | `tools/pager_client.py`; the end-to-end suite is `tools/e2e_v2.py` |
| Bridge phones | `relay/app/routers/bridge.py`, `relay/app/backends/bridge.py`, `relay/app/chat_subscribe.py`; the phone app in `bridge-android/` |
