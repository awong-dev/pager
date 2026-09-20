# DEVICE_PLAN.md — device provisioning, device authentication, and the on-device UI

**Status:** design plan, nothing here is implemented. The task-by-task execution plan derived
from it is `docs/DEVICE_TASKS.md`. `docs/PROTOCOL.md` stays authoritative for the
wire; every wire change this plan needs is listed in §8 as an edit to that document and is made
there *first* when the phase that needs it starts. `docs/SERVER_PLAN.md` is the reference for the
server side this plan extends.

**Scope.** Six device-UI requirements and one security requirement:

1. status indicator for battery and LTE signal;
2. a device screen that shows and completes provisioning of *this* device;
3. an address book the student can add recipients to, including SMS numbers, where **the server
   approves every entry** before it becomes routable;
4. recipient selection when composing;
5. selection among active chats;
6. inside a chat, scrollable history and a composer;
7. a stronger authentication and authorization scheme between device and relay than the MQTT
   password the broker checks today, sized against bytes per message;
8. a device lock with a passcode, auto-locking after a configurable number of minutes.

**Conventions.** Same as `PROTOCOL.md`: `(see rationale)` marks a choice made here; `NEEDS HUMAN
DECISION` marks something that needs a paid service, an irreversible step, or a product judgement;
`UNVERIFIED` marks a hardware or vendor-library fact assumed but not confirmed, each with the
cheapest experiment. Line references to the vendor component are against `walter-modem` **v1.5.0**.

---

## 1. Where things stand (what this plan builds on)

Read from the working tree on 2026-09-15. These are the facts that constrain the design; none of
them is a criticism of the code, which is a faithful implementation of the v1 protocol.

**Trust today.**
- The broker is meant to authenticate the device by username/password over TLS and to enforce
  per-device topic ACLs (`PROTOCOL.md` §2). **Neither is provisioned anywhere**: `tools/e2e_v2.py`
  and `infra/README.md` both say the local EMQX accepts anonymous connections and enforces no ACLs,
  and `POST /api/admin/devices` (`relay/app/routers/admin.py:242-277`) stores a password hash it
  never pushes to the broker.
- The relay authenticates the *broker* with one shared header (`X-Relay-Webhook-Key`,
  `relay/app/broker.py:105-115`) and then **trusts the device id parsed off the topic**
  (`relay/app/ingest.py:52-57`; `SERVER_PLAN.md` §5.3 says so explicitly). The only per-device checks
  are "device doc exists" and "not revoked". Anyone holding the webhook key, the broker's REST
  publish key, or broker admin access can therefore forge any device's `/up`, `/status` and `/loc`,
  and can publish any `/down`.
- Replay protection is idempotency only: `wireIds`, `locWireIds`, and monotonic delivery states.
  Those are bounded by retention (4 weeks by default), after which an old envelope replays cleanly.

**Firmware today.**
- Credentials, device id, broker host and the CA are `static constexpr` in `net.cpp:56-94` with a
  "do not flash as-is" comment. There is **no NVS** in the firmware at all
  (`firmware/main/CMakeLists.txt` requires only `json driver esp_timer`).
- The display driver, font, framebuffer, composer and one hardcoded screen all live in `ui.c`. The
  font is a hand-authored 5×7, **upper-case only** (`ui.c:73-75` upcases), 6 px advance → 49
  columns. There is no screen, scroll, cursor, invert or icon primitive.
- The CardKB is polled only while the composer is open (`ui.c:676-678`); every byte ≥ 0x80 (arrows,
  Esc, Tab on the CardKB) is dropped (`ui.c:703-729`). The one button multiplexes cancel /
  mark-read / open-composer.
- Messages are one flat 10-deep ring with no `to` field (`msg.h:88-98`); `publish_reply()` never
  emits `to` (`msg.c:511-534`). The device has no notion of a recipient, an alias, or a chat.
- `/status` omits `rssi`; nothing in `net.cpp` reads signal strength. Battery is read once per
  `/status` publish only.
- RTC memory: 928 B used of 1184 B → **≈256 B headroom** for anything that must survive a reset.
- Rendering runs synchronously on whichever task calls it, including the modem's event task
  (`firmware/README.md` R4, R5, R6). Any multi-screen UI has to fix that first.

**Server today.** Every recipient is a `users/{uid}` with a unique alias; a phone number is only ever
a delivery *backend* (`users/{uid}/backends/{bid}` with `kind:'sms'`) attached to a user, never a
recipient in its own right. The allow-list is admin-only, replace-all, with no pending/request
concept (`PUT /api/admin/allowlist`). `devices/{d}` is **owner-readable** under `firestore.rules`,
so nothing secret may be stored in it. The repo already has the two idioms this plan reuses: a
server-only collection with no rules `match` block (`phoneIndex`, `smsVerifyCodes`, …) and a
single-use code that is read-then-deleted (`gchatLinkCodes`).

---

## 2. Device authentication and authorization

### 2.1 Threat model, stated so the byte budget has something to be sized against

| # | Threat | Today | After this plan |
|---|---|---|---|
| T1 | Passive eavesdropping on the air / Internet path | TLS to the broker | unchanged |
| T2 | Someone with **broker admin / REST-publish access** forges a `/down` to the student ("pickup behind the mall") | possible | device rejects unsigned or badly signed `/down` |
| T3 | Someone with broker access or the **webhook key** forges `/up` (fake "on my way"), `/loc` (fake position), `/status` | possible | relay rejects; logged as a security event |
| T4 | Replay of a captured envelope after the dedup marker has been retention-swept | possible | per-device monotonic counter with a sliding window |
| T5 | Cross-device / cross-topic replay (device A's ack on topic B, an `/up` payload on `/status`) | wrong-device ack check only | key is per device and the topic is inside the MAC |
| T6 | Stolen device | full impersonation until revoked | unchanged — `revokedAt` + broker credential removal is the answer; NVS encryption raises the bar for key extraction (§2.7) |
| T7 | Relay database leak | leaks password hashes | leaks the per-device MAC keys (symmetric). Accepted; §2.8 says why, and what Ed25519 would buy |
| T8 | Someone who can talk to the broker guessing a device's key | n/a | 2⁻⁶⁴ per attempt, rate-limited and alarmed at the relay |

The broker's password + ACL stays as the **first** gate (it stops junk before it costs the relay a
Cloud Run invocation, and it is what limits who can even *attempt* T8). The signature is the
**second**, end-to-end gate that the relay verifies itself, so the broker and the webhook key stop
being trust anchors. Both gates are needed; neither replaces the other.

### 2.2 Options considered, with bytes

All sizes are the extra bytes on the payload, before TLS/TCP framing (which does not change: the
same number of records is sent). `n` is the replay counter of §2.4, present in every option except
the first. Two columns because the wire is **CBOR** for devices (decided; §2.4) and JSON stays for
tooling and debugging.

| Option | Extra bytes / envelope, CBOR | …JSON | Defends | Verdict |
|---|---|---|---|---|
| A. Broker password + TLS only (status quo, once actually provisioned) | 0 | 0 | T1 | not enough: T2–T5 all remain |
| B. **HMAC-SHA256, tag truncated to 64 bits** + counter | **≈ 15** (`n` 2–6, `sig` 10) | ≈ 33 | T1–T5, T8 | **chosen** |
| C. HMAC-SHA256, 128-bit tag + counter | ≈ 23 | ≈ 44 | same, 2⁻¹²⁸ | 8 bytes buys nothing once forgery attempts are rate-limited |
| D. Ed25519 signature + counter | ≈ 71 | ≈ 101 | B + T7, non-repudiation | 4–5× the tag bytes for a threat (relay DB leak) that already yields the broker keys and the whole thread store; a `/down` would need a relay signing key and the device a 32 B public key. Kept as the documented upgrade path (§2.8) |
| E. AEAD (ChaCha20-Poly1305), nonce = counter | ≈ 17 (bstr body, no base64 in CBOR) | ≈ 22 + ⅓ of `body` | B + confidentiality from the broker | body becomes opaque to the relay's pre-store validation; nothing in the threat model needs it yet. Same key, same counter, so it can be added later without re-provisioning (§2.8) |

*(see rationale for B over C: a 64-bit tag is the smallest size NIST SP 800-107 accepts without
extra conditions, the relay counts and rate-limits verification failures per device, and a forger
has to get each attempt past the broker's password gate first. And for not going **below** 8
bytes: 4 bytes would save ≈ 0.8 kB/day — under 0.5 % of an exchange once framing is counted — at
the price of a 2⁻³² forgery bound that is only acceptable if the relay's failure counter and
lockout are configured and never disabled, and a lockout an attacker who can publish as the
device can trip on purpose. Sixty-four bits needs none of that reasoning. And for not going
**above** 8: the only forgery oracle is the relay itself, online, so 2⁶³ expected attempts is
infeasible even with the failure counter removed, truncation length has no bearing on recovering
the 256-bit key, and OSCORE (RFC 8613) — the IETF security layer for exactly this class of
device — defaults to a 64-bit tag. Twelve bytes would spend the same 4 bytes per envelope as the
4-byte option, in the other direction, for nothing measurable.)*

### 2.3 Key material

- `K_dev`: 32 random bytes, generated by the relay when the device is created, returned **exactly
  once** in the provisioning bundle (§3), stored server-side in a **server-only** collection
  `deviceSecrets/{deviceId}` (§2.6) — never in `devices/{d}`, which the owner's browser can read.
- One key per device, used in both directions. The topic is part of the MAC input, which is what
  separates directions and topics; no derived per-direction keys *(see rationale: one HMAC per
  message and one 32-byte blob on the device is simpler than an HKDF step for a property the topic
  binding already gives).*
- The MQTT password stays a separate secret. It is the broker's business; rotating one does not
  require rotating the other.
- Rotation is a new setup code (§3.5): the person types it on the device and the bootstrap fetch
  replaces the key. Silent over-the-air rotation would need a `cfg` down message carrying a key
  encrypted under the old one; not designed here.

### 2.4 The wire encoding and the envelope fields

**Where the tag can live.** MQTT 5 user properties would be the natural home for a signature, but
the MQTT client lives inside the Sequans modem and speaks **3.1.1 only** (`PROTOCOL.md` §6.1,
resolved by source read: `AT+SQNSMQTTCONNECT` has no version or property argument). The payload is
the only place we control.

**CBOR, decided.** `PROTOCOL.md` §10 kept the JSON encoding CBOR-ready — short keys, an integer
keymap, first-byte dispatch, `v` untouched by an encoding change — and this plan spends it:
devices encode every envelope on `pager/{id}/…` as a **CBOR definite-length map with integer
keys**; the relay accepts JSON and CBOR on every inbound topic, dispatching on the first byte
(`0x7B` `{` vs `0xA0–0xBF`), and answers each device in the encoding of its last `/status`
(`devices/{d}.wire`). JSON stays for `tools/send.py`, the default of `pager_client.py`, logs and
humans. The rule that makes this a pure re-encoding: **values are identical to the JSON ones** —
same strings, same enums, same numbers (`lat`/`lon` as float64) — only keys become integers, and
`sig` is the one field whose type changes (a byte string instead of base64 text). `v` does not bump.

Two new fields:

| Field | CBOR | JSON | Where | Meaning |
|---|---|---|---|---|
| `n` | key 12, uint | `"n":<uint32>` | every signed envelope | Per-device, per-direction replay counter. Strictly increasing per publisher; verified with a sliding window (§2.5). |
| `sig` | key 13, bstr(8) | `"sig":"<11 base64url>"` | every signed envelope, **MUST be the last pair** | `HMAC-SHA256(K_dev, topic ‖ 0x00 ‖ P)[0:8]` where `topic` is the full MQTT topic (`pager/pgr-0001/up`) and `P` is defined below. |

**What exactly is signed, CBOR.** The publisher encodes the map with a header count that
*includes* the `sig` pair, writes every other pair, computes the tag over `topic ‖ 0x00 ‖ P` where
`P` is everything written so far, then appends exactly ten bytes: `0x0D 0x48 <8 tag bytes>`. The
verifier requires the payload to end in `0x0D 0x48 <8 bytes>`, MACs `topic ‖ 0x00 ‖ B[:-10]`,
compares in constant time, **then** decodes. No parsing before verification, no canonical form, no
re-serialisation: ten fixed bytes at the end of a static buffer. A device or tool that does not
verify decodes a perfectly ordinary map and ignores key 13.

**Same rule, JSON.** The publisher serialises without `sig` (that is `P`, ending in `}`), MACs it,
then emits `P[0:len-1]` + `,"sig":"` + base64url(tag) + `"}`; the verifier finds the trailing
`,"sig":"<11 chars>"}`, reconstructs `P` as the prefix plus `}`, and proceeds identically. `sig` is
therefore the only field with an ordering rule in the whole protocol in either encoding, and it is
a rule the producer trivially satisfies by appending.

**Sizes, CBOR with `n` and `sig`, against today's unsigned JSON (`PROTOCOL.md` §3.2 examples):**

| Envelope | Today, JSON, unsigned | CBOR, signed | JSON, signed |
|---|---|---|---|
| down message (99-byte example) | 99 | **75** | 132 |
| ack | 51 | **44** | 84 |
| up message (84-byte example) | 84 | **55** | 117 |
| `/status` (117-byte example, plus `rssi`, `bv`) | 117 | **≈ 78** | ≈ 175 |
| `/loc` (≈ 160) | 160 | **≈ 105** | ≈ 193 |
| maximal `/up` content message (`PROTOCOL.md` §3.3's 438) | 438 | ≈ 395 | 473 |

**Signed CBOR envelopes are smaller than today's unsigned JSON ones**, so the data budget of
§7.3 moves *down*, not up: the per-exchange totals of §7.2 are dominated by TLS/TCP framing, so
the saving is ≈ 5 % per exchange and the reconnect term is untouched, but the authentication costs
nothing net. The **640-byte limit is unchanged** and the headroom grows. The 640-byte static RX
buffer stays; `modes.c`'s `/status` buffer becomes 256 bytes for the JSON debug path.

**Firmware codec.** A hand-written CBOR subset in `cbor.c` (~300 lines: definite maps and arrays,
uint/negint, tstr/bstr, bool/null, float64) encoding into and decoding from a static buffer. It
replaces cJSON on the RX path, which is what closes README R12's "the RX path mallocs". The
registry component `espressif/cbor` (TinyCBOR) is the alternative if the subset grows; decision
H14. The relay uses `cbor2`; `tools/authvectors.json` carries both encodings of every vector so
the two codecs and the two signing rules are pinned against each other.

### 2.5 Replay window and counters

**Relay side, per device** (in `deviceSecrets/{d}`): `upN` (highest accepted `n`), `upBits` (64-bit
bitmap of the 64 values below it), `downN` (last `n` the relay issued). Accept an inbound `n` if
`n > upN` (shift the window) or `upN − 64 < n ≤ upN` and its bit is clear; otherwise drop as a
replay and log a security event. *(see rationale: the broker's webhook push is at-least-once and may
reorder retries, and a device can have an ack and a reply in flight at once, so "strictly greater"
would produce false rejections; 64 is the IPsec/DTLS convention and costs 8 bytes.)* The update is
a Firestore transaction on that one document; the same transaction is where the existing `wireIds`
dedup already lives, so this adds no round trip.

**Device side, `n` for `/up`, `/status`, `/loc`.** A 32-bit counter split as
`n = (epoch << 20) | lo`. `lo` lives in RTC memory and increments per publish (free). `epoch` lives
in NVS and increments **only** on a cold boot (RTC CRC invalid) or when `lo` wraps, i.e. an NVS write
a handful of times over the device's life. 12 bits of epoch = 4096 cold boots; 20 bits of `lo` = 1 M
envelopes per epoch. The relay's window absorbs the jump at each cold boot. RTC cost: 4 bytes.

*(To think about later, raised by the owner 2026-09-20; nothing here is decided or built.)* Should
the relay tell the device the last sequence number it saw, instead of the device bumping an epoch
in NVS? It can be made safe, but only as a **signed, fresh handshake**: the device sends a signed
hello carrying a random value, the relay answers with a signed message echoing that value plus its
`upN`, and the device resumes from there. A bare, unchallenged "your last n was X" is not safe: an
attacker replays an old one to wind the counter back, and every message captured after X becomes
replayable. Trade against the epoch bump: the handshake costs one round trip of airtime per cold
boot, new protocol and relay state, and the pager cannot send until the relay answers; the epoch
bump costs one NVS write per cold boot and nothing on the air, but is capped at 4096 cold boots
(12 bits), after which the device needs new credentials. About eleven years at one cold boot a
day; much less for a pager whose battery dies several times a day. A reasonable end state is to
keep the epoch and add the handshake only as the recovery path for epoch exhaustion. Ruled out:
deriving `n` from the clock. The clock is seeded from the network, and the modem was seen
reporting a year-2070 time once; one bad timestamp would push the relay's window decades ahead
and lock the device out for good.

*(Implementation note, 2026-09-20.)* The cold-boot bump described above was specified here but
missing from the firmware until it was found live: every cold boot restarted `lo` at 0 under the
same epoch, and the relay (initial `upN = 0`, accepts only `n > upN`) dropped everything as a
replay, starting with the very first publish after setup. `modes_boot()` now bumps the epoch on
every cold boot.

**Device side, `n` for `/down`.** Mirror of the relay's window: `down_n` + 32-bit bitmap in RTC
(8 bytes). Re-publishes on an online edge (`PROTOCOL.md` §5.3) reuse the message `id` but get a fresh
`n` from the relay, so dedup (§4.1 rule 7) still suppresses the re-render and the counter still
advances. A `/down` failing signature or window verification takes exactly the §3.4 malformed path:
count, do not render, **do not ack**. The relay then sees it stay `sent` — which is the right visible
outcome for a forged message and a harmless one for a bug, since a legitimate message is
re-published on the next edge.

### 2.6 Server-side storage and verification

```
deviceSecrets/{deviceId}   {hmacKey, mqttPasswordHash, upN, upBits, downN, sigFailures, createdAt, rotatedAt}
```
Server-only: no `firestore.rules` `match` block (default-deny read), the `{document=**}` write
catch-all denies writes; `relay/tests/test_rules.py` pins it, the same way it pins `phoneIndex`.
`mqttPasswordHash` moves here from `devices/{d}` because it has the same sensitivity.

Verification order in `ingest.py`, before anything is parsed: size ≤ 640 → look up
`deviceSecrets` by the topic's device id (cache in-process; it changes only on rotate) → if the
device's `authMode` is `hmac`, extract and verify `sig` → parse → check `n` in the window → existing
handling. A failure of any step drops the envelope, increments `sigFailures`, logs a security event
with the topic and first 64 bytes, and **still returns 2xx** to the broker (§3.4's rule: a bad
payload must not be redelivered forever). More than 20 failures in 10 minutes on one device raises
an alert log line and marks `devices/{d}.status.authAlarm` for the admin UI.

**The one unsigned inbound envelope**: the broker-generated LWT `{"v":1,"state":"offline",…}`
cannot carry a signature. The relay accepts an unsigned `/status` **only** when it is exactly
`state:"offline"` with no live fields. The worst a forger can do with it is mark a device offline in
the parent UI until the next signed `online`; accepted.

**Signing `/down`.** Every `/down` publish goes through one function in `broker.py` that takes the
`downN` increment in a transaction and appends `sig`. Includes `loc_req` and the `book` message of
§4. Devices provisioned with the `req_sig` flag clear (§3.4) ignore both fields, so a v1 device
stays conformant.

### 2.7 Device-side storage and implementation

- **New module `auth.c`** (`firmware/main/`): `auth_sign(topic, buf, len, cap)` appends the ten
  `sig` bytes in place; `auth_verify_down(topic, buf, len)` checks and trims them; counters and
  the window. HMAC via ESP-IDF's bundled mbedTLS (`mbedtls_md_hmac`), no new component. Cost per
  envelope: one SHA-256 over ≤ 700 bytes, microseconds on the S3.
- **Identity in NVS** (§3.4), read once at boot by a new `ident.c`; `net.cpp` loses its
  `constexpr` credentials and asks `ident_*()` getters instead.
- **No flash encryption, no NVS encryption (decided, H3).** `K_dev` and the MQTT password are
  readable by anyone who dumps flash over USB. That is T6 — a stolen device — and the answer is
  revoke, then one new setup code for the replacement. Nothing in the design depends on the key
  being unextractable, and the eFuse-burning release mode is not something a household should be
  asked to get right.
- **RTC changes** (`pager_rtc_t`, bump `PAGER_RTC_MAGIC`): add `auth.up_lo` (4), `auth.down_n`
  (4), `auth.down_bits` (4), 4 B of UI state and 16 B of lock state (§5.8); and, per §5.6, the
  reply and unread *bodies* leave RTC for NVS (≈ −480 B). Net ≈ 928 → ≈ 480 of 1184 B. `_Static_assert` stays, with room to spare
  for the first time.

### 2.8 What the chosen scheme does not do, on purpose

- **No confidentiality from the broker** (option E). The threat model has no party who can read the
  broker but should not read the thread. If that changes, E reuses `K_dev` and `n` as key and nonce
  and needs no re-provisioning; the only wire change is `body` becoming base64 ciphertext.
- **No protection against a relay database leak** (option D). Ed25519 would put only public keys on
  the relay. It costs ~100 bytes per envelope and a second key pair for the down direction; the
  upgrade path is `authMode: "ed25519"` beside `"hmac"` in `deviceSecrets`, a second NVS key slot,
  and the same `sig`-last framing with an 86-character value. Nothing in §2.4's framing would change.
- **No silent key rotation.** Rotation needs a person to type a new setup code (§3.5).

---

## 3. Provisioning — over the SIM only, for people who are not developers

### 3.0 The constraint, and the shape of the answer

Several households will each stand up their own relay, broker and web app, and none of them will
have ESP-IDF or a USB cable handy. So first identity must arrive with **no wire and no extra
radio**: the device has a keyboard, a screen, a button and a SIM, and that has to be enough.

What stops the device from simply "downloading its config" is that it has nothing to present to
the broker until it has a credential, and the vendor library's HTTP client does not build
(`PROTOCOL.md` §9.1). The way out is to split identity into two parts of very different size:

- a **short setup code** (~40 characters) that a person can type on the CardKB in a minute: a
  one-time token plus the broker host name;
- the **bundle** (device id, MQTT password, HMAC key, CA certificate, flags — 1–2 kB) which the
  device fetches itself over LTE, through a short-lived *bootstrap* MQTT credential derived from
  the token, encrypted under a second key derived from the same token.

The bootstrap hop reuses the one transport the device already has (TLS + MQTT in the modem), adds
no subscription in normal operation, and needs nothing from the person but typing.

| Channel for the setup code | Works from | Verdict |
|---|---|---|
| **Typed on the CardKB** | any device, SIM only | **primary** |
| SMS to the SIM, auto-read in setup mode | any deployment whose SIM plan delivers MT SMS | **rejected** (decided H8): the carrier would hold the token, it needs Twilio in every household's stack, and it would be the first device-side SMS path `PROTOCOL.md` §7.3 forbids. §3.6 records the shape in case it is ever revisited |
| USB serial console (`tools/provision.py`) | developers and CI | kept; same code, same decoder |
| Wi-Fi SoftAP + captive portal | every phone | **not needed** once the code is 40 characters; would add ~450 kB of Wi-Fi stack to the binary and a second radio to reason about. Recorded as the fallback if hardware bring-up says typing on the CardKB is unacceptable |
| BLE / Web Bluetooth | not iOS | rejected |
| A project-wide public bootstrap server baked into the firmware | — | rejected: the whole point is that each household runs its own stack |

### 3.1 The setup code

```
MBX9-7KQ2-P3RT-8C @ abc123.ala.us-east-1.emqxsl.com
```

- **token**: 8 random bytes (64 bits, decided H11) as 13 characters of Crockford base32 (no I, L,
  O, U; case-insensitive), shown in groups of four, plus one Crockford check character (`C`
  above), so a typo is caught on the device before any radio comes up. Hyphens and spaces are
  ignored on entry.
- **`@ host[:port]`**: the broker, from the relay's own configuration — the person never types a
  host name they had to look up; the web app prints the whole string. Port defaults to 8883.
- **optional `;apn=…`**: only for carriers that need a non-default APN, which is the one thing
  the bundle cannot carry because it is needed to fetch the bundle. Absent = carrier default, as
  `net.cpp` uses today.

Typical length 35–55 characters. The web app shows it as text with a copy button and as a QR code
(for a second phone or a laptop), and it expires after **10 minutes** or on first use.

### 3.2 The bootstrap fetch

Three values are derived from the token with HKDF-SHA256 so that nobody who sees only one of them
can reconstruct the others: `bid = hex(HKDF(token,"id"))[:12]`, `bpw = base64(HKDF(token,"pw"))`,
`bkey = HKDF(token,"bundle")` (32 bytes).

**Relay, when the admin clicks *Add device*** (`POST /api/admin/devices`, extended):

1. Generate `mqttPassword` and `hmacKey` (32 bytes) as §2.3; write `devices/{d}` (owner-readable, no
   secrets) and `deviceSecrets/{d}` (§2.6), `provisionState: "issued"`.
2. Push the device's real broker credential and its three ACL rules (§2.1's first gate) through
   EMQX's HTTP API (`/api/v5/authentication/password_based:built_in_database/users`,
   `/api/v5/authorization/sources/built_in_database/rules/users/{username}`) — new
   `relay/app/emqx_admin.py` with the API key `broker.py` already holds. The e2e stack turns EMQX
   auth and ACLs on for real, so `tools/e2e_v2.py` stops running anonymous.
3. Push a **bootstrap credential** the same way: username `boot-{bid}`, password `bpw`, ACL:
   subscribe `pager/boot/{bid}/down` only, publish `pager/boot/{bid}/up` only. Record it in
   `setupCodes/{bid}` (server-only) with `deviceId`, `expiresAt`.
4. Encrypt the bundle under `bkey` (AES-256-GCM, random 12-byte nonce, payload =
   `nonce ‖ ciphertext ‖ tag`, raw bytes) and **REST-publish it retained** to
   `pager/boot/{bid}/down`. Plaintext bundle, a CBOR map (shown here as its JSON equivalent):
   `{"v":1,"id":"pgr-0001","pw":"…","k":<32 B>,"host":"…","port":8883,"ca":"<PEM>","flags":1,"label":"Kid 1"}`
   ≈ 1.5–2.1 kB with a typical root CA; keys in §8 item 10. This topic has its own limit of **4 kB**; §3.3's 640 is
   for `pager/{id}/…` envelopes only.
5. Return the setup code once and never store it; `setupCodes` holds only `bid`.

**Device, in Setup mode** (a device with no valid `ident`, or *Set up again* on the Device screen):

1. Screen: *"Type the setup code from your pager website"*; the entry checks the Crockford check
   character per group and refuses to continue on a mismatch, naming the group.
2. Derive `bid`, `bpw`, `bkey`. Attach LTE with the code's APN or the carrier default.
3. TLS to `host:port` with **no CA validation** on this hop (`WALTER_MODEM_TLS_VALIDATION_NONE`
   in a separate TLS profile ≥ 3, so profile 2 stays the pinned production one). *(see rationale:
   the bundle is authenticated and encrypted under a single-use 80-bit key the device already
   holds; server authentication on this hop would add only DoS resistance, and it would force the
   CA — the thing the bundle exists to deliver — into the typed code. A man-in-the-middle can
   deny the fetch; he cannot read or forge the bundle.)*
4. MQTT connect as `boot-{bid}`/`bpw`, subscribe `pager/boot/{bid}/down`; the retained bundle
   arrives immediately (no wait on a relay round trip). Decrypt, verify the GCM tag, validate every
   field against the same regexes the relay uses, write the `ident` namespace (§3.4), write the CA
   to modem NVRAM cert slot 12.
5. Publish the CBOR equivalent of `{"v":1,"ok":1}` on `pager/boot/{bid}/up`, disconnect, drop the bootstrap profile, and
   continue into the normal boot path: TLS on profile 2 → MQTT as `pgr-0001` → first signed
   `/status`. The Device screen walks through *network → broker → bundle → done* with one word per
   step, and maps the failures: *no network* (SIM/coverage), *cannot reach broker* (host typo),
   *no setup code waiting* (expired — ask for a new one), *code damaged* (the check character).

**Relay, on `pager/boot/+/up`** (one more rule-engine match, same webhook, same key): mark the
code used, publish an empty retained message to clear the bundle, delete the bootstrap credential;
`provisionState` becomes `"provisioned"` when the first signed `/status` from `pgr-0001` arrives,
which is what flips the *Add device* page to *"pgr-0001 is online"*. `jobs.tick` (every 5 min)
expires stale codes the same way. The bundle is on the broker, encrypted, for at most 10 minutes.

**Bytes.** One extra TLS session (≈ 5 kB) plus ≈ 2.5 kB of bundle, once per device lifetime.

**What each party can see.**

| Party | Sees | Can do |
|---|---|---|
| the person typing | the token | fetch and decrypt the bundle — they are supposed to |
| a shoulder-surfer | the token, for 10 minutes | the same, once; the relay logs which `bid` fetched and the admin sees a device come online that they did not set up |
| anyone guessing tokens through the broker | nothing | 2⁶⁴ possibilities behind a per-token broker credential, for 10 minutes; EMQX's auth rate limit and the relay's alarm on repeated failures make the space unwalkable |
| the broker | `boot-{bid}`, `bpw`, ciphertext | nothing: `bkey` is not derivable from `bid` or `bpw` |
| a man-in-the-middle on the bootstrap hop | ciphertext | deny the fetch; not read or forge it |
| the relay database | `bid`, `expiresAt` | nothing: the token is never stored |

A second setup code issued for the same device is a **rotation** (§3.5): the previous password and
key are dead at the broker and in `deviceSecrets` before the new code is returned.

**Deployments where the relay cannot manage broker users** (`NEEDS HUMAN DECISION`, existing D2 —
whether EMQX Cloud Serverless exposes the auth/ACL API): the admin creates one long-lived
bootstrap user `boot` with ACLs `pager/boot/+/down` (subscribe) and `pager/boot/+/up` (publish)
from copy-paste text the web app shows once, and the setup code grows by that user's password
(≈ 20 characters). The bundle is still token-encrypted, so a shared bootstrap credential only
ever exposes ciphertext.

### 3.3 The broker CA

**Decision (2026-09-19): the CA is optional and the default is to pin none.** The relay sends an
empty `ca` unless `BROKER_CA_PEM` is set; the device then runs the production session with
certificate validation off. A bundle that does carry a CA still pins it, exactly as described
below, so pinning is an operator opt-in rather than something the device requires. Reasons:

- A pinned CA is a way to brick pagers. If the broker moves to a different root, every device fails
  TLS until a person types a new setup code on it. For a device whose job is to be reachable, that
  cost outweighs the attack described below, which needs an active attacker on the LTE→broker path
  (LTE-M authenticates the network and has no 2G fallback).
- Envelopes stay HMAC-authenticated either way (`PROTOCOL.md` §2.4), so pages cannot be forged or
  altered. What is given up is confidentiality of bodies and location fixes against that attacker,
  and the MQTT password. If that ever matters, §2.2's option E (AEAD bodies under `K_dev`) is the
  better fix: it protects against the broker too and cannot brick anything.
- "DNS guarantees the server" is **not** the justification: the modem does no DNSSEC validation.
  The trust placed here is in the network path.

**Hardware finding that constrains the implementation (GM02SP `LR8.2.1.0-61488`, verified by
capturing wire bytes on a server we control):** the modem's dedicated `AT+SQNSMQTT*` engine
**silently sends a plaintext MQTT CONNECT, credentials included, to the TLS port** when its TLS
profile names no CA slot (`AT+SQNSPCFG=2,2,"",0,,,,`). A TLS-only broker then waits for a
ClientHello forever and `+SQNSMQTTONCONNECT` never fires; this, not missing SNI, was the original
`setup` hang. With the slot named (`AT+SQNSPCFG=2,2,"",0,12,,,`) the same engine sends a normal
TLS 1.2 ClientHello **with SNI**, at validation level 0 or 1. The generic socket layer
(`AT+SQNSD`) does TLS either way. So every profile used for MQTT **MUST name the CA slot, even
with validation off**. `UNVERIFIED`: behaviour when the named slot is empty, as on a factory-fresh
modem; `mqtttest <host> <port> emptyca` exists to test it, and if it falls back to plaintext the
firmware must write a placeholder certificate into the slot.

The rest of this section describes the opt-in pinned mode and the original rationale for it.

**Why a CA is involved at all.** The bootstrap hop uses none: the token authenticates the bundle.
The CA is cargo for the *production* session, which pins one today (`net.cpp`'s hardcoded DigiCert
root, `PROTOCOL.md` §6.1) and must keep doing so once every household's broker chains to a
different one. *(see rationale: HMAC on every envelope gives integrity and authenticity, not
confidentiality. Without server authentication an active attacker on the LTE→broker path can
terminate TLS, read messages and location fixes in plaintext, capture the MQTT password from
CONNECT and connect as the device — which kicks the real device off the broker. The modem offers
two settings only, validate against a CA slot or validate nothing, and pinning the leaf breaks on
every certificate rotation, so the CA is the only stable pin.)* The design with no CA anywhere is
§2.2's option E: AEAD-encrypt bodies and fixes under `K_dev`, after which a MITM sees ciphertext
end to end and the only remaining exposure is the password, i.e. session hijack. Not taken,
because that hijack is a denial of service on a child's pager; recorded so the trade is explicit.

**Which CA each broker actually presents.** None of the brokers this project targets makes a
device-specific certificate; each has one server certificate, and the CA behind it is what the
bundle carries:

| Broker | Server certificate | CA in the bundle |
|---|---|---|
| EMQX Cloud Serverless (`SERVER_PLAN.md` §9.4 default) | issued by a public CA on the `*.emqxsl.com` endpoint; the console offers the CA file for download | that public root — `UNVERIFIED` which one; one `openssl s_client` call against the real account (D2) |
| Self-hosted on GCE (`infra/modules/broker-gce`) | Let's Encrypt via certbot at first boot; falls back to plain 1883 only, logged, if DNS is not ready | ISRG Root X1, which `infra/` already names |
| EMQX open source, untouched defaults | a self-signed demo certificate under `etc/certs/`, signed by a bundled test CA | that test CA — the "custom PEM" case; fine for the local compose stack, never for a deployment |

Because the CA travels in the bundle, the device needs **no built-in root store and no CA choice
in the typed code**. The relay has to know the PEM it wants pinned: `BROKER_CA_PEM` in config
(the Terraform in `infra/` knows which broker it deployed and sets it), otherwise resolved once at
startup by a TLS handshake to the broker and matching the served chain's issuer against the
`certifi` bundle, with the result shown on *Admin → Settings* so a wrong guess is visible before a
device is issued. A self-signed or private CA is just a different PEM in the same field; nothing on
the device changes.

The CA is written to modem NVRAM only when its hash differs from the stored `ca_hash`, instead of
on every `net_init()` as `net.cpp:394` does today (that call currently reruns on every F4
recovery).

### 3.4 What the device keeps

NVS namespace `ident`, one key per bundle field (`dev_id`, `mqtt_pw`, `kdev` blob 32, `host`,
`port`, `ca` ≤ 4 kB, `apn`, `flags`, `label`), plus `ca_hash`, `n_epoch` (u16, §2.5 counter epoch,
starts at 0) and `claimed` (u8, set on the first verified `book`, §4.3 — display only). Partition
table: `partitions.csv` with `nvs` (24 kB — the address book of §4 shares it under its own
namespace, and `msgq` of §5.6), `assets` (font and IME data, §5.2), `otadata`/`ota_0` reserved for
a later OTA, app. No `nvs_key` partition: encryption is off (§2.7).

### 3.5 Rotation, revocation, re-homing

- **Rotate / re-home** = issue a new setup code (`rotate-credentials`, extended to return one),
  optionally with a different owner and default recipient. Old password and key are deleted at the
  broker and replaced in `deviceSecrets` atomically before the response; the device takes the new
  code through *Set up again* on the Device screen, or by holding the button for 10 s during boot.
  There is no separate pairing step: the person issuing the code is the admin of that stack and
  names the owner on the *Add device* page, so the device is owned from the moment its code exists.
- **Revoke**: mount the missing `POST /api/admin/devices/{id}/revoke` (the store function exists,
  `relay/app/store/devices.py:100-107`; the web button 404s today), and make it also delete the
  broker credential so a stolen device cannot even connect.

### 3.6 Not built: the same code by SMS

Recorded only so the option is not re-derived. The setup code would fit in one 160-character
segment; the relay could text it through the Twilio adapter and the device, in Setup mode only,
could poll the modem's SMS store (`AT+CMGL` through `WalterModem::sendCmd()`) and ask for a button
press before using it. It is **not planned** (decided H8) because whoever can read the SMS holds
the token for 10 minutes, because it needs Twilio in every household's stack, and because it would
be the first device-side SMS path, which `PROTOCOL.md` §7.3 forbids. The typed code is the only
wireless path; the console command of §3.7 is the developer path.

### 3.7 Developers and CI

`tools/provision.py --port /dev/tty… --code "<the same string>"` types the code over the USB
console (a `setup <code>` console command that runs the identical decoder and fetch), and
`--from-api` obtains it by calling the admin endpoint first. `tools/pager_client.py` gains a
`--bootstrap <code>` mode that performs §3.2's fetch as a simulated device, which is what the e2e
suite uses. One code format, one decoder, three ways in.


## 4. Address book and approval

### 4.1 Model

The device's address book is a **projection of the server's allow-list** for the device owner,
plus the owner's pending requests. The server is the only writer of the routable part; the device
can only *ask*. Every approved contact is, as today, a `users/{uid}` with an alias, so an SMS
contact becomes a user with an `sms` backend — which also means grandma can text the pager back
through the existing inbound path with no new code.

Server additions:

```
contactRequests/{deviceId}_{reqId}  {deviceId, ownerUid, name, phone|null, alias|null,
                                     status: 'pending'|'approved'|'rejected', reason, createdAt, decidedAt, decidedBy}
devices/{d}.bookVersion             int, bumped by every change that alters the device's projection
```
`contactRequests` is readable by admins and by the device owner (the kid can see their own
pending list in the web app too); written only by the relay.

### 4.2 Requesting from the device

The Address Book screen's "Add" form (§5.5) takes a display name (1–16 code points, ≤ 48 UTF-8
bytes, no control characters — CJK allowed once an IME exists) and either a phone number (E.164,
digits and a leading `+`) or an alias the student already knows. It publishes:

```
{"v":1,"id":"u_2b7c…","ts":…,"kind":"contact_req","name":"Grandma","ph":"+15551234567","ack":null,"n":…,"sig":"…"}
```
≈ 140 bytes. `kind` becomes legal on `/up` (§8); `contact_req` carries no `from`/`body`. The relay
dedups on `id` like any up message, rate-limits to 5 pending requests per device, and treats a
request whose phone or alias matches an existing pending/approved one as a no-op. The device stores
the request locally (NVS namespace `book`) as `pending` so the student sees it immediately, and
shows "sent for approval".

### 4.3 Approving, and pushing the book down

Approval is **admin-only** *(see rationale: the allow-list is admin-owned today, and the device
owner is the student, who must not be able to approve their own recipients)*. Web `/admin/contacts`
lists pending requests across devices; approve opens a dialog:

- if `ph` matches a verified `sms` backend via `phoneIndex`, or `alias` names an existing user →
  "link to existing user";
- else "create user": alias defaults to a slug of `name` when one can be derived (a CJK name
  yields none, so the admin types the alias; it must satisfy `ALIAS_RE` and be unique), plus an
  `sms` backend with that phone, created by admin assertion (`verifiedAt` set, `adminVerified:
  true`, `phoneIndex` written) — a new `POST /api/admin/users/{uid}/backends`;
- then upsert two allow edges (owner → contact `message`, contact → owner `message`; `locate` is a
  separate checkbox, default off), recompute `locatableBy`, bump `bookVersion`.

Reject stores a reason (`"not_allowed"`, free text) that the device shows in grey.

Every `bookVersion` bump publishes one `/down` `book` message, signed like any other:

```
{"v":1,"id":"m_…","ts":…,"kind":"book","bv":7,"d":"mom",
 "c":[{"a":"mom","n":"Mom","t":"web"},{"a":"dad","n":"Dad","t":"web"},{"a":"grandma","n":"Grandma","t":"sms"}],
 "p":[{"n":"Uncle Bob","s":"pend"},{"n":"Sam","s":"no"}],
 "ack":null,"n":…,"sig":"…"}
```

| Field | Meaning |
|---|---|
| `bv` | book version; the device stores it and reports it in `/status` (§5) |
| `d` | default recipient alias (what an `/up` without `to` routes to) |
| `c[]` | approved contacts: `a` alias, `n` display name ≤ 16, `t` hint for an icon (`web`/`sms`/`chat`) |
| `p[]` | the device's own requests that are not approved: `n` name, `s` = `pend` \| `no` |

Rules: not a thread entry, not rendered as a message, **acked `shown` once applied** (reusing the
ack machinery so the relay knows the book landed; the relay expires any older unacked `book` when it
creates a new one, and §5.3's re-publish includes it). Applied atomically into NVS as a full
replacement. **Cap: 10 approved + 4 listed requests per device** — the largest such message is
≈ 590 bytes, inside the 640 limit with the signature. `more` is reserved for chunking if the cap
ever moves. `/status` gains `bv`; if the relay sees a `bv` lower than `bookVersion` (a factory
reset, or a lost message that also missed the re-publish window) it pushes the book again. That is
the entire sync protocol: one optional status field, one down kind, no new subscription.

*(see rationale for a `/down` kind rather than the reserved retained `pager/{id}/cfg` topic:
`PROTOCOL.md` §2 exists to keep the device at one subscription, and a retained message would be
re-delivered on every reconnect — 4–24 times a day — for data that changes a few times a month.)*

Device storage (`book.c`, NVS namespace `book`): one blob ≈ 10×(17+49+37+1) + 4×(49+1) + 8 ≈ 1.25 kB
(alias, display name ≤ 48 B, local nickname ≤ 36 B, type per approved contact; name and state per
request — UTF-8 throughout),
rewritten only when a `book` arrives, a request is made, or a nickname changes. RAM copy for
rendering.

---

## 5. On-device UI

### 5.1 Hardware facts the UI is designed against

296 × 128 px, 1-bit, landscape; partial refresh 0.3–0.8 s budgeted, full refresh 2–4 s
(`firmware/README.md` M7/M14, both `PENDING_HW`); every 20th partial is a full. Input: one button on
IO1 (the only thing that can wake the ESP32) and the CardKB over I²C, which cannot wake anything and
must be polled. The panel keeps its image with no power, which is what makes "the UI goes to sleep
but the screen stays" free.

### 5.2 Text and layout primitives (`gfx.c`, split out of `ui.c`)

- **Fonts are data, pre-rasterised from Noto (decided, H9).** The hand-drawn 5×7 is retired.
  `tools/mkfont.py` (FreeType, monochrome hinted rendering, `FT_LOAD_TARGET_MONO`) rasterises
  **Noto Sans** (Latin, Greek, Cyrillic) and **Noto Sans CJK** at build time into a compact 1-bit
  glyph store: per size, a sorted codepoint→offset table and per-glyph `{width, advance, bearing,
  rows}`. Two sizes, **12 px** (`normal`) and **16 px** (`large`), stored in a dedicated
  `assets` partition memory-mapped at boot (`esp_partition_mmap`), so a different language build
  changes one partition, not the firmware; an IME dictionary (§5.3) lands in the same partition. Licence: OFL 1.1, shipped alongside.
- **Coverage.** All of Latin-1 and Latin Extended-A, Greek, Cyrillic, general and CJK punctuation,
  fullwidth forms, plus one CJK repertoire chosen at build time by `PAGER_FONT_LANG` (default `sc`): `sc` (GB2312,
  6763 hanzi), `tc` (Big5 common, 5401), `jp` (JIS X 0208 levels 1+2 + kana), `kr` (KS X 1001:
  2350 hangul + 4888 hanja). Noto Sans CJK's regional variant is selected to match. Sizes: ≈ 7000
  glyphs × (16×16/8 + 4) ≈ 250 kB at 16 px and ≈ 155 kB at 12 px, ≈ 420 kB with the non-CJK
  ranges; the Walter's 16 MB flash does not notice. A codepoint outside the store renders as a
  tofu box and increments a counter shown on the Device screen.
- **Input is Unicode from day one, even though the keys are ASCII.** The CardKB is a QWERTY
  keyboard emitting one ASCII byte per press, but an **IME** may later sit between keystrokes and
  the composer (pinyin, romaji→kana→kanji, dubeolsik hangul, …), so every text buffer on the
  device — composer, contact name, nickname, setup-code entry — is UTF-8 with both of
  `PROTOCOL.md` §3.1's caps enforced at entry: **160 code points and 320 bytes**, whichever binds
  first, refusing further input rather than truncating. The §9.4 assumption that a device-typed
  reply is ASCII is **withdrawn**, and §5.6 moves the reply bodies out of RTC memory as a
  consequence. The IME hook itself is in §5.3; no IME is built in this plan.
- **Metrics are proportional now.** At 12 px Noto Sans averages ≈ 6.5 px advance → ≈ 45 Latin
  characters or 24 CJK per line; at 16 px ≈ 8.5 px → ≈ 34 Latin / 18 CJK. Status bar and footer
  are 14 px tall in the 12 px face; the body gets 100 px → **7 rows** at `normal` (14 px pitch)
  or **5 rows** at `large` (18 px pitch). Wrapping is by pixel width: on spaces for scripts that
  have them, per character for CJK. The §5.5 mockups are schematic and drawn monospaced.
- Primitives: `text(x,y,size,utf8)` with a UTF-8 decoder, `text_wrap()` returning line count,
  `text_width()`, `hline`, `rect`, `invert_rect` (selection highlight), `icon(x,y,id)` for 12×12
  bitmaps (signal 0–4 bars, battery 0–4 segments, link ok/x, lock, pending clock, sms/web/chat
  glyphs).
- Text size is a Setting (`normal`/`large`) stored in NVS; the body uses it, the status bar and
  footer are always 12 px.
- **Host-side render test.** The same `gfx.c` and glyph store compile on the host to render every
  screen to PNG, which is how the font is eyeballed before hardware; M9 then checks the real
  panel. If 12 px CJK proves illegible on glass, `normal` moves to 14 px — a build flag, not a
  design change.

### 5.3 Input model (`input.c`)

- CardKB decode extended: printable ASCII, `0x08` backspace, `0x0D` enter, `0x1B` esc, `0x09` tab,
  and **`0xB4`–`0xB7` = left/up/down/right** (M13 now also verifies these codes; they are
  `UNVERIFIED` from the CardKB documentation, ~5 min on hardware). Anything else is still dropped.
- Button: short (< 600 ms) and long, as today's FSM, plus README R2's `BTN_STUCK` so a held button
  cannot pin the loop at 40 mA.
- **UI-awake window.** A key or button event arms a 30 s timer (`PAGER_UI_AWAKE_S`, compile-time);
  while armed, the CardKB is polled every 100 ms and `modes_run()` does not light-sleep, exactly the
  carve-out `PROTOCOL.md` §8.4 already makes for the composer, now covering every screen. When it
  lapses the *screen stays as it is* and the loop returns to the wake-and-drain cadence. Cost:
  30 s × ~40 mA ≈ 0.33 mAh per interaction; 20 interactions/day ≈ 7 mAh/day on top of §8.4's
  43–50. Worth measuring whether 100 ms light-sleep between polls (I_light(0.1) ≈ 21 mA) keeps I²C
  usable; if so the term halves. The window is independent of the 10-minute *modem* active window.
- **Waking from a keyboard press (optional, needs M13).** If the CardKB holds the last key in its
  buffer until read, polling it once per wake-and-drain cycle (one I²C read, ~0.1 ms) gives "press
  any key to wake the UI" with ≤ 5 s latency and no button. Enabled only if M13 confirms the hold.
- **IME hook.** Between the key queue and any text field sits an `ime_t` with one entry point,
  `ime_feed(key) → {consumed, commit_utf8, preedit_utf8, candidates[]}`. The default IME is the
  identity (ASCII through, nothing pre-edited). A text field renders the pre-edit inline
  (underlined) and up to five numbered candidates on the footer line, `left`/`right` to page,
  `1`–`5` or `space` to commit, `esc` to drop the pre-edit. Fields declare whether the IME applies:
  the composer, contact name and nickname do; the setup-code and phone fields are ASCII-only by
  definition and bypass it. Which IME is active is an NVS setting, listed on the Device screen next
  to *Text size*; dictionaries live in the `assets` partition beside the font. No IME is designed
  here — hangul composition is algorithmic and small, pinyin and kana→kanji need dictionaries of
  tens of kB to a few MB — but the hook, the Unicode buffers and the partition are what make adding
  one a contained change (decision H15).
- All events land in one queue drained by `modes_run()`'s task. **All rendering happens on that
  task** (closes README R5) behind one mutex in `disp.c` (closes R4); the modem event task only
  ingests and sets a `render_pending` flag, and `msg_mark_shown()` moves to after the render so §4's
  "ack after BUSY deasserts" holds.

### 5.4 Screen stack and the status bar

`ui.c` becomes a stack (depth ≤ 4) of `screen_t {render, on_key, on_event}`; `push`, `pop`, and
`replace`. Every render draws the status bar, the screen's body, and a footer of key hints, into
the framebuffer, then calls the existing diff-based partial refresh — only rows that changed are
sent, so a status-bar-only change costs one 10-row window. The 20-partial/1-full cadence stays but
the full refresh is **not taken for messages arriving into an already-open Chat** (README R9): it
is deferred to the moment the UI-awake window lapses. *(Amended 2026-09-20 from the first look at
real hardware: the one exception is a message that **changes the screen**, e.g. greeting → Chat.
A partial refresh there left the greeting's large type ghosted under the message, so
`ui_incoming()` takes a full refresh for that first message only. The Chat screen's key-hint
footer was also dropped in favour of 2 px of leading between rows.)*

**Status bar** (10 px, always 1×): `[signal 0–4 bars] [link ok|x] [unsent n] [lock if sig on]
… [unread n] [battery 0–4]`. No clock *(see rationale: a clock would need a refresh every minute
while the device is meant to be asleep; message rows carry absolute `HH:MM` instead, which never
change)*.

- **Signal.** New `net_get_rssi()` over the vendor API's signal-quality call (`AT+CSQ`/`AT+CESQ`;
  `UNVERIFIED` which of `getRSSI()`/`getSignalQuality()` v1.5.0 exposes — 10 min reading
  `src/WalterModem.h`). Read at every `/status` publish (so `rssi` finally appears there) and at
  every UI wake. Bars from dBm: ≥ −85 → 4, ≥ −95 → 3, ≥ −105 → 2, ≥ −115 → 1, else 0; not
  registered → `x`. Redraw only when the *bucket* changes.
- **Battery.** `net_get_battery_mv()` at every UI wake and hourly, cached in `modes.c` as today.
  Segments from mV (LiFePO4 is flat, so these are coarse and `PENDING_HW`, to be set from M15 and a
  discharge curve): ≥ 3300 → 4, ≥ 3250 → 3, ≥ 3200 → 2, ≥ 3100 → 1, < 3100 → 0 plus a "charge me"
  toast once per boot. Percent stays a UI-side mapping per `PROTOCOL.md` §5.1.
- **Unread** = down messages in the ring not yet `read`.

### 5.5 Screens

Mockups are schematic, drawn monospaced at 49 columns; the real font is proportional (§5.2). `>`
is the selection row, drawn inverted on the panel.

**Home — active chats (requirement 5).** One row per peer alias that has any message in the ring
or is the default recipient; sorted by newest message; `system` messages appear under `system`.
Below the chats, the three fixed entries.

```
[|||.] ok  unsent 0  ⚿                 new 1  [###.]
> mom       Pickup at 3:15 by the gym       14:02 *
  dad       ok see you                      13:40
  grandma   (no messages yet)
  system    unknown recipient               09:12
  ---------------------------------------------
  New message
  Address book
  Device
  Lock now
up/down move   enter open   btn hold = home
```

Keys: up/down move, enter open, esc nothing. Button short = if any unread, open the newest unread
chat, else stay; long = Home from anywhere.

**Chat (requirement 6).** History for one peer, newest at the bottom, oldest scrolled off the top.
`up`/`down` scroll a row, `left`/`right` a page; the view auto-follows new messages unless the user
has scrolled up. Typing any printable key opens the composer line in place; `enter` sends, `esc`
clears it, `esc` on an empty composer goes back. Up messages show their state (`…` pending, `sent`,
`FAILED`). Opening a chat, or pressing any key while it is on screen, marks every rendered down
message from that peer `read` (through the existing pending-ack queue; with 8 slots, a burst of
more than 8 is acked over successive pumps).

```
[|||.] mom                                     [###.]
mom  13:58 where are you?
you  13:59 library, coming now               sent
mom  14:02 Pickup at 3:15 by the gym         NEW
mom  14:02 bring your jacket
---------------------------------------------------
> ok coming_                          9/160
enter send   esc back   ^v history
```

At 2× the body shows 4 rows of 24 columns; long messages wrap and are scrolled like any other row.

**Incoming message while asleep or on Home.** The current behaviour, generalised: the chat for that
sender is pushed with the new message at the bottom, the partial refresh completes, `shown` is
published. **Incoming while the user is typing or on another screen**: a one-line toast (`new:
mom`) and the unread count; the screen is not stolen. *(see rationale: clobbering a half-typed
reply to show the message it is replying to is the one UI failure a 10-year-old will not forgive.)*

**New message → pick recipient (requirement 4).** The approved contacts from the book, with the
default first and an icon for the delivery hint; pending/rejected entries are listed greyed and are
not selectable. `enter` opens (or creates) that chat with the composer already open. If the book is
empty (no `book` received yet) the screen says so and offers *Re-sync* on the Device screen.
Rows show the student's **nickname** for a contact when one is set (*Nicknames*, below), else the
server's display name.

```
[|||.] to:                                     [###.]
> mom        (default)                    web
  dad                                     web
  grandma                                 sms
  uncle bob  pending approval
  sam        not approved
enter choose   esc back
```

Sending from a chat sets `to` to the peer alias — except the default recipient, where `to` is
omitted so the wire stays identical to today for the common case.

**Address book (requirement 3).** Same list, plus `Add`. The Add form is two fields with `tab`
between them, `enter` submits, `esc` cancels; the phone field accepts only `+` and digits and shows
the E.164 rule inline. On submit: §4.2. The entry appears immediately as `pending`.

```
[|||.] add contact                             [###.]
Name   Grandma_
Phone  +1555123
        (or leave blank and type an @alias)

tab next field   enter send for approval   esc cancel
```

Removal is web-only in v1 *(see rationale: the student asking to remove a parent from their own
allow-list is precisely the request the server should not honour from the device)*.

**Nicknames.** On any approved contact, `enter` → *Nickname* opens a one-field form
(≤ 12 code points / 36 bytes, IME-enabled, empty clears). The nickname replaces the display name everywhere on the
device — chat list, chat header, picker — and the alias is shown small beside it in the book so
the student can still see who "gma" is. Nicknames are **device-local**: stored in the `book` NVS
namespace keyed by alias, never sent up, never overwritten by a `book` push, dropped when the
alias leaves the book. The composer also accepts `@nick` or `@alias` as the first word to pick
the recipient from the keyboard without the picker (the `@alias` shortcut `SERVER_PLAN.md` §11
item 3 already anticipated, now resolved on the device, so the wire still carries the alias).

**Device (requirement 2).** Read-mostly, one screen, scrolls:

```
[|||.] device                                  [###.]
id        pgr-0001         fw 0.2.0
owner     kid1             claimed yes
broker    mqtt.example:8883  sig on
signal    -93 dBm  bars 3   batt 3280 mV
session   s_3ab91c02   book v7
counters  memfull 0  drops 0  resets 0
> Re-sync address book
  Text size: normal
  Passcode: set          Auto-lock: 5 min
  Show senders when locked: on
  Set up again
  Factory reset
```

- **Re-sync address book** — publishes a `/status` now (it carries `bv`), which is the sync trigger.
- **Text size** — toggles 1×/2×.
- **Passcode / Auto-lock / Show senders** — §5.8. *Passcode* cycles set → change → off, asking
  for the current one first; *Auto-lock* cycles 0 (never), 1, 2, 5, 10, 30, 60 minutes.
- **Set up again** — enters Setup mode (§3.2) to take a new setup code: rotation or re-homing. The
  current identity is kept until the new bundle has been verified and written, so a cancelled or
  failed setup leaves a working device.
- **Factory reset** — erases the `ident` and `book` namespaces after the student types the device
  id to confirm; the modem's cert slot is left alone. The device then boots into Setup. *(a wiped
  device is safe; a device with a stale key is just offline.)*

**Setup.** Shown when `ident` is missing or fails validation, and by *Set up again*: the typed-code
entry and the four-step progress line of §3.2.

**Locked (requirement 8).** Reached by auto-lock, *Lock now*, or any restart while a passcode is
set. Status bar as usual; nothing else is reachable.

```
[|||.] ok                              new 2  [###.]

                 Locked
        2 new  ·  mom, dad

        passcode  ****_
enter unlock                          btn hold = nothing
```

### 5.6 Message store changes (`msg.c`)

- Ring depth 10 → **32**; RAM cost ≈ 12 kB, which §9.5 says is not the scarce resource. §5.3's
  10-per-online-edge re-publish cap is about *unacked* messages and does not need to match.
- `msg_t` gains `to[17]` (peer alias for up messages; empty = default). `pending_up` gains the
  same. `publish_reply()` emits `to` when set.
- **Variable-length bodies leave RTC memory.** `PROTOCOL.md` §9.3 keeps two 161-byte reply
  bodies and one 161-byte unread mirror in RTC on the strength of §9.4's "a device-typed reply is
  ASCII". With an IME (§5.2) a reply is up to 320 bytes, two of those do not fit in the ≈ 200 B of
  RTC headroom, and truncating a reply is the one thing §9.4 refuses. So the bodies move to the
  **NVS namespace `msgq`**: a pending reply is written once on submit (full 320 bytes, its `to`,
  its `id`) and erased on PUBACK; the newest unread down message is written once on ingest, full
  fidelity, and cleared on `read`. RTC keeps what it is good at — the dedup digest ring, the
  pending-ack queue, and per-reply `{id, state, attempts}` — and shrinks by ≈ 480 B. Wear: ≈ 30
  small writes a day on a 24 kB wear-levelled partition is decades. Durability *improves*: NVS
  survives a cold boot, RTC does not, so an unsent reply now outlives a battery pull, and the
  "trailing ellipsis after a reset" of §9.4 is gone because the mirror is no longer lossy.
- `from` on up messages stays the literal `"student"` — the relay attributes by device, not by the
  claim (`PROTOCOL.md` §3.1) — but the Device screen shows the owner alias from the book's `d`
  neighbour once a `book` has arrived.
- Chat queries: `msg_iter_peer(alias, from_newest, cb)`; peers list derived on demand.
- Accessors return **copies** under the lock (README R6).
- Survival: the ring is RAM, so a crash still drops history to the single RTC `unread` mirror and
  the existing "earlier messages lost (restart)" line stays. An NVS-backed ring is an option
  (32 × ~200 B rewritten per message, 20 writes/day, well inside NVS wear) but is not in this plan;
  revisit if crash frequency on hardware makes it matter.

### 5.7 Power and latency consequences

- The UI-awake window adds ≈ 7 mAh/day at 20 interactions (§5.3). Everything else is unchanged:
  the wake-and-drain cadence, the modem active window, and §6.5's latency budget, which the
  deferred full refresh (§5.4) actually helps.
- One more AT round trip per UI wake (signal + battery), ≈ 100 ms at 40 mA, negligible.
- Verifying `/down` signatures is microseconds and does not enter the §6.5 budget.

### 5.8 Device lock (`lock.c`)

**What it protects against.** A sibling, a classmate, a device left on a bench: someone who picks
the pager up and reads or replies. It does **not** protect against someone with a USB cable and
time — flash is unencrypted by decision (H3), so the passcode hash can be dumped and a short PIN
brute-forced offline, or NVS simply erased (which yields an unprovisioned device, the T6 outcome).

- **Passcode.** 4–16 printable ASCII characters, so a digits-only PIN is just a short passcode.
  Typed on the CardKB, masked, IME bypassed. Stored in NVS namespace `lock`: `salt` (16 random
  bytes), `hash` = PBKDF2-HMAC-SHA256(passcode, salt, 10 000 iterations) via mbedTLS
  (`UNVERIFIED`: ≈ 50–100 ms on the S3, §10), `auto_min` (u8; 0 = never; default **5**),
  `preview` (u8; default 1). Setting or changing asks for the current passcode first; *off* too.
- **Auto-lock, no timer.** `last_input_us` is the monotonic time of the last key or button event.
  On every input event and every UI wake: `if (passcode set && auto_min && now − last_input_us ≥
  auto_min × 60 s) lock()`. The UI already sleeps after 30 s (§5.3), so what the student sees is:
  press the button after the interval, get the lock screen. After a **restart** the monotonic
  clock is gone and the device comes up locked whenever a passcode is set — the safe default.
  `locked` lives in RTC so a crash does not unlock. *Lock now* on Home locks immediately.
- **Locked screen.** Count of unread down messages and, if `preview` is on, the sender aliases or
  nicknames — never a body. Incoming messages are stored normally and the count updates; the
  button and every key other than the passcode field do nothing.
- **Acks while locked — the rule that keeps §4 honest.** `shown` means "the e-paper refresh that
  displayed the body completed" (`PROTOCOL.md` §4). A body that arrived while locked has not been
  displayed, so **no `shown` is published**; the message stays `sent` at the relay, the parent sees
  "not yet seen", which is true, and §5.3's online-edge re-publish plus dedup cover the gap exactly
  as for a device that was off. On unlock the newest-unread chat opens, the refresh completes, and
  the deferred `shown` acks go out through the normal pending-ack queue. `read` is unchanged.
  Location requests (§13) are answered regardless of lock state — the lock is about the screen.
- **Wrong passcode.** Five free attempts, then a backoff of 30 s doubling to a 10-minute cap,
  shown as a countdown on the lock screen. `fail_count` and `backoff_until_us` live in RTC so a
  restart does not reset them.
- **Forgotten passcode — recovery without a cable.** Web app *Devices → Clear passcode* publishes
  a signed `/down` of a new kind, `cfg`, carrying `lock:{clear:true}`. The device honours it
  because the signature proves it came from the relay that owns `K_dev`; it clears the `lock`
  namespace, unlocks, and toasts *"passcode cleared by admin"*. `cfg` may also carry
  `lock:{auto:<minutes>}` so a parent can set the auto-lock from the web; it is applied and shown
  on the Device screen. `cfg` follows `book`'s rules: not a thread entry, acked `shown` on apply,
  only the newest re-published. Holding the button for 10 s at boot (factory reset, §3.5) remains
  the path that always works and always yields an unprovisioned device.
- **RTC**: `locked` (1), `fail_count` (1), `backoff_until_us` (8) — 16 B with padding, inside the
  room §2.7 opened.

---

## 6. Module map

**Firmware (`firmware/main/`)**

| Module | New / changed | Responsibility |
|---|---|---|
| `ident.c/h` | new | Load and validate the `ident` NVS namespace; getters; factory reset |
| `setup.c/h` | new | Setup mode: code entry + Crockford check, HKDF derivation, bootstrap TLS profile, fetch/decrypt/verify the bundle (AES-GCM via mbedTLS), write `ident`; the `setup <code>` console command |
| `auth.c/h` | new | HMAC sign/verify, counters, window (§2) |
| `book.c/h` | new | NVS-backed address book, pending requests, nicknames, `bv`, `contact_req` publish (§4) |
| `lock.c/h` | new | Passcode hash in NVS, auto-lock check, attempt backoff, `cfg` lock actions (§5.8) |
| `disp.c/h` | split from `ui.c` | SSD1680 driver, BUSY, mutex, refresh cadence |
| `gfx.c/h` | split from `ui.c` | Framebuffer, fonts, text wrap, icons, invert |
| `input.c/h`, `ime.h` | new | CardKB decode incl. arrows/esc/tab, button FSM (+`BTN_STUCK`), UI-awake timer, event queue; the `ime_t` hook with the identity IME (§5.3) |
| `ui.c/h` + `scr_home.c`, `scr_chat.c`, `scr_pick.c`, `scr_book.c`, `scr_device.c`, `scr_lock.c`, `scr_setup.c` | rewritten | Screen stack, status bar, screens (§5) |
| `msg.c/h` | changed | Ring 32, `to`, per-peer iteration, copies under lock, `kind` on up, `book` ingest hook; reply and unread bodies in the `msgq` NVS namespace instead of RTC (§5.6); UTF-8 composer with code-point and byte caps |
| `net.cpp/h` | changed | Credentials from `ident`, CA write only on change, a second TLS profile for the bootstrap hop, `net_get_rssi()`, sign every publish, verify every `/down` |
| `modes.c` | changed | RTC additions + magic bump, render on main task, `render_pending`, `/status` gains `rssi`,`bv`,`n`,`sig`, 256-byte status buffer |
| `cbor.c/h` | new | CBOR subset encoder/decoder over static buffers (§2.4); replaces cJSON on the RX path |
| `partitions.csv`, `sdkconfig.defaults` | new / changed | `nvs`, `assets`, OTA slots; `nvs_flash`, `mbedtls` and `console` in `REQUIRES`; `json` dropped once `cbor.c` lands |

**Relay (`relay/app/`)**

| Module | New / changed | Responsibility |
|---|---|---|
| `devauth.py` | new | Verify `/up`,`/status`,`/loc`; sign `/down`; window logic (§2.4–2.6) |
| `store/device_secrets.py` | new | `deviceSecrets` CRUD and the counter transaction |
| `emqx_admin.py` | new | Push/rotate/delete device and bootstrap credentials and ACL rules (§3.2) |
| `devsetup.py`, `store/setup_codes.py` | new | Setup code generation, HKDF derivation, bundle encryption, retained publish, `pager/boot/+/up` handling, expiry in `jobs.tick` (§3.2) |
| `ca_resolve.py` | new | `BROKER_CA_PEM` from config or resolved from the broker's chain (§3.3) |
| `ingest.py` | changed | Verify before parse; `kind` on `/up` (`contact_req`); `bv` → book sync; `/boot/+/up` dispatch |
| `broker.py` | changed | Single signed publish path; retained publish for the bootstrap topic |
| `devcfg.py` | new | Builds and publishes `book` and `cfg` down messages; supersedes older unacked ones (§4.3, §5.8) |
| `routers/admin.py` | changed | Setup code on create/rotate; `revoke`; contact requests approve/reject; admin-created backends; `POST /api/admin/devices/{id}/cfg` for lock clear / auto-lock |
| `store/contacts.py` | new | §4.1 |
| `firestore.rules`, `tests/test_rules.py` | changed | `contactRequests` read rule; pin `deviceSecrets`/`setupCodes` as server-only |
| `tests/test_devauth.py`, `test_devsetup.py`, `test_contacts.py` | new | Vectors shared with the firmware (below) |

**Tools and web**

| Path | Responsibility |
|---|---|
| `tools/provision.py` | §3.7: types a setup code over the USB console, or obtains one from the API first |
| `tools/pager_client.py`, `tools/e2e_v2.py` | `--bootstrap <code>` performs the §3.2 fetch; sign and verify like a real device; e2e runs with broker auth on; a setup-code/book/contact scenario |
| `tools/authvectors.json` | Test vectors (key, topic, payload in both encodings, expected `sig`; token → `bid`/`bpw`/`bkey`; an encrypted bundle) consumed by `relay/tests` **and** a firmware host-side unit test, so the two codecs and the two signing rules cannot drift |
| `tools/mkassets.py` | Rasterises Noto for a given `PAGER_FONT_LANG` and packs it (and, later, IME dictionaries) into the `assets` partition image (§5.2) |
| `web/app/admin/devices` | *Add device* wizard: label, owner, default recipient → setup code as text + QR (once, in React state as today) → "waiting for the device… / online"; rotate = new code; revoke that works; *Clear passcode* and *Auto-lock* (§5.8); per-device `authAlarm`, `provisionState`, book version |
| `web/app/admin/contacts` | Pending request queue, approve/reject dialog (§4.3) |
| `web/app/settings/devices` (owner) | Read-only view of own device, own pending requests |

---

## 7. Phases

Each phase leaves `main` green (`ci.yml` unit + e2e) and is independently useful. Sizes are rough
(S ≈ a day, M ≈ a few days, L ≈ a week or more of focused work) and assume no hardware until
phase 6.

| # | Phase | Size | Depends on | Done when |
|---|---|---|---|---|
| 0 | **Protocol edits** (§8) to `PROTOCOL.md`; `SERVER_PLAN.md` §3 gains the new collections | S | — | Docs merged; nothing else changes |
| 1 | **Relay device auth + CBOR**: `deviceSecrets`, `devauth.py`, verify-before-parse, signed `/down`, `authMode` and `wire` per device, alarms; CBOR ingest and publish with the §8 keymap (`cbor2`); `pager_client.py` signs and speaks both encodings; test vectors | M | 0 | e2e passes with a signing simulated device; an unsigned envelope from an `hmac` device is dropped and logged; replay inside/outside the window behaves per §2.5 |
| 2 | **Broker credentials for real**: `emqx_admin.py`, create/rotate/revoke push, e2e stack with EMQX auth + ACL on, `revoke` route mounted, web revoke fixed | M | 1 | e2e device cannot connect with a wrong password or publish to another device's topic |
| 2b | **Setup codes, server side**: `devsetup.py`, bootstrap credentials, encrypted retained bundle, `pager/boot/+/up`, expiry, `ca_resolve.py`, *Add device* wizard; `pager_client.py --bootstrap` | M | 2 | e2e: a code issued by the API → simulated device fetches, decrypts, connects with the real credential → page shows online; an expired code yields "no setup code waiting"; a shared-bootstrap-user deployment (manual broker) passes the same scenario |
| 3 | **Firmware identity + signing + setup + CBOR**: `partitions.csv`, NVS, `ident.c`, `cbor.c` (cJSON out of the RX path), `auth.c`, `setup.c` (typed code, HKDF, AES-GCM, bootstrap TLS profile, console command), `net.cpp` on getters, `/status` `rssi`+`n`+`sig`, RTC bump; `tools/provision.py`; host-side vector test | L | 1, 2b | `idf.py build` clean; the host-side test derives `bid`/`bpw`/`bkey` and decrypts the vector bundle byte-for-byte like the relay; a simulated setup over the console reaches "done" against the local stack |
| 4 | **Address book + cfg, server + web**: `contactRequests`, approve/reject, admin-created backends, `book` and `cfg` down messages (`devcfg.py`), `bv` sync, `/admin/contacts` page, *Clear passcode* / *Auto-lock* on the device page | M | 1 | e2e: simulated device requests a phone contact → admin approves → device receives `book` with the new alias → `to` that alias routes to SMS mock |
| 6 | **Firmware UI framework**: `disp`/`gfx`/`input` split, `tools/mkassets.py` + the `assets` partition with Noto at 12/16 px and one CJK repertoire, UTF-8 proportional text, the IME hook with the identity IME, screen stack, status bar, render on main task, UI-awake window, Home + Chat + Device + Locked screens, `lock.c` with deferred `shown`, `msg.c` changes incl. bodies to NVS | L | 3 | Builds; a host-side framebuffer test renders each screen to a PNG, including a CJK message, for eyeballing before hardware; R4/R5/R6/R9 closed |
| 7 | **Firmware address book**: `book.c`, `book` ingest + ack, pick-recipient, Address Book + Add + Nickname screens, `@nick` in the composer, `to` on replies | M | 4, 6 | Builds; simulated end-to-end against the local stack with the firmware's own CBOR |
| 8 | **Hardware bring-up additions** to `firmware/README.md`'s checklist: M13 arrow codes and key-hold, M9 on Noto at 12 and 16 px including CJK, the battery thresholds, RSSI mapping, UI-awake current | — | hardware | Numbers replace `PENDING_HW` |

Phases 1–2–2b form the server track and 3 the firmware track; they can run in parallel once 0 is
merged, with 3 needing 2b only for its final e2e. 4 and 6 can run in parallel once their
dependencies are in.

---

## 8. Edits this plan makes to `PROTOCOL.md` (phase 0)

Listed here so the diff to the authoritative document is reviewable on its own. All additive; a
`v:1` device that ignores every new field stays conformant.

0. **§3 encoding**: "Payloads are minified UTF-8 JSON objects" becomes "a JSON object **or** a
   CBOR definite-length map with the §10 integer keys, dispatched on the first byte; devices SHOULD
   emit CBOR; values are identical in both; `v` is unaffected by the encoding". §10 is promoted
   from "documented, not designed" to normative and its keymap becomes the wire contract.
1. **§3.1 fields**: `n` (uint32, optional, replay counter), `sig` (bstr(8) in CBOR / 11-char
   base64url string in JSON, optional, MUST be the last pair), `bv` (int, `/status`), `name`/`ph`
   (`contact_req` only), `d`/`c`/`p`/`more` (`book` only). Publisher order becomes
   `v,id,ts,kind,from,to,body,ack,…,n,sig`.
2. **§3.2 kinds**: `kind` is now legal on `/up` with values `msg` (default) and `contact_req`; on
   `/down` add `book` and `cfg`. Shapes and byte counts for each. Rules for `book` and `cfg` (not a
   thread entry, acked `shown` on apply, superseded not re-published) and for `contact_req` (no
   `from`/`body`, dedup on `id`, rate limits). `cfg` carries a `lock` map with `clear` (bool) and
   `auto` (minutes); unknown members of `cfg` are ignored, which is what makes it the home for
   future settings.
2a. **§4**: `shown` is explicitly *not* published for a body received while the device is locked
   (§5.8 of this plan); the message stays `sent` and takes the §5.3 re-publish path.
2b. **§2 topics**: a bootstrap namespace `pager/boot/{bid}/down` (relay → device, QoS 1,
   **retained = true**, raw bytes, ≤ 4 kB) and `pager/boot/{bid}/up` (device → relay, QoS 1), used
   only in Setup mode with the bootstrap credential; the "one subscription" rule holds per session.
   The 640-byte limit is explicitly scoped to `pager/{device_id}/…`.
3. **§3.3**: worst-case table gains the `n`/`sig` lines for both encodings; achievable JSON maxima
   473 / 449, CBOR ≈ 395 / ≈ 370; limit stays 640 for both.
4. **§3.4**: an envelope from an `hmac`-mode device without a valid `sig`, or outside the replay
   window, is malformed; the unsigned-LWT exception.
5. **§4.2 routing**: unchanged, but note `to` is now set by a recipient picker; §4.2 case 3 is what
   the device's greyed "not approved" entries prevent from ever being sent.
6. **§5.1 `/status`**: `bv`, and `rssi` now actually published.
7. **§5.3**: re-publish selection includes `book`; only the newest per device.
8. **§7.2/§7.3**: per-exchange rows re-derived for signed CBOR payloads (down message 75 B, ack
   44 B, up 55 B, status ≈ 78 B, loc ≈ 105 B); the daily totals fall slightly, the reconnect term is
   unchanged, and the verdict is unchanged.
9. **§9.2–9.4 storage**: the reply and unread *bodies* move from RTC to the `msgq` NVS namespace
   (§5.6); §9.3's table drops `pending_up[].body` and `unread[].body`, gains `auth.up_lo`,
   `auth.down_n`, `auth.down_bits`, `to[17]` × 2 and UI state, new total ≈ 460 B; §9.4's
   "ASCII by construction" argument and the lossy 161-byte mirror are withdrawn — a reply and the
   unread mirror are full 320-byte fidelity, and §9.6's survival table gains an NVS column that
   survives even a cold boot. Magic bump.
10. **§10 keymap**, now normative. Envelope and `/loc`: the existing `v=0 … err=11`, plus
    `n=12, sig=13, bv=14, name=15, ph=16, d=17, c=18, p=19, more=20`. `/status` shares the same
    map: `state=21, mode=22, batt_mv=23, rssi=24, session=25, fw=26, loc_period_s=27,
    loc_min_s=28` (`v`, `ts`, `bv`, `n`, `sig` as above). Bootstrap: `ok=29`, and the bundle's
    `pw=30, k=31, host=32, port=33, ca=34, flags=35, label=36, apn=37`; `cfg`: `lock=38`. Sub-maps: `loc` keeps
    `lat=0, lon=1, acc=2, fix_ts=3, src=4`; a `c[]` contact is `a=0, n=1, t=2`; a `p[]` request is
    `n=0, s=1`; `lock` is `clear=0, auto=1`. The table only ever appends.
11. **§11**: `book` is spent as a `/down` kind; `/cfg` remains reserved and unused, with the
    rationale from §4.3 recorded.
12. **New §14 Device authentication**: §2.3–2.6 of this document, normative.
13. **§12**: mark item 5 (provisioning) as owned by this plan, and record that provisioning is
    over the SIM (§3), not at flash time, and that flash encryption is deliberately off.
14. **§6.1 TLS row**: a second TLS profile (≥ 3, validation none) exists for the bootstrap hop only;
    profile 2 remains the pinned production profile.

---

## 9. Decisions for the human, collected

| # | Decision | Recommendation |
|---|---|---|
| H1 | Tag length 64 vs 128 bits (§2.2) | **Decided: 64** |
| H2 | Device verifies `/down` (`req_sig`) — on by default? | Yes; the relay sets the flag. Off is for a v1 device being kept around |
| H3 | Flash / NVS encryption | **Decided: off.** A stolen device is handled by revoke + a new setup code (§2.7) |
| H4 | EMQX Cloud Serverless exposes the auth/ACL HTTP API? (existing D2) | Verify with the real account before phase 2; manual-paste fallback is built either way |
| H5 | Approval by admin only, or also by the device owner | Admin only; the owner is the student |
| H6 | Book cap 10 approved + 4 requests | Accept; chunking is reserved |
| H7 | Every SMS contact becomes a user with an `sms` backend, so they can text back | Accept; it is the existing model and it is what makes grandma's reply work |
| H8 | Setup code by SMS (§3.6) | **Decided: not built.** Typed code only |
| H11 | Token length | **Decided: 8 bytes** (64 bits, 13 Crockford characters + check) |
| H12 | No CA validation on the bootstrap hop (§3.2 step 3) | Accept; the token-keyed AES-GCM is the authentication, and it keeps the CA out of the typed code |
| H13 | Nicknames device-local only, never synced to the web app | Accept; it keeps them off the wire and out of the allow-list model |
| H9 | Font | **Decided: Noto Sans, with Noto Sans CJK for the CJK ranges, pre-rasterised, upper + lower + CJK** (§5.2). Default repertoire `sc`; the other three stay a build flag |
| H10 | UI-awake window 30 s, keyboard poll 100 ms | Accept as compile-time defaults; retune on M-numbers |
| H14 | Wire encoding | **Decided: CBOR** for devices, JSON kept for tools (§2.4). Still open: hand-written subset codec (recommended; ~300 lines, no dependency, static buffers) vs the `espressif/cbor` registry component |
| H15 | IMEs | Not in this plan; the hook, Unicode buffers and `assets` partition are (§5.3). When one is wanted: hangul first (algorithmic, no dictionary), then pinyin or kana→kanji with a dictionary sized to the partition |
| H16 | Lock screen shows sender names (`preview`) by default | Yes; a parent's name on a locked screen is not a leak, and it is what tells a kid whether to bother unlocking |
| H17 | Auto-lock default 5 min; passcode optional or required | 5 min; optional, with a one-time prompt after setup. A parent can push `cfg lock.auto` from the web; a "required" policy is a one-field addition to `cfg` if wanted |

## 10. Unverified assumptions introduced by this plan

| § | Assumption | Cheapest experiment |
|---|---|---|
| 5.3 | CardKB sends `0xB4`–`0xB7` for arrows, `0x1B` esc, `0x09` tab | Part of M13, 5 min |
| 5.3 | CardKB holds the last key until read (enables key-to-wake) | Part of M13: press a key, wait 5 s, read; 2 min |
| 5.4 | `walter-modem` v1.5.0 exposes a signal-quality call usable from application code | Read `src/WalterModem.h`, 10 min, no hardware |
| 5.4 | LiFePO4 mV thresholds for the battery icon | Discharge curve under the device's own load, with M15 |
| 5.3 | I²C polling works across 100 ms light-sleep cycles | One afternoon with M1's current trace |
| 2.6 | EMQX built-in-database auth accepts the credential shape the relay pushes | Phase 2's e2e |
| 3.3 | Which public root EMQX Cloud Serverless's `*.emqxsl.com` certificate chains to | `openssl s_client -showcerts` against the real account; 2 min |
| 5.2 | Noto Sans CJK at 12 px, hinted and thresholded to 1 bit, is legible on this panel | Host-side PNG first, then M9 on glass; the fallback is 14 px for `normal` |
| 5.2 | `esp_partition_mmap` of a ~420 kB `assets` partition costs no RAM and no boot time worth noting | First build of phase 6; read the map file |
| 5.6 | An NVS write of a 320-byte blob completes in well under the 100 ms composer poll and never blocks the modem event task | Time it on the first phase 6 build; it runs on the main task either way |
| 5.8 | PBKDF2-HMAC-SHA256 at 10 000 iterations takes ≈ 50–100 ms on the S3 | Time it in the host-side test first, then on the device; adjust iterations to land near 100 ms |
| 3.2 | `walter-modem` v1.5.0 lets a second TLS profile use `WALTER_MODEM_TLS_VALIDATION_NONE`, and a retained message is delivered on subscribe through the modem's client | Read `tlsConfigProfile()`; phase 3's first bootstrap against the local EMQX |
| 3.2 | EMQX Cloud's REST publish honours `retain: true` | One call against the real account (D2) |
| 3.1 | Typing ~50 mixed characters on the CardKB is acceptable to a non-developer | First bring-up, with a stopwatch; if not, the Wi-Fi portal of §3.0 is the fallback |
