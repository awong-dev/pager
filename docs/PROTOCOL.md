# PROTOCOL.md — School Pickup Pager wire contract

**Status:** authoritative. The relay, the test client, the web app and the firmware
(`net.cpp`/`modes.c`/`msg.c`) MUST all conform to this document. Any topic or schema change edits
this file *first*, then the code — including anything `docs/SERVER_PLAN.md` implies, whose wire
changes are all recorded here.

**Scope:** a text and location relay, **plus** (v0.2, owner decision 2026-09-20, §3.6/§7.3) an
audited device-direct SMS path to a parent-managed allow-list. Geofences and schedule-based mode
switching are still out of scope; §11 records where the contract leaves room for them.

**Compatibility.** Payloads carry an explicit schema version (`v`, §3.1) and every field added
since the first release is optional, so a device that ignores all of them stays conformant and a
`v: 1` payload stays valid. Where a rule exists only to preserve that property, it says so.

**Conventions used below**
- `(see rationale)` in a table cell = a choice this document makes rather than inherits, with a
  one-line reason. None of them moves the §3.3 byte limit, changes a QoS or retained flag, or adds
  a device subscription.
- `NEEDS HUMAN DECISION` = would require a paid service, a firmware dependency beyond
  `walter-modem` / `esp_timer` / a display driver, or a GPIO change. Not decided here. Collected in §12.
- `UNVERIFIED` = a hardware or vendor-library fact this document assumes but cannot confirm; each
  one names the cheapest experiment that settles it.
- Power numbers are labelled `(estimate)`, `(vendor)` or `(TBD — hardware measurement)`. `(vendor)`
  means a figure documented by DPTechnics in the `walter-modem` source; everything else is still an
  estimate. **No number here is a measured number yet** — see `firmware/README.md`'s measurement
  checklists.
- `RESOLVED by source read` / `RESOLVED by header read` marks an assumption settled by reading
  `dptechnics/walter-modem` **v1.5.0** at
  `firmware/managed_components/dptechnics__walter-modem/`. Source line references are against that
  version; re-check them if the component is upgraded. v1.5.0 rewrote the MQTT API
  (event-driven receive, `mqttReceive()`, `mqttDidRing()` deprecated) — do not read conclusions
  drawn against older releases into this document.
---

## 1. Identifiers

| Thing | Format | Max len | Notes |
|---|---|---|---|
| `device_id` | `^[a-z0-9][a-z0-9-]{2,23}$` | 24 | lowercase; example `pgr-0001` *(bounds max topic length to 37 bytes so device-side topic buffers are static)* |
| MQTT client id (device) | exactly `device_id` | 24 | *(stable across reboots is required for `cleanSession=false` session resumption; see §6)* |
| MQTT client id (relay) | `relay-1` | — | *(fixed id so the relay also resumes a persistent session and does not miss `/up` while restarting.)* *(the relay no longer holds an MQTT session at all; see §2's transport note. The id is retained only for whatever short-lived client the relay or a test harness opens, and nothing on the device depends on it.)* |
| Message id (relay-originated, down) | `m_` + 8 lowercase hex | 10 | 32 bits of `os.urandom`; UNIQUE in the relay store, regenerate on collision |
| Message id (device-originated, up) | `u_` + 8 lowercase hex | 10 | from `esp_random()` |
| Location id (device-originated, `/loc`) | `l_` + 8 lowercase hex | 10 | from `esp_random()` *(a distinct prefix so a `/loc` envelope is identifiable in logs without its topic; it is still just an `id` per the rule below.)* |
| Session id | `s_` + 8 lowercase hex | 10 | per **cold boot**, not per deep-sleep wake; lives in RTC memory (§9) |

`id` is opaque to every consumer. Validators accept `^[a-z0-9_]{3,16}$` so a future generator can
change the prefix scheme without a protocol version bump.

---

## 2. Topics

Three topics, one subscription on the device. QoS and retained flags are decided here.

| Topic | Direction | QoS | Retained | Publisher | Subscriber |
|---|---|---|---|---|---|
| `pager/{device_id}/down` | relay → device | 1 | **false** (fixed) | relay | device only (`pager/{own_id}/down`) |
| `pager/{device_id}/up` | device → relay | 1 | **false** *(a retained reply would be redelivered to the relay on every relay reconnect and double-post to the thread)* | device | relay (`pager/+/up`) |
| `pager/{device_id}/status` | device → relay | **1** *(QoS 0 can silently lose the `online` edge that triggers re-publish of unacked messages)* | **true** (fixed) | device, and broker on LWT | relay (`pager/+/status`) |
| `pager/{device_id}/loc` | device → relay | **1** when `req` is non-null, **0** otherwise *(an answer to a location request is something a human is waiting on and must not be silently lost; an unsolicited periodic fix is superseded by the next one, so QoS 0 is right and cheaper)* | **false** *(a retained fix would be redelivered to the relay on every relay reconnect and re-post a stale position)* | device | relay (`pager/+/loc`) |
| `pager/boot/{bid}/down` | relay → device | 1 | **true** | relay | device only (setup mode) |
| `pager/boot/{bid}/up` | device → relay | 1 | **false** | device (setup mode) | relay |

- The device subscribes to **exactly one** topic per session: in normal operation, `pager/{own_id}/down`
  (QoS 1); in setup mode, `pager/boot/{bid}/down` (QoS 1). No wildcards on the device *(a wildcard
  subscription on a metered link is an unbounded data risk)*.
  `/loc` is a **publish**, and location requests arrive on `/down` as a
  `kind` (§3.2), not on a second subscription.
- **Byte limit**: The `pager/{device_id}/…` namespace is subject to the 640-byte limit (§3.3).
  The `pager/boot/{bid}/…` namespace carries the encrypted provisioning bundle and has its own
  **4 kB limit** per message.
- The relay receives `pager/+/up`, `pager/+/status` and `pager/+/loc`. It originally received
  them as an MQTT subscriber at QoS 1; the broker now pushes them to the relay — see the
  transport note below.
- Broker ACLs: device credentials may publish **only** to `pager/{own_id}/up`,
  `pager/{own_id}/status` and `pager/{own_id}/loc`, and subscribe **only** to
  `pager/{own_id}/down`. The `relay` credential gets the mirror image. Enforced at the broker, not
  just in code.
- Reserved-but-unused: `pager/{device_id}/cfg`, `/evt` (§11). Devices MUST NOT subscribe to them.

**Relay transport (the relay must not require a long-lived process).** The relay's
*role* is unchanged: it is the authoritative endpoint for `/up`, `/status` and `/loc`, and the only
publisher to `/down`. Its *transport* changes. Instead of holding a persistent MQTT session as
`relay-1`, the relay is reached by the broker's **rule engine**, which matches `pager/+/up`,
`pager/+/status` and `pager/+/loc` (including broker-generated LWTs) and POSTs each message to an
authenticated HTTPS endpoint on the relay; the relay publishes `/down` through the broker's **REST
publish API** at QoS 1, retained false. Consequences, and only these:
- `sent` (§4) now means *the broker's publish API accepted the QoS 1 message* — the same fact
  PUBACK reported, reported over a different channel. Every state, rule and timing in §4 is unchanged.
- The offline→online re-publish of §5.3 is triggered by the `/status` push rather than by a
  subscriber callback.
- The push is at-least-once: a broker rule may retry a POST it did not get a 2xx for, so ingest of
  `/up`, `/status` and `/loc` MUST be idempotent, which it already is — §4.1 rule 1 for acks,
  §4.2's transactional `id` dedup for up messages, §13.2's for `/loc`. This replaces QoS 1
  redelivery as the duplicate source; it does not add a new one.
- Nothing the device sees changes: topics, QoS, retained flags, ACLs, the §3.3 byte limit and the
  envelope schema are all identical, and a v1 device cannot tell the two transports apart. A relay
  that still runs as an MQTT subscriber remains conformant; this is a deployment choice.

---

## 3. Message schema (JSON or CBOR)

Payloads are **minified UTF-8 JSON objects or CBOR definite-length maps with integer keys** (§10),
no BOM, no trailing newline, no whitespace between tokens. One JSON object or CBOR map per MQTT
payload. Devices **SHOULD** emit CBOR; the relay accepts both on inbound topics and dispatches on
the first byte (`0x7B` = JSON `{`, `0xA0–0xBF` = CBOR map). The relay answers each device in the
encoding of its last `/status` (recorded as `devices/{d}.wire`). Values are identical in both
encodings — same strings, same enums, same numbers (`lat`/`lon` as float64) — only keys differ
(text vs integer), and `sig` is the one field whose type changes (byte string in CBOR, base64url
text in JSON). **`v` does not bump on an encoding-only change.**

Base envelope:

```json
{"v":1,"id":"m_7f3a","ts":1757700000,"from":"parent","body":"Pickup at 3:15 by the gym","ack":null}
```

### 3.1 Fields

| Field | Type | Required | Range / max | Meaning |
|---|---|---|---|---|
| `v` | int | no (default `1`) | `1` | Schema version. *(a one-key, 6-byte cost that makes §10 migration possible; absent MUST be read as `1`.)* |
| `id` | string | **yes** | `^[a-z0-9_]{3,16}$` | Message id (§1). On an ack, this is the **down message's** id. |
| `ts` | int | **yes** | 0 or 1×10⁹…2×10⁹ | Unix epoch **seconds, UTC**. Set by the publisher. |
| `from` | string | yes on content messages, **absent** on acks | `^[a-z0-9][a-z0-9_-]{0,15}$` **or** the literal `system`, ≤16 chars | Author's **alias**. *(a deployment has named users rather than one parent and one student, so `from` carries the sender's alias rather than a two-value enum. `parent` and `student` are ordinary aliases, so an older two-value payload is still valid. **Firmware impact: none** — `msg.c` accepts any 1–16 byte string and renders it verbatim.)* |
| `body` | string | yes on content messages, **absent** on acks | ≤ **160 Unicode code points** (fixed) **and** ≤ **320 UTF-8 bytes** *(the code-point cap alone allows 640 bytes; the byte cap lets firmware size static buffers, and §9.4 turns it into the 161-byte RTC mirror by way of the ASCII-only CardKB)* | Message text. |
| `ack` | string \| null | yes; `null` on content messages | `shown` \| `read` | Ack state being reported. |
| `kind` | string | no (default `msg`) | `msg` \| `loc_req` \| `contact_req` \| `book` \| `cfg` \| `sms_log`; `/down`: `msg`/`loc_req`/`book`/`cfg`; `/up`: `msg`/`contact_req`/`sms_log` | What the message *is* (§3.2). Absent MUST be read as `msg`. |
| `to` | string | no; `/up` content messages only | same regex as `from` | Recipient alias chosen by the device. Absent → the relay uses the device's configured default recipient, or broadcasts to every user the owner may message. *(the device can address one of several users; optional, so a device that never sets it works unchanged.)* |
| `n` | uint | no; signed envelopes only | 0…2⁵³-1 *(v0.2: widened from a 32-bit counter; see rationale)* | Per-device, per-direction replay counter (§2.4, §2.5, §14.2). Strictly increasing per publisher. |
| `sig` | bstr(8) in CBOR / base64url(8) in JSON | no; signed envelopes only | — | HMAC-SHA256 tag, truncated to 64 bits, MUST be the last pair (§2.4). |
| `bv` | int | `/status` only | 0…2³²-1 | Book version (§4.3, §5.1). |
| `name` | string | `contact_req` only | ≤16 code points, ≤48 UTF-8 bytes | Contact display name (§4.2). |
| `ph` | string | `contact_req` only | E.164 or absent | Phone number `+…` or alias reference (§4.2). |
| `d` | string | `book` only | same regex as `from` | Default recipient alias (§4.3). |
| `c` | array of objects | `book` only | ≤10 contacts | Approved contacts; each has `a` (alias), `n` (name ≤16 cp), `t` (type: `web`/`sms`/`chat`) (§4.3). |
| `p` | array of objects | `book` only | ≤4 pending requests | Pending `contact_req`; each has `n` (name), `s` (status: `pend`/`no`) (§4.3). |
| `more` | bool | `book` only | — | Reserved for chunking if the cap moves (§4.3). |
| `cfg` | object | `/down` `cfg` kind only | — | Configuration map carrying `lock` (object with `clear` bool and `auto` int minutes; a dangling cross-reference to "§5.8" for its full shape predates this table's current section numbering and is flagged, not fixed, here), `ca` (v0.2, §4.4) and `sms` (v0.2, §3.6 — the SMS contact allow-list). |
| `peer` | string | `sms_log` only | E.164 | The other party's phone number (§3.6). |
| `dir` | string | `sms_log` only | `out` \| `in` | Direction of the SMS this entry audits (§3.6). |
| `st` | string | `sms_log` only | `sent` \| `failed` \| `recv` \| `blocked` | Outcome of the SMS this entry audits (§3.6). |
| `sms_ts` | int | `sms_log` only | ≥ 0, epoch s | When the SMS itself was sent/received, which may differ from the envelope's own `ts` if the pager was offline and queued the audit entry (§3.6). |

Additional rules:
- `body` MUST NOT contain Unicode control characters `U+0000`–`U+001F` or `U+007F`. Relay strips
  them on ingest from the parent API and rejects if the result is empty. *(keeps
  JSON escaping bounded, and the e-paper renderer has no control-char handling.)*
- `body` MUST NOT be empty on a content message.
- **Unknown fields MUST be ignored, not rejected.** This is the forward-compatibility rule that
  makes §11 additive.
- Field order is unspecified; a receiver MUST NOT depend on it. Publishers SHOULD emit
  `v,id,ts,kind,from,to,body,ack,…,n,sig` in that order to keep logs diffable. **`sig` MUST be
  the last pair** (§2.4).
- A publisher SHOULD omit `kind` when it is `msg` and omit `to` when it has no recipient to name;
  both defaults are defined precisely so the common payload does not grow.
- `system` matches the alias regex, so it is a **reserved alias**: the relay MUST NOT issue it to a
  user *(§11's `system` enum headroom is only headroom if nothing else can claim it,
  and a user able to publish as `system` could forge relay notices)*. A `to` of `system` is an
  unknown recipient and takes §4.2's drop path. *(the reservation needs a matching
  inbound rule or it does not achieve what its rationale claims.)* `from:"system"` is **shape-valid
  on the wire** — wire-format validation checks shape only — but the relay MUST NOT attribute a
  stored message to `system` on the strength of a device-supplied `from`. Attribution of an `/up`
  message is the relay's decision from the device→user mapping, never the device's claim; a
  `from` that disagrees with that mapping is logged as a **security event** (same class as §4.1
  rule 4's wrong-device ack). This is a relay-side rule; it is deliberately *not* a reason to
  reject the envelope, so nothing the device sees changes.

### 3.2 Message kinds

| Kind | Topic | Shape | Notes |
|---|---|---|---|
| Down message (sender → device) | `/down` | `{"v":1,"id":"m_7f3a","ts":…,"from":"parent","body":"…","ack":null}` — **99 bytes** for the example above | Thread entry, rendered and acked. |
| Location request | `/down` | `{"v":1,"id":"m_7f3a","ts":…,"kind":"loc_req","from":"mom","ack":null}` — **78 bytes**; no `body` | Not a thread entry; device answers on `/loc` (§13.2). |
| Contact request (device → relay) | `/up` | `{"v":1,"id":"u_2b7c…","ts":…,"kind":"contact_req","name":"Grandma","ph":"+15551234567","ack":null,"n":…,"sig":"…"}` — ≈140 bytes; no `from`, no `body` | Requests admin approval (§4.2); rate-limited and deduped on `id`. |
| Book (relay → device) | `/down` | `{"v":1,"id":"m_…","ts":…,"kind":"book","bv":7,"d":"mom","c":[{"a":"mom","n":"Mom","t":"web"},…],"p":[{"n":"Uncle Bob","s":"pend"},…],"ack":null,"n":…,"sig":"…"}` — ≈590 bytes max | Not a thread entry; acked `shown` on apply (§4.3); only newest re-published. |
| Config (relay → device) | `/down` | `{"v":1,"id":"m_…","ts":…,"kind":"cfg","cfg":{"lock":{"clear":true,"auto":5}},"ack":null,"n":…,"sig":"…"}` | Not a thread entry; carries device settings; acked `shown` on apply (§5.8); only newest re-published. |
| Ack (device → relay) | `/up` | `{"v":1,"id":"m_7f3a","ts":…,"ack":"shown"}` — **51 bytes**; no `from`, no `body` | — |
| Up message (device reply) | `/up` | `{"v":1,"id":"u_91c0","ts":…,"from":"student","body":"ok coming","ack":null}` — **84 bytes** | Thread entry, routed to default recipient. |
| Up message, addressed | `/up` | `{"v":1,"id":"u_91c0","ts":…,"from":"student","to":"mom","body":"ok coming","ack":null}` — **95 bytes** | Thread entry, routed to named recipient. |
| SMS log (device → relay) | `/up` | `{"v":1,"id":"s_1a2b3c4d","ts":…,"kind":"sms_log","peer":"+12065550100","dir":"out","st":"sent","body":"On my way","sms_ts":…,"n":…,"sig":"…"}` — **170 bytes** signed JSON, **88 bytes** signed CBOR | Not a thread entry, not routed to anyone; audits the pager's own direct SMS send/receive (§3.6). QoS 1, deduped on `id` like any other up message. |
| Location | `/loc` | §13 | — |

A receiver distinguishes an ack from a content message by `ack !== null`. A payload with both a
non-null `ack` and a non-empty `body` is **malformed** (§3.4).

**`kind:"loc_req"` (see §13 for the answer it asks for).** A location request is a
down message with `kind:"loc_req"`, **no `body`**, `ack:null`, and `from` set to the requesting
user's alias. It is a request for a fix, not a message:

- The device MUST NOT `shown`- or `read`-ack it, and MUST NOT render it in the message thread. It
  is not a thread entry and it does not wake active mode. A redelivered `loc_req` (QoS 1 duplicate)
  MUST NOT cost a second fix: it is suppressed either by the §4.1 rule 7 dedup ring or by the
  rate limit of §13.3, both of which reach that outcome. There is no ack to re-send.
- The device answers on `pager/{own_id}/loc` (§13) with `req` set to **this message's `id`**,
  subject to the device-side rate limit in §13.3.
- Relay-side lifecycle: `queued → sent → fulfilled` (a `/loc` carrying a matching `req` arrived) or
  `expired`. Like §4's `expired`, `fulfilled`/`expired` are **derived at read time**; the expiry is
  **15 minutes** after creation *(a location answer that is a quarter of an hour late
  answers a question nobody is still asking; it is also short enough that a derived state needs no
  timer)*.
- A `loc_req` is **not** re-published on an online edge (§5.3) — a stale location request is
  worthless, and re-asking is one API call for the requester.
- **Firmware note:** current firmware treats a body-less down message as malformed and drops it
  *without acking* (§3.4). That is exactly the right behaviour for a device that predates this
  field: the request is never answered, it simply expires, and no message state moves backwards.
  Parsing `kind` is a firmware follow-up, not a prerequisite for the rest of the contract.

**`kind:"contact_req"` (device → relay).** A contact request is an up message with
`kind:"contact_req"`, `ack:null`, and either `ph` (E.164 phone number) or neither `ph` nor `body`
(for an alias reference — use `name` as a display name and the relay infers the alias from context
or returns an error). Fields: `name` is required (1–16 code points, ≤48 UTF-8 bytes); `ph` is
optional (E.164 or absent). The device includes `n` and `sig` as with any `/up` from an `hmac`
device. The request is deduped on `id` like any up message and is rate-limited: **at most 5 pending
requests per device**. A request whose `ph` or implied alias matches an existing pending/approved
contact is a no-op. The device stores requests locally as `pending` (§4.2, §5.6).

**`kind:"book"` (relay → device).** An address book is a down message with `kind:"book"`, `ack:null`,
carrying the approved contacts and pending requests for this device:
- **Not a thread entry:** the device MUST NOT render it in the message thread, and MUST NOT `shown`-
  or `read`-ack it in the normal sense. Instead, the device acks with `shown` **once the book has
  been applied** (atomically written to NVS), which is what signals to the relay that the book has
  landed.
- **Newest only:** the relay expires any older unacked `book` when it creates a new one. Re-publish
  on an online edge (§5.3) includes `book`.
- The payload carries `bv` (book version), `d` (default recipient alias), `c[]` (approved contacts,
  max 10), `p[]` (pending requests from §4.2, max 4, each with status `pend` or `no`), and `more`
  (reserved for chunking).
- A `/down book` is signed by the relay (§2.4, §2.6), carrying `n` and `sig`.

**`kind:"cfg"` (relay → device).** A configuration message is a down message with `kind:"cfg"`,
`ack:null`, carrying device settings that only the relay can modify (passcode lock, auto-lock
timing, CA trust, the SMS contact allow-list, etc.). Fields: `cfg` is an object that currently
holds `lock` (see §5.8 for structure), `ca` (v0.2, §4.4) and `sms` (v0.2, §3.6/§10 — an array of
`{n, p}` objects, the device's *whole* SMS contact allow-list, replaced wholesale on every push;
`[]` is a legal push meaning "no SMS contacts"). Unknown members of `cfg` are ignored, making this
the home for future settings.
- **Not a thread entry:** the device MUST NOT render it in the message thread, and MUST NOT
  `shown`- or `read`-ack it in the normal sense. Instead, the device acks with `shown` **once the
  config has been applied**.
- **Newest only:** the relay expires any older unacked `cfg` when it creates a new one. Re-publish
  on an online edge (§5.3) includes `cfg`.
- A `/down cfg` is signed by the relay, carrying `n` and `sig`.

### 3.3 Envelope size limit

**Hard limit: 640 bytes** for the `pager/{device_id}/…` namespace, unchanged. Any payload larger than
640 bytes MUST be dropped unparsed by both relay and device.

Worst case, with every field at its maximum and JSON escaping expanding `"` and `\` to two bytes.
**This table is a deliberately conservative per-field ceiling, not a payload that can exist**: it
sums each field's independent maximum, and several of those maxima are mutually exclusive.

**JSON, unsigned (v1):**
```
{} 2
"v":1, 6
"id":"<=16>", 24
"ts":1757700000, 16
"from":"<=16>", 26
"body":"<=320>", 330
"ack":"<=8>", 16
  ----
  420 v1 down msg, text fields only

"to":"<=16>", 24
  ----
  444 up msg with to

"sig":"<11 base64url chars>", 20   (the tag is 8 bytes, §3.1, which is 11 base64url characters)
  ----
  464 up msg with to and sig (signed)
```

**JSON, signed (with `n` and `sig`):**
```
"n":9007199254740991, 19  (v0.2: n's max grew from 2^32-1 (10 digits) to 2^53-1 (16 digits), +6 B)
"sig":"<11 base64url>", 20
  ----
  449 down msg, signed
  
  ----
  425 up msg without to, signed
```

**CBOR, signed (with `n` and `sig`):**
- Down message 75 B, up message 55 B, `/status` ≈78 B, location ≈105 B — roughly 25% smaller than
  signed JSON.

**Achievable figures:** The largest payload that can actually exist is ≈438 bytes (JSON, unsigned):
a `/up` content message with `id`, `from` and `to` all 16 characters and `body` at its cap. With
signing added, a signed JSON down message or book reaches ≈479 bytes (v0.2: +6 B over v0.1's
≈473, from `n`'s wider maximum); signed CBOR is ≈395 bytes (CBOR encodes `n` at minimal length, so
it does not grow until an epoch value actually needs more bytes, §14.2).
A maximal `/down` `loc_req` is 101 bytes (78 in §3.2's example). §13's `/loc` envelope is ≤ ~200
bytes. Real headroom against the 640-byte limit is **≥ 166 bytes** for the largest payloads, and
typical shapes have over 300 bytes.

*(the one escaping assumption the limit depends on, stated because it was previously
implicit.)* Publishers MUST serialise non-ASCII `body` characters as **raw UTF-8, not `\uXXXX`
escapes**. §3's "minified UTF-8 JSON" already implies this, but the consequence is load-bearing:
a legal 160-code-point non-ASCII `body` emitted with `\u` escaping is 1078 bytes and would be
dropped unparsed by every receiver. The relay and the device both already emit raw UTF-8; this
fixes that in writing.

### 3.4 Malformed payload handling

A payload is malformed if it is >640 bytes, not valid UTF-8, not a JSON or CBOR object, missing
`id`/`ts`/`ack`, has an out-of-range field, violates a `body` rule, sets both `ack` and `body`,
or (for a device-originated envelope on an `hmac`-authenticated device) lacks a valid `sig` or
falls outside the replay window. The one exception: an unsigned `/status` with `state:"offline"` is
accepted **only** from the broker-generated LWT, never from a device publishing an up message.

*(unknown **values** need the same rule as unknown fields, or the enum additions in
§3.1 are not additive after all.)* A `kind` the receiver does not recognise is handled exactly like
`loc_req` is handled by text-only firmware as: **do not render, do not ack, count it, drop it.** The message
then expires at the relay, which is the visible, correct outcome. This is the only place a newer
receiver may treat a well-formed envelope as undeliverable; it is deliberately the same code path
as §3.4's malformed handling below.

- **Device:** log, increment a counter, **do not ack**, do not render, do not reboot. The message
  stays `sent` at the relay and the parent UI shows it as undelivered.
- **Relay:** log with the topic and first 64 bytes, drop. Never crash the ingest path on a parse
  error — one bad payload must not take down the persistent session, and must not fail the
  broker's webhook request either: the relay logs it, drops it, and still answers 2xx, or the
  broker will redeliver the same bad payload forever. Never auto-reply on MQTT. *(one exception, and
  the only one: an up message whose `to` names an unknown or disallowed recipient is answered with
  one `system` down message — see §4.2. That is a routing failure of a well-formed payload, not a
  parse failure.)*
- The relay's HTTP API rejects oversize/invalid bodies with `400` **before** anything is stored, so
  a malformed message never reaches the air interface.

### 3.5 Clock

The device sets `ts` from the LTE network clock obtained at attach (NITZ / modem RTC via
`walter-modem`). If no network time is available yet, the device publishes `ts: 0` and the relay
substitutes its own receive time. *(avoids adding an SNTP/`esp_netif` code path
purely for timestamps; the relay is already authoritative for thread ordering.)*
Thread order in the parent UI is the relay's insertion order (SQLite rowid), **not** `ts`.

### 3.6 `kind:"sms_log"` (device-direct SMS, v0.2)

Owner decision (2026-09-20, `V02_DESIGN.md` §6): the pager may send and receive SMS **directly
through its own modem**, to a phone-number allow-list the device's owner (or an admin) manages in
the web app — a delivery path that works without the relay, for exactly the case the relay itself
cannot cover (the relay and the broker are both down, or the parent's own phone has no data). This
reverses this document's own earlier "no device-side SMS path" rule (§7.3's SMS budget section);
the pager has **no UI of its own** to add, edit or remove an allow-list entry — only the parent, in
the web app, can.

- **The allow-list itself is not a wire message.** It travels as `/down cfg.sms` (§3.2's `cfg` kind,
  §10's `cfg.sms[]` sub-map): an array of `{n, p}` (name, E.164 phone), the *whole* list every time,
  newest-wins exactly like `cfg.lock`/`cfg.ca`, acked `shown` once the device has written it to NVS.
  Max 8 entries. The relay's own name-length cap for this list (24 UTF-8 bytes, tighter than
  `book`/`contact_req`'s 48-byte name cap) exists purely so 8 maximal entries always fit under the
  640-byte envelope limit in *both* wire encodings — see the byte arithmetic in the relay
  implementation task's report; a future revision that widens the name cap back to 48 bytes would
  first have to either shrink the entry cap or move to CBOR-only delivery for this one kind.
- **Sending.** A message to an SMS contact goes out through the modem's own `smsSend()`, never
  through `/down`/`/up` at all — the relay is not in this path.
- **Receiving.** An inbound SMS from a listed number is inserted into the on-device thread and
  alerts like any other message; from an unlisted number, it is **never shown** to the student —
  only logged.
- **The audit trail, non-negotiable:** every SMS in either direction — sent, failed, received, or
  blocked — produces exactly one signed `/up` envelope, `kind:"sms_log"`, id `s_` + 8 hex, QoS 1,
  deduped on `id` exactly like any other up message (redelivery-safe). Fields, beyond the base
  envelope's `id`/`ts`/`n`/`sig`:

  | Field | Type | Range | Meaning |
  |---|---|---|---|
  | `peer` | string | `^\+[1-9]\d{6,14}$` (E.164) | The other party's phone number. |
  | `dir` | string | `out` \| `in` | Direction of the SMS this entry audits. |
  | `st` | string | `sent` \| `failed` \| `recv` \| `blocked` | Outcome: `sent`/`failed` for `dir:"out"`; `recv`/`blocked` for `dir:"in"` (`blocked` = sender not on the allow-list, never shown). |
  | `body` | string | ≤160 Unicode code points; **empty allowed** | The SMS text. Unlike an ordinary content message's `body` (§3.1), an empty `body` is legal here — some real handsets/gateways deliver a body-less SMS, and this is an audit record of what happened, not a thing a human reads on the pager's screen. |
  | `sms_ts` | int | ≥ 0, epoch s | When the SMS itself was sent/received — differs from the envelope's own `ts` whenever the pager was offline and queued the entry. |

  Example (JSON, signed, ~170 bytes; ~88 bytes as signed CBOR):
  ```json
  {"v":1,"id":"s_1a2b3c4d","ts":1757700000,"kind":"sms_log","peer":"+12065550100","dir":"out","st":"sent","body":"On my way","sms_ts":1757700000,"n":12,"sig":"…"}
  ```
- **Not a thread entry, not routed to anyone.** The relay stores it under
  `devices/{deviceId}/smsLog/{logId}` (`logId` = this envelope's own `id`) and nothing else — no
  `messages/{id}` row, no delivery/backend fan-out, no push notification. It is read back only
  through `GET /api/devices/{id}/sms-log` (owner or admin), never through a Firestore listener.
- **Offline durability.** If the pager is offline when an SMS is sent or received, the audit entry
  is queued in NVS (at least 16 entries) and published once the session is back; **sent/received
  first, logged second** — the whole point of this path is that it works without the relay, so a
  slow or failed audit publish must never block or roll back the SMS itself. If the queue fills, the
  oldest entry is dropped and the drop is counted in `/status`'s `sms_lost` (§5.1) rather than
  silently lost with no trace at all.
- **Retention.** No dedicated sweep exists yet for `smsLog` (unlike `messages`/`locations`, §5.7) —
  entries accumulate until a future retention task adds one. Flagged as a known gap, not a decision
  to keep them forever.
- **Compatibility.** All of §3.6 is new in v0.2; a v0.1 device sends no `sms_log` and applies no
  `cfg.sms`, and is unaffected — every field here is on a kind (`sms_log`) and a sub-map (`cfg.sms`)
  such a device never emits or parses.

---

## 4. Ack state machine (down messages)

**The relay is authoritative.** The device is a reporter of events; it never holds the canonical
state. States are **monotonic** — a message never moves backwards.

```
  relay stores broker accepts device /up ack device /up ack
 (HTTP POST) ------------> queued --------------> sent --------------> shown -------------> read
  (PUBACK, or 2xx
  from the publish
  API — §2)
  | |
  | 24 h, no ack | 24 h, no ack
  +----------------------+--------> expired (terminal, relay-only)
```

| State | Owner | Entered when | Notes |
|---|---|---|---|
| `queued` | relay | message committed to the relay's store by the send API | Also the state of a message whose publish attempt failed (broker down). |
| `sent` | relay | broker returns **PUBACK** for the QoS 1 `/down` publish — equivalently, the broker's REST publish API accepts the QoS 1 publish with a 2xx | Means *the broker accepted it*, **not** that the device received it. The UI must not say "delivered" here. *(the two are the same fact carried over different transports (§2), so the state machine, its rules and its timings are untouched.)* |
| `shown` | relay, on device report | device publishes `{"id":…,"ack":"shown"}` on `/up` | Device publishes this **after the e-paper refresh completes** (BUSY deasserted), never before. **Exception:** if the device is locked, `shown` is **not** published for a message that arrived while locked; the message stays `sent` and is re-published on an online edge (§5.3). |
| `read` | relay, on device report | device publishes `{"id":…,"ack":"read"}` on `/up` | Triggered by a short press of button IO1 while the message is on screen. |
| `expired` | relay | still `queued` or `sent` 24 h after creation | *(a pickup pager delivering "be at the gym at 3:15" two days late is worse than not delivering it. A terminal, relay-only state; the device never sees or acks it.)* |

### 4.1 Rules

1. **Idempotent.** A repeated ack for a state already reached is a no-op (log at debug). QoS 1 makes
  duplicates normal, not exceptional.
2. **Out of order.** A `read` arriving without a prior `shown` promotes the message to `read` and
  back-fills `shown_ts = read_ts`. `shown` arriving after `read` is ignored.
3. **Unknown id.** An ack for an id the relay has never stored is logged and dropped. It MUST NOT
  create a row (that would let a compromised device inject thread entries).
4. **Wrong device.** An ack arriving on `pager/A/up` for a message addressed to device `B` is
  dropped and logged as a security event.
5. **No timeout state change.** Between `sent` and `shown` there is no automatic transition other
  than the 24 h `expired` sweep; the UI derives "not yet delivered" from `sent` + age.
6. **Device-side ack durability.** An ack that could not be published (no session, PUBACK missing)
  is kept in an RTC-memory pending list (§9) and retried on the next wake, up to **3 attempts**,
  then dropped. Re-publish on device `online` (§5.3) covers the dropped case.
7. **Device-side dedup.** The device keeps the last **16** received message ids in RTC memory,
  stored as a 32-bit digest per id rather than the literal string (§9.3). A
  duplicate delivery is **re-acked** (relay is idempotent) but MUST NOT re-render, re-alert or
  re-enter active mode. *(prevents a QoS 1 redelivery storm after a reconnect
  from costing a full active-mode window per duplicate, which is the single most expensive
  failure mode in the power budget: ~2–3 mAh per spurious 10-minute window, estimate.)*

### 4.2 Up messages (student replies)

**Up messages have no relay-side lifecycle beyond storage, and the relay sends no ack back to the
device** *(an ack-of-reply would cost one extra down message plus one extra
radio wake per reply for information the device already has from its own PUBACK; the parent page is
the endpoint that matters)*.

Device-side only, in RTC memory: a **2-entry** queue holding the reply body at full fidelity
(§9.3, §9.4). A reply is never truncated to fit; the composer refuses input past 160 bytes
instead.

```
compose --> pending --(modem PUBACK)--> done (entry freed)
  |
  +--(3 failed attempts, or >2 h old)--> failed (UI shows "not sent", entry freed)
```

Relay-side an up message is simply inserted with `direction='up'` and served to the reading UI. An
up message whose `id` already exists is a duplicate and is dropped (QoS 1 redelivery); the dedup
check on that `id` MUST be part of the same transaction that stores the message, or a QoS 1
redelivery that races the original posts the thread entry twice.

**Routing an up message's `to` (the allow-list is a server-side rule, so the device
is never trusted to know who it may talk to).**

1. `to` absent → the relay routes to the device's configured default recipient, or, if it has
  none, to every user the device's owner is allowed to message. This is what a device that never
  sets `to` gets, so such a device works with no change.
2. `to` present and naming a recipient this device's owner is allowed to message → routed there.
3. `to` present but naming an unknown alias, or one the allow-list does not permit → the relay
  **drops the message**, logs it as a **security event** (same class as §4.1 rule 4's wrong-device
  ack), and sends exactly one `system` down message whose body is `unknown recipient` so the
  device's user gets feedback instead of silence. The dropped message is not stored and is never
  delivered to anybody, and the reply is rate-limited to one per offending up message — it is a
  reply, never an alert loop.

The allow-list decision is made by the relay **and** re-stated in the data store's own access
rules; neither alone is the enforcement point. Case 3's `system` reply is the single exception to
§3.4's "never auto-reply on MQTT".

---

## 5. Status / LWT contract

Topic `pager/{device_id}/status`, **QoS 1, retained = true**, both for device publishes and for the
broker-generated LWT.

### 5.1 Online payload (117 bytes for this example)

```json
{"v":1,"state":"online","mode":"sleep","batt_mv":3280,"rssi":-95,"session":"s_3ab91c02","ts":1757700000,"fw":"0.1.0"}
```

| Field | Type | Required | Range | Meaning |
|---|---|---|---|---|
| `v` | int | no (default 1) | `1` | Schema version |
| `state` | string | **yes** | `online` \| `offline` | See §5.2 for the precise meaning |
| `mode` | string | yes when `online` | `sleep` \| `active` | Device mode (firmware/README.md) |
| `batt_mv` | int | yes when `online` | 2000…4500 | Battery millivolts. *(raw mV, not percent; LiFePO4 has a flat 3.2 V plateau so any percent mapping belongs in the UI where it can be changed without a firmware flash.)* |
| `rssi` | int | yes when `online` | −140…0 | RSSI in dBm (now published at every `/status` for field debugging and status bar rendering). |
| `session` | string | **yes** | `^s_[0-9a-f]{8}$` | Cold-boot session id (§1). Lets the relay tell a reboot from a deep-sleep cycle. |
| `ts` | int | yes when `online` | epoch s, or 0 | Same rule as §3.5 |
| `fw` | string | no | ≤16 chars | Firmware version |
| `bv` | int | no | 0…2³²-1 | Book version (§4.3). Reported so the relay can detect a factory reset or a lost book message and re-publish. |
| `loc_period_s` | int | no | 0…86400 | The periodic `/loc` interval **the device has chosen** (§13); `0` = periodic location off. |
| `loc_min_s` | int | no | 0…86400 | The device's own minimum gap between on-demand fixes (§13.3); default 120. v0.2 firmware, which uses the growing backoff of §13.3's amendment, reports **600**: the floor it keeps since its last attempt even after a backoff reset. |
| `tls` | string | no | `unpinned` \| `pinned` \| `broken` | *(v0.2, `V02_DESIGN.md` §4.1)* CA trust state: `unpinned` (no CA in the identity, validation off, by choice, not a fault), `pinned` (CA set, last connect validated), `broken` (CA set, last validated connect failed, running with validation off as a reachability fallback — §13.3's "pages still arrive" rule applies here too). Absent means firmware older than v0.2. |
| `ca_fp` | string | no | 16 lowercase hex chars | *(v0.2)* First 16 hex characters of the SHA-256 of the pinned CA PEM (the same digest carried in the bootstrap bundle's `ca_sha`/a `cfg.ca.sha` push, §4.4). Absent when `tls` is `unpinned` or absent. |
| `loc_backoff_s` | int | no | 0…86400 | *(v0.2, §13.3)* Seconds until the device's own growing location-attempt backoff next allows a fresh fix attempt; `0` = an attempt is allowed now. See §13.3's amendment for how this relates to `loc_min_s`. |
| `sms_lost` | int | no | ≥ 0 | *(v0.2, §3.6 — device-direct SMS)* Count of `sms_log` audit entries dropped from the device's NVS queue for lack of space; normally 0. |

*(all six fields above are **display and diagnosis only**; the relay stores the reported
values and never writes them back. The device owns its location duty cycle because the cost being
traded is GNSS power on its battery (§12 item 8), which the server cannot see. Making these
server-settable would need a `/cfg` topic, which §11 still only reserves.)* They are optional, so
a `/status` without them remains valid, and a relay MUST treat their absence as "unknown", not as `0`.
**Compatibility (§0):** every field in this table added since the first release — these five
included — is optional, and an *older* relay MUST NOT reject a `/status` merely because it carries
a field the relay predates (§3.1's "unknown fields MUST be ignored" already covers this; called out
again here because it is the exact case v0.2 shipped the relay's own acceptance of these fields
ahead of any firmware sending them, per the "add the relay's acceptance ... before any firmware
that sends them is flashed" rule).

### 5.2 LWT payload (48 bytes)

Registered in CONNECT, so it cannot contain live data:

```json
{"v":1,"state":"offline","session":"s_3ab91c02"}
```

**`state` means "the MQTT session is alive", not "the ESP32 is awake."** In sleep mode the modem
keeps the TLS+MQTT session up on eDRX while the ESP32 is in deep sleep; that device is `online` with
`mode:"sleep"`. This distinction is load-bearing for the UI copy — do not render `online` as
"awake".

### 5.3 Relay behaviour on status

- On a retained `offline` or an LWT `offline`: mark the device offline; the parent UI shows
  "last seen <time>". No message state changes (§4.1 rule 5).
- On an `online` whose `session` **differs from the last seen `session`**, or on any
  offline→online edge: **re-publish unacked messages and the newest `book` and `cfg`**. Broker QoS 1
  covers the common case; this covers session loss.
  - Order: oldest first.
  - Selection: state in (`queued`, `sent`), age < 24 h; plus the newest unsent `book` (§4.3) and
    the newest unsent `cfg` (§5.8), one each, which have their own status tracking. Older
    `book`/`cfg` messages already have a newer one and are never re-sent.
  - Cap: **at most 10 messages per online edge** *(matches the device's 10-entry thread ring for
    unacked down messages; `book` and `cfg` are not counted against this cap since they are not
    thread entries)*.
  - Re-publish reuses the **same `id`** so device dedup (§4.1 rule 7) suppresses double-rendering
    and deferred acks.
  - **Selection excludes `kind:"loc_req"` (a location request that missed its window is worthless,
    and re-asking is one API call for the requester).** A `loc_req` that is still `sent` when the
    device comes back simply expires per §3.2.
  - *(same rule, new trigger.)* The edge is detected from the `/status` message however it reaches
    the relay: today that is the broker's `/status` push (§2), not a subscriber callback. Selection,
    order, cap and the re-used `id` are unchanged.
- The relay MUST NOT publish anything to `/down` on a timer for liveness. There is no application
  ping. MQTT keepalive is the only liveness mechanism (§6).

### 5.4 Device publish cadence

Status is published: (a) immediately after MQTT connect, (b) on every mode change, (c) when
`batt_mv` has moved more than 50 mV since the last publish, and (d) as a heartbeat, at most
**once per 3600 s**.

*(the heartbeat period is 2× the MQTT keepalive interval (§6). Marginal cost
≈ 0.33 kB of data and ≈ 0 extra radio sessions. An independent hourly status timer would add ~24
radio wakes/day ≈ 2.4 mAh/day (estimate) — a meaningful tax for nothing. **Update:** the
keepalive wake it used to piggyback on no longer exists (§6.2), so the heartbeat now rides an
ordinary 5 s poll wake — the ESP32 side is free, and the only cost is the one radio session the
publish itself needs. Implementation is a counter in RTC memory, not a timer: publish when
`now - last_status_epoch >= 3600`.)*

Status is **never** published on a plain paging wake or on receipt of a down message.

---

## 6. Session, keepalive and eDRX

### 6.1 Session

| Parameter | Value | Rationale |
|---|---|---|
| MQTT version | **3.1.1** | ** **RESOLVED by header read**: the library exposes no version, no `sessionExpiry`, and no MQTT 5 property API anywhere. `mqttConnect()` emits `AT+SQNSMQTTCONNECT=0,<host>,<port>,<keepAlive>` (`src/proto/WalterMQTT.cpp:83-95`). 3.1.1 it is. |
| Clean session | **false** — **NOT SETTABLE from the library** | Required in spirit by the "one persistent session" constraint: with `cleanSession=false` and a stable client id the broker queues QoS 1 `/down` while the TCP link is briefly down. **Finding:** neither `mqttConfig()` nor `mqttConnect()` exposes a clean-session flag, so the value is whatever the Sequans MQTT client defaults to. UNVERIFIED, and load-bearing for §4.1 rule 6 and §5.3. Weak evidence against us: `mqttConnect()` *deliberately clears the entire local subscription table* before connecting (`src/proto/WalterMQTT.cpp:87-89`), and the vendor's own `examples/mqtts` re-subscribes from inside the CONNECTED event handler — which is what a library assuming a **clean** session on every connect would look like. Experiment: connect, reset the ESP32 only, and have the relay publish while the device is down — if the queued message arrives on reconnect, the session is persistent. ~20 min on hardware. |
| LWT | **NOT SETTABLE from the library** | `mqttConfig()` emits `AT+SQNSMQTTCFG=0,"<clientId>"[,"<user>","<pass>"][,<tlsProfileId>]` and stops there (`src/proto/WalterMQTT.cpp:53-75`) — no will topic, message, QoS or retain argument, and grepping the whole of `src/` for `will`/`lastwill` returns nothing. §5.2's LWT contract therefore has no implementation path through the typed API. Fallback: `WalterModem::sendCmd()` (public) can queue a raw `AT+SQNSMQTTCFG=...` carrying the will parameters *before* `mqttConnect()`. UNVERIFIED against the Sequans AT manual. If that fails, the relay must fall back to inferring offline from keepalive expiry and §5.2's LWT becomes advisory. Tracked in §12. |
| TLS (production) | TLS 1.2; server certificate validated **only if the bootstrap bundle carried a CA** (`DEVICE_PLAN.md` §3.3, default: none, validation off); username/password per device; profile 2. **The profile MUST name certificate slot 12 even with validation off** — with no slot named the modem's MQTT engine silently sends plaintext MQTT to the TLS port (verified on hardware, `DEVICE_PLAN.md` §3.3). | Pinned mode is the free-tier HiveMQ Cloud model. Provisioning is the vendor's `examples/mqtts` flow: `tlsWriteCredential(false, 12, ca_pem)` → `tlsConfigProfile(2, WALTER_MODEM_TLS_VALIDATION_CA, WALTER_MODEM_TLS_VERSION_12, 12)` → `mqttConfig(client_id, user, pass, 2)`. Both functions are public (`src/WalterModem.h:4147` and `:4404`). **Slot discipline: certificate slots 0–10 and private-key index 1 are reserved for Sequans/BlueCherry — use ≥ 11, and TLS profile ≥ 2 (profile 1 is BlueCherry's).** |
| TLS (bootstrap) | no server-cert validation; profile 2 (the library caps profile ids at 0–2, and bootstrap and production never share a power cycle) | For the one-time setup fetch (§3.2): `tlsConfigProfile(2, WALTER_MODEM_TLS_VALIDATION_NONE, WALTER_MODEM_TLS_VERSION_12, 12)`. **Verified on hardware:** the slot argument is required — `no_cert` makes the MQTT engine send plaintext (see the production row). The bundle is authenticated and encrypted under a token-derived key, so server authentication on this hop adds only DoS resistance. |
| Reconnect policy | **Only** on detected session loss. Never on a timer. Backoff 5 s, 15 s, 60 s, 300 s, then 300 s steady. | Each reconnect costs a full TLS handshake ≈ 5 kB (§7) — reconnects are the largest single term in the data budget. |

*(scoping note, no behaviour change.)* Every row above describes the **device's**
session. The relay no longer keeps an MQTT session of its own (§2), so its former persistent
session, its `relay-1` client id and its reconnect behaviour are not part of this contract; the
broker-generated LWT of §5.2 still matters, because it is the broker, not the relay, that produces
it. The broker must therefore still support an LWT and QoS 1 in both directions (§12 item 2).

### 6.2 Keepalive

| Parameter | Value |
|---|---|
| MQTT keepalive | **1800 s (30 min)** *(see arithmetic below)*, passed as the third argument of `mqttConnect(host, port, keepAlive)` |
| ESP32 RTC-timer wake for keepalive | **None.** The MQTT client runs *inside* the modem and the library exposes **no ping API at all** — there is no `mqttPing()`, and `keepAlive` is handed to the modem in the `AT+SQNSMQTTCONNECT` command. PINGREQ is therefore the modem's job by construction, not the ESP32's. The old wake source #3 does not exist. |
| Server Keep Alive override | If the broker documents a lower cap, the **smaller** value wins. Note `WALTER_MODEM_MQTT_MIN_PREF_KEEP_ALIVE` (`src/WalterModem.h:236`, value 20) is still declared but referenced nowhere in v1.5.0, so the library imposes no floor of its own. Whether the modem or broker silently clamps 1800 s is UNVERIFIED. |

Arithmetic behind 1800 s: a PINGREQ/PINGRESP pair costs ~0.18 kB of data (negligible) but requires
an RRC connection — roughly 3 s at ~120 mA ≈ 0.1 mAh (estimate). At 1800 s that is 48 pings/day
≈ **4.8 mAh/day**; at 900 s it is 96 pings/day ≈ 9.6 mAh/day. Against a sleep-mode budget of roughly
20 mAh/day, halving the keepalive would cost ~25 % of the battery for no latency benefit — down
messages arrive via paging (§6.3), not via the keepalive. Longer than 1800 s risks carrier NAT
timeout on the TCP flow. `PAGER_MQTT_KEEPALIVE_S` is a compile-time constant so this can be retuned
it against a measurement.

Note the ~4.8 mAh/day keepalive term is now a **modem-side** cost only: the ESP32 does not wake for
it, so it is unaffected by the sleep-cycle rewrite in §8.

UNVERIFIED: carrier NAT idle timeout on this SIM. Cheapest experiment: leave the device idle with
keepalive disabled and log how long before the first publish fails; bisect 5/15/30/60 min. Costs one
overnight run, no extra hardware.

### 6.3 eDRX (sleep mode)

| Parameter | Value | Note |
|---|---|---|
| eDRX cycle | **20.48 s** (fixed) | 3GPP WB-S1 eDRX value nibble `"0010"`. **RESOLVED by source read**: `configEDRX(mode, req_edrx_val, req_ptw)` takes `const char *` and splices both verbatim into `AT+SQNEDRX=<mode>,<actType>,"<req_edrx_val>","<req_ptw>"` (`src/WalterModem.cpp:4757-4774`). They are **raw 4-bit binary nibble strings**, not seconds. Firmware passes the literals `"0010"` / `"0001"`. |
| PTW (Paging Time Window) | **2.56 s** *(with a 1.28 s idle DRX this gives 2 paging occasions per cycle, i.e. one retry, at ~0.2 mA average; PTW 5.12 s would give 4 occasions and roughly double the paging energy for a redundancy we do not need)* | WB-S1 PTW nibble `0001` |
| PSM | **disabled** *(PSM suspends paging entirely, which breaks the ≤30 s sleep-mode delivery target. Revisit only if a scheduled/mailbox mode is added (§11).)* | Firmware calls `configPSM(WALTER_MODEM_PSM_DISABLE)` explicitly rather than relying on a default. Two traps if PSM is ever enabled, both still present in v1.5.0: `configPSM()` takes `const char *` for T3412/T3324 but the helpers `durationToTAU()` / `durationToActiveTime()` return a `uint8_t` (`src/WalterModem.h:5681,5699`), so the caller must format the byte as an 8-character binary string itself; and `_convertDuration()` (`src/WalterModem.cpp:4796`) writes `final_base * multiplier` — the *index* into the base-time table — into `actual_duration_seconds` instead of `base_times[final_base] * multiplier`, so the reported actual duration is wrong. Neither affects this project while PSM stays off. |
| Carrier acceptance | The network may grant different eDRX/PTW values than requested. Firmware MUST log the **granted** values and use them for its latency math. | UNVERIFIED per SIM/carrier, but **v1.5.0 gives us a first-class API for it** — no log-scraping needed. Register `setNetworkEventHandler()` (`src/WalterModem.h:5924`); on `WALTER_MODEM_NETWORK_EVENT_EDRX_RECEIVED` (`:1348`) the handler receives `WMNetworkEventData.edrx { actType, requestedEdrx[16], nwProvidedEdrx[16], pagingTimeWindow[16] }` (`:2388-2405`), parsed by the library at `src/WalterModem.cpp:1914-1930`. **`nwProvidedEdrx` is the granted value and is the one §6.5's latency math must use.** The same handler's `cereg` branch also carries granted PSM values (`activeTime`, `periodicTau`, `hasPsmInfo`), which is how firmware confirms PSM really is off. Use `configEDRX(WALTER_MODEM_EDRX_ENABLE_WITH_RESULT, …)` (mode `2`) so the modem emits them. |

### 6.4 How these interact with "one persistent session"

The MQTT session lives **inside the modem** and survives ESP32 deep sleep. Therefore:

1. ESP32 deep sleep MUST NOT power-cycle or reset the modem. Any power-gating of the modem
  would destroy the session and force a ~5 kB TLS handshake on every wake — that alone would be
  ~150 kB/day at 30 wakes/day and would dominate everything else in §7.
2. eDRX is what makes a 30-minute keepalive compatible with 30-second delivery: the network pages
  the modem within one eDRX cycle (≤20.48 s), independent of the keepalive timer.
3. The ESP32 does **not** wake for the keepalive (§6.2). The only periodic ESP32 wake in sleep mode
  is the **5 s wake-and-drain cycle** of §8 — which exists because the library's incoming-message
  state does not survive an ESP32 restart, not because the session needs servicing.

### 6.5 Latency budget check

The ESP32 is woken by its own poll timer, not by the modem (§8).

| Term | Sleep mode (target ~30 s) | Active mode (target < 5 s) |
|---|---|---|
| network → modem: paging + RRC setup | ≤ 20.48 s (eDRX cycle) + 1–2 s, but the modem does this **while the ESP32 sleeps**, so only the eDRX term is serial: **≤ 20.48 s** | C-DRX, already RRC-connected: **≤ 0.5 s** |
| modem → ESP32: URC held off by RTS until the next wake | **≤ T = 5.0 s** | **≤ T = 2.0 s** |
| light-sleep wake + URC flush + event dispatch (~30 ms, §8.0) + `mqttReceive()` round trip | **0.3 s** | 0.3 s |
| JSON parse + partial e-paper refresh | 0.5–1.5 s | 0.5–1.5 s |
| **worst case** | **20.48 + 5.0 + 0.3 + 1.5 = 27.3 s — fits 30 s with 2.7 s margin** | **0.5 + 2.0 + 0.3 + 1.5 = 4.3 s — fits 5 s with 0.7 s margin** |

Both margins are thin, and both are now sensitive to the wake interval `T`, which §8.2 chose for
energy reasons. Two things break this budget, and firmware MUST log enough to detect either:

1. A granted eDRX cycle larger than the requested 20.48 s (§6.3). At the next standard value,
  40.96 s, the sleep-mode worst case becomes 47.8 s and **the 30 s target is unreachable** at any
  wake interval. The response is to lower `T` only if the overshoot is small; otherwise the target
  itself has to move. v1.5.0 hands the granted value to `setNetworkEventHandler()` directly
  (§6.3), so this is a one-line assertion at attach, not a log-scraping exercise.
2. An e-paper refresh slower than 1.5 s. The partial-refresh time must be measured on hardware
  (`firmware/README.md`, M14) and reported back here if it exceeds 1.5 s; at `T` = 5 s there is
  only 2.7 s of slack to spend. `ui.c` uses a
  custom in-tree SSD1680 driver (`ui.c`) doing a mode-2 differential partial refresh, budgeted at
  0.3-0.8 s; the `shown` ack is published only after BUSY deasserts (§4), so this term is serial
  with the latency above and is **measurement M7**.

---

## 7. Data budget

### 7.1 Per-frame overhead assumptions

- MQTT PUBLISH QoS 1 = 2 B fixed header + 2 B topic length + topic + 2 B packet id + payload.
- MQTT PUBACK = 4 B. PINGREQ/PINGRESP = 2 B each.
- TLS 1.2 AES-128-GCM record = **+29 B** (5 header + 8 explicit nonce + 16 tag). TLS 1.3 would be
  +22 B; using the worse number.
- IPv4 + TCP headers = **+40 B** per segment. Bare TCP ACKs counted at 40 B where not piggybacked.

### 7.2 Per-exchange totals

Per-exchange sizes are based on CBOR-signed payloads (§3, item 0–8), which are smaller than
unsigned JSON but carry authentication. Sizes include MQTT frame overhead (§7.1). Figures below the
line are rounded for the month-total calculation in §7.3.

| Exchange | CBOR signed | + TLS | Total | Notes |
|---|---|---|---|---|
| `/down` msg PUBLISH (75 B CBOR, topic 19) + device PUBACK + broker TCP ack | 100 + 4 | +29 | **161 B ≈ 0.16 kB** | Down message delivered (`sent`) |
| `/up` ack PUBLISH (44 B CBOR, topic 17) + PUBACK + TCP ack | 70 + 4 | +29 | **135 B ≈ 0.14 kB** | One ack (`shown` or `read`) |
| **Down msg fully acked** (down + shown + read) | | | **432 B ≈ 0.43 kB** | vs. 0.82 kB unsigned JSON |
| `/up` reply PUBLISH (55 B CBOR) + PUBACK + TCP ack | 80 | +29 | **137 B ≈ 0.14 kB** | Reply sent, vs. 0.29 kB unsigned JSON |
| `/status` PUBLISH (78 B CBOR, topic 21) + PUBACK + TCP ack | 104 | +29 | **161 B ≈ 0.16 kB** | vs. 0.33 kB unsigned JSON |
| `/loc` periodic PUBLISH, QoS 0 (105 B CBOR, topic 18) + TCP ack | 130 | +29 | **187 B ≈ 0.19 kB** | vs. 0.29 kB unsigned JSON |
| `/loc` answering `loc_req`, QoS 1 (105 B CBOR) + PUBACK + TCP ack | 130 + 4 | +29 | **191 B ≈ 0.19 kB** | vs. 0.37 kB unsigned JSON |
| One location request served (`/down` `loc_req` 78 B + device ack + QoS 1 `/loc` answer) | | | **330 B ≈ 0.33 kB** | vs. 0.65 kB unsigned JSON |
| keepalive PINGREQ + PINGRESP (+ TCP ack) | 4 | +29 | **71 B ≈ 0.07 kB** | Modem-side only (§6.2) |
| **TLS reconnect** (TCP + full TLS 1.2 handshake + MQTT CONNECT/CONNACK/SUBSCRIBE/SUBACK) | | | **≈ 5 kB (estimate)** | Session lost, needs re-auth and re-subscribe |

### 7.3 Monthly projection

Signed CBOR payloads (§7.2) are ≈ 45–50% smaller than unsigned JSON, so the daily budget falls
despite adding authentication. Data costs nothing on the battery budget (§8), only on the SIM.

Nominal school day: 20 down messages, 5 student replies, 4 reconnects, and a periodic fix
every 15 min plus 2 on-demand location requests.

```
20 down msgs fully acked 20 x 0.43 kB = 8.6 kB
 5 replies 5 x 0.14 kB = 0.7 kB
48 keepalives 48 x 0.07 kB = 3.4 kB
34 status publishes 34 x 0.16 kB = 5.4 kB (24 heartbeat + ~10 event-driven)
 4 reconnects 4 x 5.00 kB = 20.0 kB
  ---------
  38.1 kB/day -> 1.14 MB / 30 days (signed CBOR)
96 periodic /loc @15 min 96 x 0.19 kB = 18.2 kB
 2 location requests 2 x 0.33 kB = 0.7 kB
  ---------
  57.0 kB/day -> 1.71 MB / 30 days
```

Pessimistic day: 100 down messages, 20 replies, 24 reconnects (bad coverage), and a periodic
fix every 5 min plus 10 on-demand requests.

```
100 x 0.43 + 20 x 0.14 + 48 x 0.07 + 60 x 0.16 + 24 x 5.00
= 43.0 + 2.8 + 3.4 + 9.6 + 120.0 = 178.8 kB/day -> 5.36 MB / 30 days (signed CBOR)
+ 288 x 0.19 + 10 x 0.33 = 54.7 + 3.3 = 236.8 kB/day -> 7.10 MB / 30 days
```

**Verdict.** The 100 MB/month cap allows 3413 kB/day, i.e. ~59× the nominal profile signed. Signed
CBOR payload encoding adds authentication for ≈0.5 % of the nominal budget and ≈3 % of the
pessimistic budget — negligible against the data cap and the project's stricter "< 10 MB" bar.
Both profiles stay well under 10 MB/month: nominal 1.71 MB, pessimistic 7.10 MB. **The 5-minute
periodic-fix interval still raises the pessimistic profile** but no longer jeopardizes the 10 MB
bar — instead, the margin is ≈3 MB. The interval stays device-chosen (§5.1, §13.3).

**The dominant term is reconnects, not messages** — 20 of 38 kB nominal, 120 of 179 kB pessimistic.
This is the quantitative reason for "never reconnect on a timer". **Checked whether `walter-modem`
exposes TLS session resumption: it does not.** There is no session-ticket or session-id parameter
in v1.5.0's public TLS API, so a reconnect is a full ~5 kB handshake every time. This does not
threaten the data constraint but it is the dominant energy cost.

**SMS budget: device-direct SMS is in scope (v0.2, owner decision 2026-09-20, reversing this
document's earlier "no device-side SMS path" rule — see §3.6).** The pager's own modem may send
and receive SMS directly, to a **parent-managed allow-list of phone numbers**, as a delivery path
that does not depend on the relay at all. Every real SMS the modem sends or receives against this
SIM's 100-message/month allowance is one message off that budget — a school day with a handful of
direct texts to a parent's own phone is nowhere near it, but this line exists precisely so nobody
has to guess: **every SMS sent or received by the modem MUST also produce one signed `sms_log`
audit envelope** (§3.6), non-negotiable, so the relay has a complete record of what left/reached
the SIM even though it never carried the message itself. `UNVERIFIED` whether the production SIM
(a Google Fi data-only SIM) carries SMS at all — `smstest` (§1 item 7) exists to find out; if it
does not, the feature fails safe (every send attempt logs `st:"failed"`, §3.6) rather than silently
pretending to work.

*(the pre-existing clarification the server-side SMS backend needs, unchanged by the paragraph
above.)* SMS also exists, separately, as a **server-side delivery backend**: the relay hands a
message to a third-party SMS provider over HTTPS, from the server, to a human's phone. That traffic
never touches this SIM, this modem or this budget, and a message delivered to a user by that backend
is still delivered to the *device* by MQTT exactly as specified above — it is a wholly different
path from the direct-SMS one above, sharing nothing but three letters. `docs/SERVER_PLAN.md` §6.4
covers it; nothing in this section applies to it.

### 7.4 Measurement — validating the model

§7.3 is a theoretical count of JSON bytes plus assumed TLS/TCP framing overhead. There is one
real measurement to sanity-check it. **This does not replace §7.3** — it validates the
application-layer MQTT byte counts that §7.3's per-exchange table (§7.2) is built from; it does not,
and cannot, measure the TLS or LTE-M cellular framing §7.3 assumes for the real device↔broker link,
because there is no TLS and no cellular hop in this setup.

**What was measured.** `relay/docker-compose.yml`'s real `eclipse-mosquitto` broker and the real
relay container (no fake transport, no unit-test mocks), with one simulated device
(`paho-mqtt`, matching `tools/pager_client.py`'s device side) connected over plain TCP on the Docker
bridge network. Traffic sent, matching the "measurable, non-modem" slice of §7.3's nominal profile
over a fixed synthetic load: **20 down messages, each fully acked `shown` then `read`, plus 5
student replies** (48 modem-side keepalives are not reproducible here — there is no modem, and the
relay's own keepalive is a plain 60 s service keepalive per `RELAY_KEEPALIVE_S`
(`relay/app/mqtt_transport.py`), not the device's 1800 s figure — so keepalive/status traffic is
excluded from this measurement, not estimated).

**How.** `docker stats --no-stream --format '{{.NetIO}}' relay-broker-1` before and after the run,
taken once the device's CONNECT/SUBSCRIBE/initial-status handshake had settled (2 s idle), so the
delta is steady-state per-message traffic, not connection setup. The broker container serves no
HTTP, so 100% of its NetIO is MQTT protocol bytes — this sidesteps having to separate the relay's
HTTP (parent API) traffic from its MQTT traffic, at the cost of the measurement covering **both**
hops through the broker (relay↔broker *and* broker↔device), not the single relay↔broker hop §7.2's
table models. A from-scratch theoretical figure was computed in parallel, using §7.1's exact stated
rules (MQTT PUBLISH/PUBACK sizes, +40 B TCP/IP per segment, no TLS) applied to the *actual* payload
bytes sent in the run, so the comparison uses real payload sizes, not a fixed example string.

**Results** (20 down messages fully acked + 5 replies; bodies ~50–100 bytes, longer than §7.2's
example on purpose, to make sure the comparison isn't an artifact of one convenient string length):

| Quantity | Value |
|---|---|
| Theoretical, MQTT bytes only, both hops, no TLS | 14,420 B |
| Theoretical, MQTT + 40 B/segment TCP/IP, both hops, no TLS | 24,820 B |
| **Measured, `docker stats` NetIO delta on the broker container** | **42,870 B** |
| measured ÷ theoretical (MQTT+TCP/IP, both hops) | **1.73×** |

Re-expressed as a single hop (measured ÷ 2, since the two hops carry near-identical payloads —
this is the number comparable to §7.2's per-exchange table, which models one hop):

| Quantity (single hop, no TLS unless noted) | Value |
|---|---|
| Computed, down message fully acked (down + `shown` + `read`), this run's payload sizes | 688 B |
| §7.2's worked example, same exchange, **with TLS**, a shorter example body | 818 B |
| Computed, student reply, this run's payload sizes | 248 B |
| §7.2's worked example, same exchange, **with TLS** | 289 B |
| Theoretical single-hop total for the whole 20+5 run (§7.2's 3-frame-per-exchange method, no TLS) | 15,010 B |
| Measured-implied single-hop total for the whole run (measured ÷ 2) | 21,435 B |
| ratio (measured-implied ÷ theoretical, single hop) | 1.43× |

**Assessment: consistent, not an exact match, and the gap is explained.** The measured bytes are
higher than the theoretical estimate by 1.4–1.7× depending on how the two hops are split, in the
direction expected, for reasons the theoretical model explicitly doesn't try to capture:

1. **Ethernet framing.** `docker stats` counts bytes at the container's virtual NIC, which includes
  the 14 B Ethernet header (+ often an FCS) per frame that §7.1's "+40 B IPv4+TCP" figure doesn't
  include (that number is IP+TCP headers only, per §7.1's own text). Over ~260 frames in the both-hop
  run, that alone is on the order of 3–4 kB.
2. **TCP ACKs are not perfectly piggybacked.** §7.1 already flags this ("bare TCP ACKs counted at
  40 B where not piggybacked") but the theoretical count only adds one bare ACK per exchange; real
  TCP on a lightly-loaded loopback/bridge link sends more standalone ACKs than that model assumes.
3. **Docker's bridge/NAT path adds its own small overhead** (veth pair + bridge + userland-proxy or
  iptables DNAT for the published port) that has no equivalent in either §7.1's model or a real
  cellular link, and is specific to this local measurement setup, not the production topology.
4. **Missing TLS pushes the other way, and is a separate comparison from point (1)–(3) above.**
  A real device↔broker hop adds TLS record overhead (§7.1: +29 B/record) that this plaintext
  measurement has none of. This is visible if the *computed* (not measured) no-TLS single-hop
  figure for a fully-acked down message in this run (688 B, using this run's longer body) is set
  next to §7.2's TLS-inclusive computed figure for its own, shorter example body (818 B): the two
  are close despite one including TLS and the other not, because a longer body and no TLS roughly
  cancel out here. That is a coincidence of this run's message lengths, not evidence that TLS is
  cheap — the actual measured-vs-theoretical gap (point 1–3, 1.4–1.7×) is entirely independent of
  TLS, since neither side of that comparison includes it.
5. **This measurement cannot validate the TLS or cellular assumptions at all**, by construction —
  there is no TLS handshake, no TLS record framing, and no LTE-M/RRC layer in a
  docker-bridge-to-container TCP connection. §7.3's dominant term, the ~5 kB reconnect estimate
  (built on a full TLS 1.2 handshake), and the +29 B/record TLS overhead applied throughout §7.2,
  remain unverified by this measurement and stay in the `UNVERIFIED`/`(estimate)` category per §12
  until measured against the real HiveMQ/EMQX broker over real TLS, ideally from real hardware or
  at least a real `tlsClient`-terminated connection.

**Verdict.** The measurement confirms §7.1/§7.2's core claim — MQTT PUBLISH/PUBACK sizes computed
from JSON payload lengths plus fixed per-field overhead are the right order of magnitude and the
right formula — to within the 1.4–1.7× gap explained above, all of which is either (a) real overhead
the theoretical model intentionally simplified away (Ethernet framing, imperfect ACK piggybacking)
or (b) an artifact of the local Docker bridge that has no cellular equivalent. Since §7.3's monthly
projections (1.69 kB/day nominal, 236 kB/day pessimistic) sit at roughly 3% and 7% of the 100 MB
cap respectively (§7.3's own "~59×" headroom claim), even a 1.7× correction to the message-only
terms — the reconnect term dominates the pessimistic case and this measurement says nothing new
about it — leaves both profiles comfortably under the cap and under the project's stricter 10 MB
bar. **No change to §7.3's verdict is warranted by this measurement**; it is recorded here as
supporting evidence, not a revision.

---

## 8. ESP32 wake sources

Written against `dptechnics/walter-modem` v1.5.0 (read at
`firmware/managed_components/dptechnics__walter-modem/`). The obvious design — have the modem wake
the ESP32 from deep sleep on an incoming MQTT message — is **not supported by the library**, and
working around that is the single largest constraint on this project's power story. RTC memory
contents are defined in §9.

### 8.0 The library constraint that forces the design

Three facts, each checked in the **v1.5.0** source, not assumed:

1. **Incoming messages are announced only by a live URC, and the announcement carries data you
  cannot reconstruct.** The modem emits `+SQNSMQTTONMESSAGE:0,"<topic>",<len>,<qos>,<mid>`; the
  library parses it (`src/WalterModem.cpp:3408-3466`) and pushes a `WalterModemEvent` onto a
  FreeRTOS queue carrying `{topic, msg_length, qos, mid}`. The application handler then calls
  `mqttReceive(topic, mid, buf, len)` to fetch the payload — this is the vendor's own pattern in
  `examples/mqtts/main/mqtts.cpp:383-390`.
2. **None of that state is in RTC memory.** `_sleepPrepare()` (`src/WalterModem.cpp:4068-4095`)
  copies PDP contexts, the CoAP context set, the MQTT **topic/subscription** list, sockets and
  BlueCherry state into RTC — and nothing else. The event queue is an ordinary FreeRTOS queue in
  regular RAM. v1.5.0 removed the old `_mqttRings[]` array entirely, so there is not even a
  buffer left to inspect.
3. **There is no way to ask the modem what it is holding.** `AT+SQNSMQTTRCVMESSAGE` needs the
  `mid` that only the lost URC carried. The one no-`mid` form of the command is the QoS **0** path,
  and it is what the deprecated `mqttDidRing()` hardcodes — `mqttDidRing()` is now nothing but
  `mqttReceive(topic, 0, …)` plus a deprecation warning (`src/proto/WalterMQTT.cpp:174-182`), so it
  is the wrong call for our QoS 1 `/down` topic. **Firmware MUST NOT use `mqttDidRing()`.**

Consequence: **a `/down`
message that arrives while the ESP32 is in deep sleep is announced into a powered-down UART and is
then unreachable through the public API.** Deep sleep does not merely fail to wake us — it loses the
message, and it loses the `mid` needed to ever ask for it again.

The corollary that shapes the loop: **there is nothing to poll.** A "poll cycle" calling
`mqttDidRing()` is both deprecated on v1.5.0 and wrong for QoS 1. The correct shape is a
**wake-and-drain cycle** — the ESP32 wakes, the modem flushes the URC it was
holding while RTS was deasserted, the library's RX task parses it, the event task dispatches it, the
handler calls `mqttReceive()`. The ESP32 issues no speculative AT traffic at all. The energy model in
§8.2 is unaffected: what matters is the length of the awake window and how often it happens, not what
the ESP32 does inside it.

Two v1.5.0 features the design relies on:

- **A real event system, dispatched from a dedicated task.** `setMQTTEventHandler()`
  (`src/WalterModem.h:6011`) delivers `WALTER_MODEM_MQTT_EVENT_MESSAGE` / `_CONNECTED` /
  `_DISCONNECTED` / `_SUBSCRIBED` / `_PUBLISHED` / `_MEMORY_FULL` (`:1368-1375`) from
  `_eventProcessingTask` (`src/WalterModem.cpp:1595-1626`), **not** from the RX ISR/task. Calling
  modem APIs from the handler is supported and is what the vendor example does. Note the task
  polls its queue on a 10 ms tick and adds a further 10 ms settle delay before dispatch, so budget
  **≥30 ms of awake time** for an event to surface at all.
- **`+SQNSMQTTMEMORYFULL` → `WALTER_MODEM_MQTT_EVENT_MEMORY_FULL`** (`src/WalterModem.cpp:3398-3405`)
  tells us the modem's own message buffer overflowed. That is the direct signal that the
  wake-and-drain cycle is falling behind, and firmware should count it (§8.4 measurement M6).

### 8.1 Wake source table

| # | Wake source | Mechanism | Trigger | Mode | Verified? | RTC state that must survive |
|---|---|---|---|---|---|---|
| 1 | **Wake-and-drain timer** (primary) | `esp_sleep_enable_timer_wakeup(T)` + `esp_light_sleep_start()`, `T` = 5 s sleep / 2 s active | Wake, reassert RTS, let the modem flush the URC it held, let the library's RX and event tasks dispatch it to the MQTT handler, which calls `mqttReceive(topic, mid, …)`. **No polling call is made** (§8.0). | both | Mechanism yes; the RTS hold-off behaviour is **UNVERIFIED** (§8.3) | Light sleep retains RAM, so *nothing* has to survive — this is the point |
| 2 | Button IO1 | `esp_sleep_enable_ext0_wakeup(IO1, 0)`, active low, RTC GPIO | Short press = mark read / open composer; long press = send reply | both | Yes — IO1 is an RTC GPIO | `mode`, `active_until`, msg ring, `pending_acks` (deep-sleep path only) |
| 3 | ~~RTC timer — keepalive~~ | — | **Not needed.** The MQTT client is in the modem and the library exposes no ping API; PINGREQ is the modem's job (§6.2). The status heartbeat rides an ordinary poll wake using a counter in RTC memory. | — | n/a | `status_pub_count` |
| 4 | RTC timer — active-mode exit | `esp_timer` while awake, not a sleep wake | 10 min with no button/keyboard activity → sleep mode | active | Yes | `mode`, `active_until` |
| 4b | MQTT event (no sleep involved) | `setMQTTEventHandler()` → `_eventProcessingTask` | `_MESSAGE` short-circuits the wake-and-drain latency while the ESP32 happens to be awake; `_DISCONNECTED` drives F3 recovery; `_MEMORY_FULL` flags a missed drain | both | Yes — v1.5.0 API | none (handler runs while awake) |
| 5 | ~~Modem URC / RI line into deep sleep~~ | `ext1` on the modem RX line | **Not supported by the library's public API; not pursued.** See §8.3 for why, and for the one variant that could still work if someone wants the battery back. | — | n/a | — |
| 6 | LIS3DH INT1 (IO2) | `ext1` | Motion wake | — | **Out of scope.** Reserved only. | — |
| 7 | CardKB | — | **Cannot wake the ESP32.** No interrupt line to an RTC GPIO; polled at 100 ms only while the composer is open. A reply always starts with a button press. | active | Yes (by construction) | — |

**No GPIO reassignment is needed for the modem UART.** The
modem UART is board-fixed inside the component — RX 14, TX 48, RTS 21, CTS 47, RESET 45
(`src/WalterModem.cpp:82,87,92,97,102`, Kconfig-overridable but correct for Walter as shipped). None of those
collide with `firmware/main/pins.h` (1, 2, 8, 9, 10, 11, 12, 15, 16, 17, 18), and `begin()` takes
only a `uart_port_t`. No pin needs to be added to `pins.h` and **no GPIO reassignment is requested**.
For the record, GPIO14 and GPIO21 are both RTC-capable on the ESP32-S3 (RTC GPIOs are 0–21), so no
bodge wire is needed either — the blocker is the library API, not the board.

### 8.2 Why light sleep, and why `T` = 5 s — the arithmetic

(Terminology: `T` is the **wake-and-drain interval**, not a polling interval — see §8.0. The energy
model is indifferent to that distinction; the latency model in §6.5 is not.)

Vendor figures, from the `WalterModem::sleep()` doc comment (`src/WalterModem.h:4269-4271`) — these
are **vendor-documented board-level numbers, not this document's estimates**: light sleep **1 mA**,
deep sleep **9.5 µA**. Assumption attached to both: they describe the board with the modem also
asleep. Our modem stays attached in eDRX, so the modem paging term is added on top of both and
cancels out of the comparison below.

Per-wake energy, ESP32 side, at an assumed **40 mA** active draw (ESP32-S3, radio off):

| | awake time per cycle | why |
|---|---|---|
| Light-sleep cycle | **50 ms** (estimate) | CPU resumes in place. No re-init, and **no AT traffic in the common case** — the ESP32 issues nothing unless a URC actually arrived (§8.0). The floor is set by the library's event task, which ticks at 10 ms and adds a 10 ms settle delay, so ~30 ms is the minimum useful window; 50 ms gives margin for the UART flush. |
| Deep-sleep cycle | **600 ms** (estimate) | Deep sleep restarts program execution. Bootloader + `app_main` ≈ 300 ms, then `WalterModem::begin()` must re-open the UART, spawn its tasks, run `_sleepWakeup()` — which itself issues a `checkComm()` (`src/WalterModem.cpp:4097-4100`) — and then `configCMEErrorReports()` + `configCEREGReports()`, i.e. at least three AT round trips (`src/WalterModem.cpp:4258-4276`). |

ESP32-side average current as a function of the poll interval `T`:

```
I_light(T) = 1.0 + 40 x (0.050 / T) = 1.0 + 2.0 / T mA
I_deep(T) = 0.0095 + 40 x (0.600 / T) = 0.0095 + 24 / T mA

crossover: 1.0 + 2/T = 0.0095 + 24/T -> 0.99 = 22/T -> T = 22 s
```

**Deep sleep only wins above a 22-second wake interval.** §6.5 caps the interval at
30 − 20.48 − 0.3 − 1.5 = **7.7 s**. The two requirements do not overlap, so light sleep wins, and it
is not close:

| `T` | `I_light` | `I_deep` | sleep-mode worst-case latency |
|---|---|---|---|
| 2 s | 2.00 mA | 12.0 mA | 24.3 s |
| **5 s** | **1.40 mA** | **4.81 mA** | **27.3 s** |
| 7 s | 1.29 mA | 3.44 mA | 29.3 s |
| 22 s | 1.09 mA | 1.10 mA | 44.3 s — **misses the 30 s target** |

`T` = **5 s** is the choice: 1.40 mA against 1.29 mA at 7 s — 0.11 mA, about 2.6 mAh/day, roughly
0.4 % of a 1500 mAh cell — buys 2.0 s of latency margin on a budget that has only 2.7 s of it. Going
below 5 s costs real current (2 s costs +0.6 mA, ~14 mAh/day) for latency nobody asked for in sleep
mode. `PAGER_WAKE_INTERVAL_SLEEP_MS` / `PAGER_WAKE_INTERVAL_ACTIVE_MS` are compile-time constants so
this can be retuned against a current trace without touching logic.

### 8.3 Deep sleep: why it is not used, and the one way back

Two variants could make deep sleep safe, and both are blocked on something unverified:

- **(a) Hold RTS deasserted across deep sleep.** RTS is GPIO21, an RTC GPIO, so `rtc_gpio_hold_en()`
  plus `gpio_deep_sleep_hold_en()` can pin it high while the ESP32 is down. If the Sequans honours
  CTS and queues its URCs instead of dropping them, the URC is delivered intact after
  `WalterModem::begin()` releases the hold — which is exactly the trick the library already performs
  for light sleep (`src/WalterModem.cpp:4413-4450`). This fixes *correctness*, but it does not fix
  *energy*: the 600 ms re-init per cycle still applies, so the table above still says deep sleep
  loses at `T` ≤ 7.7 s. **Not worth pursuing at this project's latency target.**
- **(b) `ext1` wake on the modem RX line (GPIO14) + forced redelivery.** Wake on the URC's start
  bit, accept that the URC bytes are lost, then `mqttDisconnect()` + `mqttConnect()` so the broker
  redelivers the unacked QoS 1 message while the ESP32 is awake. Energy would be excellent —
  9.5 µA floor plus roughly 0.107 mAh per message (600 ms boot at 40 mA, plus ~3 s of RRC at
  ~120 mA for the handshake), so about **12–19 mAh/day** against the 38–50 mAh/day of §8.4, a 2.5–4×
  win. Data cost is ~5 kB per message (§7.2), ~100 kB/day at 20 messages — irrelevant against the
  3413 kB/day the SIM allows. **But it is entirely dependent on `cleanSession=false`, which the
  library cannot set (§6.1).** If the Sequans defaults to a clean session there is no redelivery and
  the message is simply lost. Do not implement (b) until the §6.1 clean-session experiment has
  passed. It is the highest-value follow-up in this document.

Note that (a) and (b) are mutually exclusive: holding RTS high stops the modem transmitting, so the
RX line never toggles and the `ext1` wake never fires.

The firmware implements neither. It implements the light-sleep wake-and-drain cycle, which is
correct by construction and needs no GPIO change and no unverified modem behaviour.

**This section is the record of that degradation, rather than a silent one.** It is real — roughly
a 2× cut in idle battery life against what a modem-wake design would have given — and its cause is
a library limitation, not a hardware one.

### 8.4 Sleep-mode current estimate

Vendor-documented where marked; everything else remains `(estimate)` pending a current trace on
real hardware (`firmware/README.md`, M1).

| Term | Value | Assumption |
|---|---|---|
| ESP32-S3 light sleep, board level | **1.00 mA** *(vendor)* | `WalterModem::sleep()` doc comment; RAM retained, UART domain powered |
| Wake-and-drain overhead, `T` = 5 s | 0.40 mA (estimate) | 50 ms awake at 40 mA every 5 s (§8.2) |
| Modem eDRX paging | 0.2–0.5 mA (estimate) | 2 paging occasions per 20.48 s cycle, ~50 ms each at ~50 mA RX, plus warm-up |
| Modem idle floor | 0.01–0.05 mA (estimate) | Sequans GM02SP deep-sleep-between-paging |
| Keepalive, amortised | ~0.20 mA (estimate) | 48 PINGs/day × ~0.1 mAh (§6.2); modem-side only, the ESP32 no longer wakes for it |
| Display (gated off via IO15) | ~0 mA | e-paper VCC gated between refreshes |
| **Sleep-mode total** | **≈ 1.8–2.1 mA → 43–50 mAh/day** | On a ~1500 mAh LiFePO4 cell: **~30–35 days idle** |

> **Why this is worse than it looks on paper.** A design built on an ESP32 deep-sleep floor of
> 0.01–0.10 mA would give 0.6–1.2 mA → 15–29 mAh/day → ~50–100 days. That floor is only reachable
> with a modem-driven deep-sleep wake, which §8.0 shows the library cannot provide, so the honest
> number is roughly **2× worse**. §8.3 (b) is the path back to something near it.

Active mode, 10-minute window: the ESP32 light-sleeps at `T` = 2 s between drains rather than
spinning, so its contribution is `I_light(2)` ≈ **2.0 mA**, not the ~35 mA of a busy-wait. The modem
holding RRC/C-DRX dominates at an estimated 5–15 mA, giving **7–17 mA → 1.2–2.8 mAh per window**.
Six windows/day ≈ **7–17 mAh/day**, so active mode adds roughly 20–40 % on top of sleep mode rather
than the 100–200 % a busy-waiting design would add. Carve-out: while the reply composer is open the
CardKB needs 100 ms polling, so the device stays fully awake for that window — budget for it here.

Firmware MUST count `WALTER_MODEM_MQTT_EVENT_MEMORY_FULL` events and publish the counter in the
`/status` heartbeat if it is ever non-zero: it is the only direct evidence that the wake-and-drain
cycle is losing messages, and it is cheap.

Worth measuring on hardware: whether idle-mode eDRX at 5.12 s meets the <5 s active-mode target. If
it does, it is likely several× cheaper than holding RRC. That is a measurement to take, **not** a
change to the mode architecture.

## 9. Storage contract: RTC memory vs. ordinary RAM

Putting the whole message store in RTC memory does not fit: the vendor library already takes 7003
of the 8192 bytes (§9.1). The store is therefore **split**. RTC memory holds only what must survive
a reset to keep §4's delivery guarantees honest; the bodies and the thread history live in ordinary
static RAM, which the light-sleep cycle (§8) retains.

### 9.1 The measured budget

ESP32-S3 RTC slow memory is **8192 B** (`rtc_slow_seg`, `0x50000000`, length `0x2000`). All figures
below are read from `firmware/build/school_pager.map` after a clean
`idf.py set-target esp32s3 && idf.py build` on `espressif/idf:release-v5.2`:

| Consumer | Bytes | Map symbol |
|---|---|---|
| `walter-modem` v1.5.0 (`_pdpCtxSetRTC`, `_mqttTopicSetRTC`, `_socketCtxSetRTC`, `_coapCtxSetRTC`, `blueCherryRTC`) | **7003** | `.rtc.data.0-.4` under `WalterModem.cpp.obj` (`0xc80 + 0x910 + 0x3c0 + 0x208 + 0x3`) |
| IDF/linker remainder | 5 | `_rtc_slow_length` (`0x1f58`) minus `.rtc.data` (`0x1f54`) |
| **Available to the pager** | **1184** | `0x2000 - 0x1f58 + sizeof(g_rtc)` |

Reclaiming the library's 7003 B is not an option: `CONFIG_WALTER_MODEM_ENABLE_SOCKETS`/`_HTTP`/
`_COAP`/`_BLUECHERRY` each **fail to build the vendor library itself** (unguarded references in
`WalterBlueCherry.cpp` and `_dispatchEvent()`), and this project never patches a managed
component. See `sdkconfig.defaults` for the full finding.

### 9.2 The split, and the rule that decides it

> **Rule.** A datum lives in RTC memory if losing it to a reset would make the relay's view of
> delivery *wrong* — i.e. the relay believes a message was delivered or a reply was sent when the
> student will never see it / never sent it. Everything else lives in RAM or NVS.

Under that rule three things are RTC-resident: the **dedup digest ring** (§4.1 rule 7 — without it a
post-reset redelivery storm costs a full active-mode window per duplicate), the **pending ack queue**
(§4.1 rule 6), and the **pending reply metadata** (§4.2 — id, state, attempts only; bodies are in
NVS). One more is RTC-resident by judgement rather than by rule: the **newest unread down message
metadata**, because §5.3 only re-publishes messages in state `queued`/`sent` — a message already
acked `shown` but not yet `read` is unrecoverable from the relay, so losing it means the student
never sees "pickup at 3:15" and nobody finds out. (The message bodies themselves are stored in NVS
`msgq` namespace and survive both reset and cold boot.)

**Reply and unread bodies now survive resets.** Device-typed replies and the newest unread message
are stored in NVS namespace `msgq` with full fidelity (up to 320 UTF-8 bytes), so an unsent reply
outlives a battery pull and truncation is never silent. (RTC still holds the per-reply metadata:
id, state, attempts.)

### 9.3 RTC-resident layout (`pager_rtc_t`, owned by `modes.c`)

| Field | Bytes | Purpose |
|---|---|---|
| `magic` + `crc32` | 8 | Validity check. On mismatch, treat as cold boot: regenerate `session_id`, clear everything. **Bump the layout digit in `magic` whenever this table changes** — a stale-but-CRC-valid struct read across an incompatible change decodes as garbage. |
| `boot_count` | 4 | Diagnostics; distinguishes cold boot from reset recovery |
| `session_id[12]` | 12 | §1; regenerated **only** on cold boot |
| `mode`, `active_until_epoch` | 16 | Mode state machine |
| `status_pub_count`, `last_status_epoch` | 16 | §5.4 heartbeat cadence |
| `mqtt_memfull_count`, `oversize_drop_count`, `modem_resets`, `last_modem_reset_us`, `attach_fail_cycles`, `wake_cycle_count` | 32 | §8.4 M6, §3.4, F4/F1 counters |
| `msg.seen_ids[16]` (`uint32_t`) + `seen_head` | 68 | Dedup ring (§4.1 rule 7) — **a 32-bit digest of the id, not the id string.** Message ids carry 32 bits of entropy by construction (§1), so the digest is the id's own randomness; 16 entries collide with probability ≈3×10⁻⁸, and the only consequence of a collision is one suppressed render. Costs 68 B where the literal strings cost 276 B. `id_hash == 0` means "empty slot"; a real digest of 0 is stored as 1. |
| `msg.pending_acks[8]` (`id[17]`, `state`, `attempts`) | 152 | Ack retry queue (§4.1 rule 6). Ids are stored in full — the ack has to put the real id on the wire. |
| `msg.pending_up[2]` (`created_epoch`, `id[17]`, `id_len`, `to[17]`, `to_len`, `attempts`, `in_use`) | 80 | Unsent student replies (§4.2) — metadata only; bodies are in NVS `msgq` (§9.2) |
| `msg.unread[1]` (`ts`, `id[17]`, `from[17]`, `to[17]`, `flags`, `in_use`) | 60 | Newest down message acked `shown` but not yet `read` (§9.2) — metadata only; body in NVS |
| `msg.dedup_hits`, `msg.malformed_drops`, `msg.reply_failed` | 12 | Diagnostics for the `/status` heartbeat |
| `auth.up_lo` | 4 | Low 20 bits of `/up` counter (`n`), per-device replay window (§2.5) |
| `auth.down_n` | 4 | Highest accepted `n` for `/down` messages (§2.5) |
| `auth.down_bits` | 4 | 64-bit bitmap of recent `/down` counter values for the replay window (§2.5) |
| `ui_state` | 4 | UI flags and settings (e.g., last screen) |
| `lock` (`locked`, `fail_count`, `backoff_until_us`, padding) | 16 | Device lock state (§5.8); `locked` flag, wrong-passcode attempt counter, backoff expiry, padding |
| **Total** | **≈ 460 of 1184** | ~724 B headroom |

`modes.c` remains the sole owner of the struct, its single `magic`/`crc32` pair and `rtc_save()`
(§11's "one transition funnel" discipline applies to RTC writes too). `msg.c` receives a typed
pointer to the nested `msg` sub-struct and calls back into `modes.c` to re-CRC. `modes_boot()` MUST
log `sizeof(pager_rtc_t)` at boot and MUST `_Static_assert(sizeof(pager_rtc_t) <= 1184)`.

### 9.4 Reply and unread bodies are now full fidelity

§3.1 caps a body at 160 Unicode code points **and** 320 UTF-8 bytes.

**Reply bodies (§4.2, §5.6).** Once an IME is added (§5.2), a composed reply can be up to 320 UTF-8
bytes of arbitrary Unicode. Instead of storing bodies in RTC (which has no headroom for 320 bytes),
replies are now stored in NVS namespace `msgq` with full fidelity. The composer MUST **refuse input
past 320 UTF-8 bytes** (160 code points, whichever binds first) rather than truncate; a truncated
reply would be a silent correctness failure. The NVS write happens on submit; the entry is erased
on PUBACK.

**Unread down messages.** The newest unread down message is stored in NVS `msgq` in full (up to 320
bytes). In normal operation the UI renders from the RAM copy; on reset, the device restores from NVS
with full fidelity, never truncated. (Older unread messages beyond the newest are lost to RAM; §9.6
says so explicitly.)

### 9.5 RAM and NVS store

**RAM-resident, ordinary `.bss`:**

| Field | Bytes | Purpose |
|---|---|---|
| `s_thread[32]` — `msg_t` = `ts`, `id[17]`, `from[17]`, `to[17]`, `body[321]`, `body_len`, `dir`, `ack_state`, `flags`, `in_use` | 13312 | Thread history, both directions, full 320-byte bodies. **32 entries** for richer history display; §5.3's 10-per-online-edge re-publish cap is about message *state*, not ring depth. |
| `s_composer[321]` + cursor/length/to | 328 | In-progress reply text, UTF-8 with IME (§5.2, §9.4); `to` field to track recipient |
| `ui.c` frame buffers: new plane + shadow (old) plane, 16 B/row × 296 rows each | 9472 | SSD1680 differential partial refresh needs both planes |

≈ 23.1 kB of the ESP32-S3's ~512 kB SRAM. RAM is not the scarce resource.

**NVS-resident, namespace `msgq` (survives reset and cold boot):**

| Field | Bytes | Purpose |
|---|---|---|
| Reply body (each entry) | ≤ 320 + metadata | Unsent reply body, stored on submit, erased on PUBACK (§9.4, §4.2) |
| Unread message body | ≤ 320 + metadata | Newest unread down message, full fidelity (§9.4) |

### 9.6 What survives what

| Event | RTC struct | NVS (`msgq`, `book`, `lock`, `ident`) | `.bss` (thread, composer, frame buffers) | Modem TLS+MQTT session |
|---|---|---|---|---|
| Light-sleep wake (every 2 s / 5 s — everyday case, §8) | survives | survives | **survives** | survives |
| `esp_restart()`, watchdog reset, panic/crash | survives | **survives** | **lost** | survives (modem never power-gated, §6.4) |
| Brownout, EN reset, battery removal, first power-on | **lost** (CRC fails → cold boot) | **survives** | lost | lost |

Concretely, after a **crash or watchdog reset**: pending acks and pending replies are retried
normally, dedup still suppresses redeliveries, and the newest unread message is re-rendered from
`msg.unread[0]` with a truncation ellipsis if the parent used non-ASCII. What is lost is the
**scrollback** — the thread view drops from up to 10 messages to 1, and `ui.c` MUST say so rather
than pretend (render an explicit "earlier messages lost (restart)" line). Older *unread* messages
beyond the newest one are also lost; this is the one accepted gap and it is bounded by how many
messages arrive unread inside one crash window.

After a **cold boot** the RTC struct is invalid, so `session_id` is regenerated, which §5.3 turns
into a session change at the relay and therefore a re-publish of everything still `queued`/`sent`.
Dedup is empty at that point, so those messages render normally. Cold boot is the *better*-recovered
case of the two.

> **Optional follow-up, not adopted (relay-side change, needs an owner).** Widening
> §5.3's re-publish selection from `state in (queued, sent)` to
> `state in (queued, sent, shown) AND age < 2 h` on a **session change only** would let the relay
> recover *all* unread messages after a cold boot, not just the ones that never reached `shown`, and
> would make `msg.unread[]` redundant rather than merely shallow. It costs nothing in the common
> case (dedup suppresses re-render when RTC survived; a session change only happens on cold boot).
> Not done, because it changes relay code and §5.3's contract. Raised in §12.

### 9.7 Standing rules

- Firmware MUST NOT assume the modem's MQTT session state is mirrored in RTC memory. On every wake
  it re-reads the modem's actual connection state and drains the URC the modem was holding (§8.0).
- **Do not shrink `seen_ids`** below 16 entries — see the duplicate-storm cost in §4.1 rule 7.
- Keeping this layout in RTC rather than moving all of it to RAM still buys the two things §8.3(b)
  needs: if the clean-session experiment (§12 item 4) passes and deep sleep returns, the durability
  half of the store already has the right shape and the change is a sleep-call swap, not a data-model
  rewrite.
---

## 10. CBOR keymap (normative)

Devices emit CBOR (§3) with this integer keymap. The relay accepts both JSON (text keys) and CBOR
(integer keys) on every inbound topic and answers in the encoding of its last `/status`.

**Envelope keys** (message, ack, location, /status, bootstrap):

| Key | Name | Type | Where |
|---|---|---|---|
| 0 | `v` | int | all envelopes (default 1) |
| 1 | `id` | tstr | all envelopes |
| 2 | `ts` | int | all envelopes (0 if no network time) |
| 3 | `from` | tstr | `/down` msg, ack, location answer |
| 4 | `body` | tstr | `/down` msg, `/up` msg, bootstrap ok (if present) |
| 5 | `ack` | tstr | all envelopes (`shown`, `read`, or `null`) |
| 6 | `kind` | tstr | `/down` (msg/loc_req/book/cfg), `/up` (msg/contact_req), bootstrap ok |
| 7 | `to` | tstr | `/up` content messages |
| 8 | `loc` | map | `/loc` envelope (§13.2) |
| 9 | `req` | tstr | `/loc` envelope (§13.2) |
| 10 | `cached` | bool | `/loc` envelope (§13.2) |
| 11 | `err` | tstr | `/loc` envelope when `loc` is null |
| 12 | `n` | uint | signed envelopes (replay counter) |
| 13 | `sig` | bstr(8) | signed envelopes (HMAC tag); **MUST be last** |
| 14 | `bv` | int | `/status` and `/down` `book` |
| 15 | `name` | tstr | `/up` `contact_req`, `/down` `book` contacts |
| 16 | `ph` | tstr | `/up` `contact_req` |
| 17 | `d` | tstr | `/down` `book` (default recipient) |
| 18 | `c` | array | `/down` `book` (contacts) |
| 19 | `p` | array | `/down` `book` (pending requests) |
| 20 | `more` | bool | `/down` `book` (reserved for chunking) |
| 21 | `state` | tstr | `/status` (online/offline) |
| 22 | `mode` | tstr | `/status` (sleep/active) |
| 23 | `batt_mv` | int | `/status` (battery millivolts) |
| 24 | `rssi` | int | `/status` (signal strength in dBm) |
| 25 | `session` | tstr | `/status` (session id) |
| 26 | `fw` | tstr | `/status` (firmware version) |
| 27 | `loc_period_s` | int | `/status` (periodic location interval) |
| 28 | `loc_min_s` | int | `/status` (on-demand location rate limit) |
| 29 | `ok` | int | bootstrap `/up` |
| 30 | `pw` | tstr | bootstrap bundle (MQTT password) |
| 31 | `k` | bstr(32) | bootstrap bundle (device HMAC key) |
| 32 | `host` | tstr | bootstrap bundle (broker hostname) |
| 33 | `port` | int | bootstrap bundle (broker port) |
| 34 | `ca` | tstr | bootstrap bundle (CA PEM) — **v0.1-era, legacy.** A relay MUST still decode this key if it appears (an old retained bundle, or a bundle from before a device's firmware/relay pair upgraded), but a v0.2 relay MUST NOT emit it: `ca_url`+`ca_sha` (40/41 below) replace it (§4.4). |
| 35 | `flags` | int | bootstrap bundle (device flags) |
| 36 | `label` | tstr | bootstrap bundle (device label) |
| 37 | `apn` | tstr | bootstrap bundle (carrier APN, optional) |
| 38 | `cfg` | map | `/down` `cfg` (device settings) |
| 39 | `tls` | tstr | `/status` (v0.2, CA trust state — §5.1) |
| 40 | `ca_url` | tstr | bootstrap bundle (v0.2, CA pointer URL — §4.4) |
| 41 | `ca_sha` | bstr(32) | bootstrap bundle (v0.2, CA pointer SHA-256 — §4.4) |
| 42 | `ca_fp` | tstr(16) | `/status` (v0.2, CA fingerprint — §5.1) |
| 43 | `loc_backoff_s` | int | `/status` (v0.2, location backoff — §5.1, §13.3) |
| 44 | `peer` | tstr | `/up` `sms_log` (v0.2, §3.6 — the other party's E.164 phone number) |
| 45 | `dir` | tstr | `/up` `sms_log` (v0.2, §3.6 — `out`/`in`) |
| 46 | `st` | tstr | `/up` `sms_log` (v0.2, §3.6 — `sent`/`failed`/`recv`/`blocked`) |
| 47 | `sms_ts` | int | `/up` `sms_log` (v0.2, §3.6 — when the SMS itself was sent/received) |
| 48 | `sms_lost` | int | `/status` (v0.2, §3.6 — device SMS audit-drop counter) |
| 49 | `cell` | map | `/loc` envelope, optional (§13.2 — cell-tower location fallback) |

**Sub-map keys:**

`loc` object (inside `/loc` envelope): `lat=0, lon=1, acc=2, fix_ts=3, src=4`.

`cell` object (inside `/loc` envelope, key 49 above, §13.2 — cell-tower location fallback):
`mcc=0` (tstr, 3 digits), `mnc=1` (tstr, 2 or 3 digits), `tac=2` (uint, 0…65535), `ci=3` (uint,
0…268435455), `rsrp=4` (int, dBm, optional). Unknown sub-keys are ignored, per §3.1's usual
forward-compatibility rule.

`c[]` contact object (inside `/down` `book`): `a=0` (alias), `n=1` (name), `t=2` (type web/sms/chat).

`p[]` pending request object (inside `/down` `book`): `n=0` (name), `s=1` (status pend/no).

`cfg` object (inside `/down` `cfg`, key 38 above): `lock=0` (map, existing — see below), `ca=1`
(map, v0.2 — §4.4: `{url=0 tstr, sha=1 bstr(32)}`; `sha` absent on an un-pin push, `url=""`),
`sms=2` (array, v0.2 — §3.6: the device's whole SMS contact allow-list, `[{n=0 tstr, p=1 tstr}, …]`).

`lock` map (inside `/down` `cfg.lock`): `clear=0` (bool), `auto=1` (int minutes).

`ca` map (inside `/down` `cfg.ca`, v0.2, §4.4): `url=0` (tstr; `""` means un-pin), `sha=1`
(bstr(32), the CA PEM's SHA-256 — same digest as the bootstrap bundle's `ca_sha`; absent when
`url` is `""`).

`cfg.sms[]` item (v0.2, §3.6): `n=0` (tstr, contact display name, ≤16 code points and ≤24 UTF-8
bytes — tighter than `book`/`contact_req`'s 48-byte name cap; see §3.6 for why), `p=1` (tstr,
E.164 phone number). Note this is its own small namespace, distinct from `c[]`'s `{a, n, t}` above,
even though both happen to use `n` for a display name.

**JSON note (§3, §14.3):** the CBOR sub-map keys above are integers; in JSON the same *names* are
used (`{"lock":{...}}`, `{"ca":{"url":...,"sha":...}}`), and, exactly like `sig`, a `bstr`-typed
field (`ca_sha`/`cfg.ca.sha`) is base64url text without padding in JSON, raw bytes in CBOR.

---

## 11. Future extension points (do not design now)

These are reservations only. Code MUST NOT implement, subscribe to, or emit any of them.

**Topic namespace**
| Reserved topic | Intended use | Notes |
|---|---|---|
| ~~`pager/{id}/loc`~~ | ~~GNSS fixes, device → relay~~ | **Spent — this is now a live topic, specified in §13.** QoS ended up 1-when-answering / 0-when-periodic rather than "QoS 0 likely". |
| `pager/{id}/cfg` | relay → device config | **Remain reserved but unused.** Device settings arrive as `/down` `cfg` kind messages (§5.8) on the normal `/down` topic. A dedicated `/cfg` topic would require a second subscription and re-publish messages 4–24 times per day for data that changes a few times per month (§5.3 rationale, §4.3). |
| `pager/{id}/evt` | geofence enter/exit, motion, low-battery alerts | Separate from `/up` so the parent thread stays human messages only |

**Schema slots** — reserved field names, not to be reused for anything else:
~~`loc` (lat/lon/acc)~~ — **spent, see §13**; ~~`book`~~ — **spent as a `/down` kind (§4.3)**;
along with `kind`, `to`, `req`, `cached`, `err`, `loc_period_s`, `loc_min_s`, `bv`, `n`, `sig`,
which are now defined fields and not reservations — `prio` (priority / alert level), `exp` (message
TTL, would supersede §4's fixed 24 h `expired` sweep), `sched` (mode schedule id).

**Enum headroom** — `from` already allows `system`, which is where geofence and low-battery
notifications will render in the thread without a schema change; the rest of `from` is now
an alias (§3.1), which is an enum removal rather than an addition, but `system` is preserved
verbatim precisely so this reservation still holds. `kind` is likewise a string enum with exactly
two defined values, so `/evt`-style kinds can be added to `/down` later under §3.4's
unknown-`kind`-is-dropped rule. `mode` is a string, not a boolean,
specifically so `school` / `travel` / `night` can be added later; every consumer MUST treat an
unknown `mode` value as `sleep` for display purposes rather than erroring.

**Mode state machine stub** — there are exactly two modes and three transitions: boot
→ sleep; sleep → active on incoming message or button; active → sleep after 10 min idle. Later
schedule- or geofence-driven switching enters at the same `set_mode()` edge, driven by `/cfg` and
`/evt`, so `modes.c` MUST funnel every transition through one function with a reason code rather
than setting the mode variable in-line. That is the only structural requirement today's code owes
the future.

---

## 12. Open questions for the human

1. **~~`NEEDS HUMAN DECISION` — modem wake line GPIO.~~ CLOSED — no decision needed.**
  The question was whether the modem's ring-indicator / UART RX line is routed to an RTC-capable
  GPIO, and whether enabling deep-sleep wake needs a bodge wire. It does not: the modem UART is
  board-fixed inside the component at RX 14 / TX 48 / RTS 21 / CTS 47 / RESET 45
  (`src/WalterModem.cpp:82,87,92,97,102`), GPIO14 and GPIO21 are both RTC-capable on the ESP32-S3,
  and nothing collides with `pins.h`. **No GPIO change is requested and none is needed.**
  What replaced it is not a hardware question but a library one, and it is not a decision for the
  human either — it is an experiment (item 4 below). See §8.0: the library discards
  incoming-message state across an ESP32 restart, so deep sleep loses messages regardless of how we
  wake. The documented consequence is §8.4's estimate: **≈1.8–2.1 mA / 43–50 mAh/day, about
  2× worse than the 0.6–1.2 mA a modem-wake design would give**, i.e. ~30–35 days of idle life
  instead of ~50–100.

2. **`NEEDS HUMAN DECISION` — broker free-tier limits.** This contract needs,
  per device: QoS 1 both directions, a retained `/status`, an LWT, `cleanSession=false` with a
  persistent session that outlives a coverage gap, and a 1800 s keepalive. If the chosen free tier
  caps session expiry below a useful window, caps keepalive below 1800 s, or limits
  credentials/connections such that a second device needs a paid plan, that is a paid-service
  decision. There is a wrinkle: two of those five properties (LWT, clean session) may be
  unreachable from the device side regardless of what the broker allows — see item 3.

3. **`NEEDS HUMAN DECISION` — LWT and clean session are not settable from `walter-modem` v1.5.0.**
  §5.2's LWT contract and §6.1's `cleanSession=false` both assumed an API the library does not
  expose (`mqttConfig()` stops after the TLS profile id; `mqttConnect()` has no session flag; a grep
  of `src/` for `will`/`lastwill`/`clean_session` returns nothing). The options, cheapest first:
  **(a)** send a raw `AT+SQNSMQTTCFG` with the will parameters via the public `sendCmd()` before
  connecting — free if the Sequans AT manual supports it, needs the manual to confirm;
  **(b)** upgrade the component if a future release exposes them; **(c)** drop the LWT and have the
  relay infer offline from keepalive expiry, which costs up to 1800 s of staleness in the parent UI;
  **(d)** patch the managed component, which this project's dependency rule argues against.
  Someone should pick between (a)-(c). Default if nobody picks: (a) attempted, (c) as the
  fallback, and §5.2 is then advisory rather than binding.

4. **Not a decision, but the highest-value experiment in this document.** Settle whether the modem's
  MQTT session is persistent across an ESP32 restart (§6.1 clean session). If it is, §8.3 (b)
  becomes implementable and idle battery life goes from ~30–35 days back to roughly 80–120 days.
  ~20 minutes on hardware. Nobody should redesign anything for it until it has been run.

5. **Device provisioning is now owned by `DEVICE_PLAN.md` §3 (phase 0/1/2b/3).** One-time setup
  over the SIM using a typed code and a token-encrypted bootstrap bundle (§3.2 of this plan). The
  device-side mechanism is `tlsWriteCredential()` into modem NVRAM certificate slot ≥ 11; device
  identity, MQTT password, and HMAC key are written to NVS namespace `ident` once per setup; the
  relay stores the per-device HMAC key in server-only `deviceSecrets` collection. Flash encryption
  is **deliberately off** (no NVS encryption; revoke + new setup code is the threat response for a
  stolen device).

6. **Resolved — no dedicated battery-voltage sense pin, but option (b) exists and is wired in.**
  §5.1's `/status` schema requires `batt_mv`, and `firmware/README.md`'s hardware table does not
  list an ADC-capable GPIO wired to the battery for that purpose — option (a), a dedicated sense pin
  + resistor divider, would need a GPIO change and a human decision, and is **not** pursued.

  Instead: `walter-modem` v1.5.0 exposes `configVoltageMonitor()` / `getVoltage()`, mapping to the
  Sequans-specific `AT+SQNVMON` command, which reports the GM02SP modem's own supply-rail voltage
  in tenths of a volt (confirmed directly in `src/WalterModem.h`/`.cpp` — not merely assumed from
  documentation). Whether that rail tracks the *battery* rather than a fixed regulated rail was
  checked against Walter's public hardware schematics
  (`https://github.com/QuickSpot/walter-hardware`), not just guessed:
  - the GM02SP's power pins sit on a net explicitly named `VBAT`, distinct from Walter's own
  `+3V3`/`+1V8` regulated rails, and that net is not sourced anywhere inside Walter's onboard
  Power Management block (which only produces `+3V3`/`+1V8`);
  - Walter's onboard regulator (`TPS6208833`) is a **buck-boost**, which only makes sense if `VIN`
  is expected to sometimes sit *below* 3.3V — i.e. a single Li-ion/LiFePO4 cell directly, not
  only a fixed 5V USB input;
  - DPTechnics' own official reference battery design in the same repo (`walter-feels`) wires its
  battery charger IC's `VBAT` output — the real, live cell voltage — **directly into Walter's
  `VIN` pin**, with no additional regulation in between.

  Taken together this is strong (not certain — Walter's own internal PCB routing from `VIN` to the
  GM02SP's `VBAT` pins is not published, only inferred by elimination) evidence that `getVoltage()`
  tracks real battery voltage on a battery-powered Walter build, not a fixed 3.3V rail. `net.cpp`
  now calls it and reports the result as `batt_mv`; §12's unverified-assumptions table below
  carries the one open item this rests on. **`UNVERIFIED`, cheap to confirm**: on first hardware
  bring-up, compare `getVoltage()`'s reported value against a multimeter reading of the actual
  battery — 5 minutes, no code change either way.

7. **`NEEDS HUMAN DECISION` — should the relay re-publish `shown`-but-unread messages on a session
  change?** See §9.6's note. Today a down message that reached `shown` and then died in a
  crash is unrecoverable: §5.3 only re-publishes `queued`/`sent`, and the device only keeps the
  **newest** unread message in RTC memory (`msg.unread[1]`, 208 B — a second slot does not fit in
  the 1184 B §9.1 measures). Widening §5.3's selection to
  `state in (queued, sent, shown) AND age < 2 h`, **on a session change only**, closes the gap for
  free: dedup suppresses re-render whenever RTC survived, and a session change only happens on a
  cold boot, when dedup is empty anyway. It is a relay change plus a §5.3 edit, and it has not been
  made unilaterally. If nobody picks this up, the documented behaviour stands: **a
  crash can lose all but the most recent unread message.**

8. **`UNVERIFIED`, and it must be measured before the first periodic interval is chosen — GNSS
  power (§13 deliberately leaves the interval to the device, so this measurement is
  the thing that actually sets it).** §13 gives the device a periodic fix and an on-demand fix, and
  §7.3 shows the *data* cost is small (0.29 kB per fix). **The real cost is energy, and it is not
  estimated anywhere in this document.** A cold GNSS fix on the GM02SP can run for tens of seconds
  at tens of mA; at §8.4's sleep-mode budget of 43–50 mAh/day, even one 30 s fix at 30 mA is
  0.25 mAh, so a 15-minute interval would be ≈ 24 mAh/day — **roughly a 50 % increase in idle
  drain, taking ~30–35 days of idle life to ~20–23**. That arithmetic uses two made-up numbers and
  is offered only to show the term is not negligible. **Nobody should pick `loc_period_s` until
  fix time and fix current have been measured on hardware** (assisted vs cold, indoors vs
  outdoors, and how much of it warm-start assistance data removes); the interval should then be
  derived from that measurement and the battery budget, not assumed. Cheapest experiment: the same
  current trace §8.4 already needs, with a GNSS fix triggered inside the window; record fix time,
  mean current and the resulting mAh per fix in `firmware/README.md` and bring the number back
  here. This is **why** the interval is device-side and not a server setting (§5.1): the server
  cannot see the cost it would be spending.

> **Retracted claim, kept as a warning.** An earlier revision of this section carried a fifth
> `NEEDS HUMAN DECISION` claiming `tlsWriteCredential()` was private and that application code
> therefore could not provision a CA, forcing a choice between unauthenticated TLS and a separate
> provisioning firmware. **That was wrong** — it was read against v1.2.0, where the symbol was
> briefly private. In v1.5.0 it is public (`src/WalterModem.h:4147`, inside the `public:` block that
> starts at `:4132`) and is demonstrated in the vendor's own `examples/mqtts`. There is no decision
> to make: the TLS provisioning flow works as specified. The episode is recorded
> because it is the second time a conclusion in this document turned on a component version — pin
> the version in `firmware/main/idf_component.yml` and re-read the source after any upgrade.

### Unverified assumptions tracked in this document

| § | Assumption | Status | Cheapest experiment |
|---|---|---|---|
| 6.1 | `walter-modem` MQTT is 3.1.1, not 5 | **RESOLVED** — 3.1.1; no version/property API exists | — |
| 6.1 | `walter-modem` exposes TLS session resumption | **RESOLVED — it does not.** Costs the ~40 % saving on the pessimistic data budget (§7.3) | — |
| 6.1 | A CA can be provisioned from application code | **RESOLVED — yes**, `tlsWriteCredential()` is public in v1.5.0; cert slot ≥ 11, TLS profile ≥ 2 | — |
| 6.1 | The modem's MQTT session is `cleanSession=false` | **OPEN, load-bearing.** Not settable, so it is whatever the Sequans defaults to — and `mqttConnect()` clearing the subscription table hints at "clean" | §12 item 4, ~20 min on hardware |
| 6.1 | An LWT can be registered | **OPEN** — no typed API | Raw `AT+SQNSMQTTCFG` with will parameters via `sendCmd()`; check the Sequans AT manual first, 30 min, no hardware |
| 6.2 | Modem issues PINGREQ autonomously | **RESOLVED by construction** — there is no ping API, so it must; the ESP32 keepalive wake is deleted | — |
| 6.2 | 1800 s keepalive is not silently clamped by modem or broker | OPEN | Read the granted value from the broker's connection view, or idle past 1800 s and see if the session survives |
| 6.2 | Carrier NAT tolerates a 1800 s idle TCP flow | OPEN | Overnight idle run with keepalive off, bisect the timeout |
| 6.3 | eDRX API takes seconds vs. the raw 3GPP nibble | **RESOLVED** — raw 4-bit binary nibble strings, spliced verbatim into `AT+SQNEDRX` | — |
| 6.3 | The granted eDRX/PTW values are readable | **RESOLVED — yes**, `setNetworkEventHandler()` → `WALTER_MODEM_NETWORK_EVENT_EDRX_RECEIVED` → `data.edrx.nwProvidedEdrx` | — |
| 6.3 | Carrier grants the requested 20.48 s / 2.56 s | OPEN, and §6.5 breaks if it grants 40.96 s | Assert on `nwProvidedEdrx` at attach — free, first bring-up |
| 8 | Modem URC can wake the ESP32 from deep sleep with the message still retrievable | **RESOLVED — it cannot**, via the public API. §8.0. This is the correction that drives §8.4 | — |
| 8.3 | Sequans queues URCs rather than dropping them when CTS is deasserted | **OPEN, and the whole sleep design rests on it** | Awake ESP32, drive RTS high manually, send a message, wait 30 s, drop RTS, see whether the URC arrives |
| 8.4 | All current figures except the two vendor ones | OPEN | Current-trace measurement; record in `firmware/README.md` |
| 6.5 | SSD1680 partial refresh completes in <1.5 s at room temperature | **OPEN (M7)** — §6.5 breaks if it does not; only 2.7 s of slack at `T`=5 s | Toggle a GPIO around `ui_refresh()`, scope the pulse width; also log `esp_timer` deltas around the BUSY wait |
| 9.4 | The M5Stack CardKB emits one ASCII byte per keypress (no multi-byte sequences) | **OPEN, load-bearing for §9.4** — if it can emit >0x7F, a 160-byte RTC reply slot is no longer 160 characters | Poll 0x5F over I2C, dump every non-zero byte for a full pass over the keyboard incl. Fn/sym combos; 15 min on hardware |
| 8.4 | ESP32 draws ~40 mA awake and ~50 ms per wake-and-drain cycle | OPEN — the overhead term (0.40 mA of 1.8–2.1 mA) rests entirely on this, and the 50 ms floor is set by the library event task's 10 ms tick + 10 ms settle | Toggle a GPIO around the awake window and read the duty cycle on a scope; a current trace gives both numbers at once |
| 12.6 | `getVoltage()`/`AT+SQNVMON` reports real battery voltage, not a fixed regulated rail | **OPEN, cheap to confirm** — inferred from Walter's public schematics (§12 item 6), not from Walter's own unpublished internal routing | Compare `getVoltage()`'s reported `batt_mv` against a multimeter reading of the actual battery on first hardware bring-up; 5 min, no code change either way |
| 13 | Energy per GNSS fix (time-to-fix and mean current) — **no figure exists**, and §13's periodic interval cannot be chosen without one | **OPEN (§12 item 8), load-bearing for the battery budget** | Trigger a fix inside §8.4's current trace; log time-to-fix cold and warm, mean current, mAh per fix |
| 13 | `walter-modem` v1.5.0 exposes a usable GNSS fix API with a bounded timeout and assistance data | **OPEN** — assumed by §13, not yet read against the component source the way §6/§8 were | Grep `src/` for the GNSS/GPS API and read it, the same way §6/§8 were settled for MQTT; 30 min, no hardware |

---

## 13. Location

Spends the `pager/{id}/loc` topic and the `loc` schema slot that §11 reserved.
Everything in this section is additive: a device that never publishes `/loc` and never parses
`kind` (§3.2) is still conformant, it simply has no location feature.

*(location is a separate topic rather than a field on `/up`, because a fix is not a
thread entry: it has a different lifetime, a different QoS, a different retention class and a
different read permission from a human message, and folding it into `/up` would put all four of
those decisions inside one parser.)*

### 13.1 Topic

`pager/{device_id}/loc`, **device → relay**, retained **false**.
QoS **1** when `req` is non-null, QoS **0** otherwise (§2's table carries the reason).
Broker ACL: the device credential may **publish** this topic and nothing more; it MUST NOT be
subscribable by the device. The relay receives it the same way it receives `/up` and `/status` —
a broker rule forwards it to the relay's authenticated HTTPS endpoint (§2).

### 13.2 Payload

```json
{"v":1,"id":"l_3c9a11f0","ts":1757700000,"loc":{"lat":37.774929,"lon":-122.419416,"acc":14,"fix_ts":1757699991,"src":"gnss"},"req":"m_7f3a2b10","cached":false}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `v` | int | no (default `1`) | Schema version, as §3.1 |
| `id` | string | **yes** | `l_` + 8 hex per §1's id rules; the dedup key for this envelope |
| `ts` | int | **yes** | When the envelope was published; §3.5's clock rule applies |
| `loc` | object \| null | **yes** | `null` **only** when `err` is set and no fix was obtainable |
| `loc.lat`, `loc.lon` | number, 6 decimal places | **yes** | WGS-84 degrees. 6 dp ≈ 0.11 m, far below any achievable accuracy, and it bounds the field length |
| `loc.acc` | int, metres | no | Horizontal accuracy estimate. Absent = unknown, **not** zero |
| `loc.fix_ts` | int, epoch s | **yes** | When the fix was *taken*. Differs from `ts` whenever `cached` is true; this is the field a UI ages, not `ts` |
| `loc.src` | `gnss` \| `cell` | no (default `gnss`) | How the position was obtained |
| `req` | string \| null | **yes** | The `id` of the `loc_req` (§3.2) this answers; `null` = an unsolicited periodic fix |
| `cached` | bool | no (default `false`) | True when the device's rate limit (§13.3) answered from the last fix instead of powering GNSS |
| `err` | `no_fix` \| `disabled` | only when `loc` is `null` | `no_fix` = the fix attempt timed out; `disabled` = location is off on the device (`loc_period_s` 0 and the user has disabled on-demand fixes) |
| `cell` | object, optional | no | The device's serving cell (§13.2's own sub-table below). Absent = today's behaviour exactly. |

**`cell` (cell-tower location fallback).** GNSS and LTE cannot run at once on this modem, and a
school pager is indoors most of the day, so a GNSS attempt usually ends in `no_fix`. The pager
always knows its serving cell; it cannot turn that into coordinates, but the relay can (a
pluggable third-party lookup, `relay/app/cellgeo.py`). The device sends `cell` whenever it answers
without a GNSS fix (`loc:null, err:"no_fix", cell:{...}`); it MAY also send it alongside a real
GNSS fix, in which case the GNSS fix wins and the cell is only recorded (on
`devices/{d}.status.lastCell`, for diagnosis). **`loc.src:"cell"` fixes are produced by the relay,
never by the pager** — the pager's own `loc.src` is always `gnss` or absent; a `cell` sub-map on
the wire is raw cell identity, not a position.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `cell.mcc` | string, 3 digits | **yes** | Mobile Country Code. A string (not an int) so a leading zero survives. |
| `cell.mnc` | string, 2 or 3 digits | **yes** | Mobile Network Code. Same leading-zero reason as `mcc`. |
| `cell.tac` | uint, 0…65535 | **yes** | Tracking Area Code. |
| `cell.ci` | uint, 0…268435455 (2²⁸-1) | **yes** | E-UTRAN Cell Identity (28 bits). |
| `cell.rsrp` | int, dBm, -156…-30 | no | Reference Signal Received Power, if known. |

A malformed `cell` (an out-of-range value, a missing required sub-field, a non-object) is treated
as **absent**, not as a reason to drop the whole `/loc` envelope — the pager's actual fix/no_fix
answer is still good and must still be processed. Unknown `cell` sub-keys are ignored, per §3.1's
usual forward-compatibility rule. Relay-side: a resolved cell position is stored exactly like a
fix (`src:"cell"`, the provider's own accuracy radius, `fixTs` = the envelope's `ts` or the relay's
receive time if `ts` is 0) and fulfils the `loc_req` it answers, same as a real fix would; an
unresolved cell behaves exactly like a plain `no_fix` answer, except the raw cell identity and
timestamp are still recorded on `devices/{d}.status.lastCell` so the web app can show "last known
cell" even with no position. See `docs/SERVER_PLAN.md` §5.6/§3 for the relay-side cache and data
model, and `relay/app/cellgeo.py` for the pluggable provider (`google` / `opencellid` /
`none` — the default, which makes no third-party call at all).

The example is ~160 bytes; the worst case is ≤ ~200 bytes without `cell`, and ≤ ~260 bytes with a
maximal `cell` sub-map — §3.3's 640-byte limit applies unchanged and is nowhere near binding
either way. A `/loc` payload that violates any rule above is malformed
and is handled per §3.4 — logged and dropped, never crashing the ingest path.

Relay-side: dedup on `id` exactly as §4.2 dedups an up message, in the same transaction that
stores the fix. A periodic fix is **not** a thread entry; it updates the device's last-known
position. A fix answering a `req` additionally resolves that request (§13.4).

### 13.3 Device-side rate limit — **normative**

*(written normatively here, not left to the implementation, so that firmware, the
Python test client and the relay's mirrored limit agree on one set of numbers. A limit that the
device and the server disagree about is a limit that produces phantom `expired` requests.)*

1. A `loc_req` arriving **less than `loc_min_s`** (default **120 s**) after the last fix *attempt*
  is answered **immediately** from the last fix, with `cached:true`, **without powering GNSS**.
  The window runs from the last *attempt*, not the last success, so a device in a basement cannot
  be made to retry continuously by a user pressing a button. **Amended, v0.2 (`V02_DESIGN.md`
  §5):** a device may replace this fixed `loc_min_s` window with a **growing backoff** instead —
  5 minutes after the first failed attempt, doubling on every further failure, capped at 12 hours,
  and reset to zero by a successful fix. This is *stricter* than the fixed `loc_min_s` window (it
  only ever waits as long or longer), so it remains conformant with the rule above; a device
  choosing it reports the seconds until its next allowed attempt in `/status`'s `loc_backoff_s`
  (§5.1) so the relay and web app do not show a phantom "expired" while the device is deliberately
  quiet. `loc_min_s` keeps its old meaning (the floor a fixed-window device enforces, or the
  backoff-scheme device's minimum step) either way.
2. Otherwise the device attempts a fix, **bounded by 60 s**, and then answers: the fix if it got
  one, otherwise `loc:null, err:"no_fix"`. The device always answers a `loc_req` it accepted;
  silence is reserved for firmware that does not implement `kind` at all (§3.2).
3. **At most one fix attempt is in flight.** A second `loc_req` arriving during an attempt does not
  start another one; it is answered by the same result, as a separate `/loc` publish with its own
  `req`.
4. `loc_min_s` and the periodic interval `loc_period_s` are reported in `/status` (§5.1) for
  display. They are the **device's** choices — see §12 item 8 for why. `loc_backoff_s` (§5.1,
  v0.2) is reported the same way, for a device using item 1's growing-backoff amendment.

**The relay mirrors this limit; it does not merely trust it.**

5. At most **one in-flight `loc_req` per device**. A second requester inside the window **attaches
  to the existing request** rather than creating another: both requesters are answered by the one
  `/loc` that comes back. This is a transaction on a single per-device record, not a query, so two
  simultaneous requests cannot both win.
6. A request arriving **within 60 s of a fulfilled one** is answered from the stored fix with
  `cached:true`, without any wire traffic to the device at all.
7. Consequently one `/down` `loc_req` is published per device per 60 s at most, however many
  people ask. A location request is a privileged, rate-limited operation on someone else's
  battery: **who may make one is a server-side allow-list decision** — the same mechanism §4.2
  applies to messages, carrying a separate "may locate" right — enforced in the relay *and* in the
  store's own access rules, and never on the device. The device answers whatever it is asked;
  it is not the gate.
8. `cell` rides on the same rate-limited answer — it costs the pager nothing extra to attach (the
  serving cell is already known from the modem's registration state, no separate radio activity),
  so it does not get its own rate limit or its own `loc_req`/`/locate` path; it is only ever a
  sub-field of an ordinary `/loc` answer (§13.2).

### 13.4 Request lifecycle

`queued → sent → fulfilled | expired`, defined in §3.2 and repeated here for the whole picture:

| State | Entered when |
|---|---|
| `queued` | the relay stored the `loc_req` |
| `sent` | the broker accepted the QoS 1 `/down` publish (§4's `sent`, same meaning) |
| `fulfilled` | a `/loc` arrived whose `req` equals this request's `id` |
| `expired` | 15 minutes after creation with no matching `/loc` — **derived at read time**, like §4's `expired`; no timer, and nothing depends on a process being alive to run one |

A `loc_req` is never re-published on an online edge (§5.3), and it is never `shown`/`read`-acked
(§3.2), so `fulfilled` and `expired` are the only terminal states. Delivery state remains
monotonic: `fulfilled` and `expired` are terminal and a late `/loc` for an already-`expired`
request is logged and dropped, exactly as §4.1 rule 1 drops a redundant ack.

### 13.5 What the device may publish unsolicited

A periodic fix (`req:null`) is published every `loc_period_s` when that value is non-zero, at
QoS 0, and is the device's decision alone. §7.3 budgets it and flags the interval below which a
bad-coverage day breaks the project's 10 MB bar. `loc_period_s` = 0 disables periodic location
entirely and is a valid, fully conformant configuration; on-demand `loc_req` still works.

---

## 14. Device authentication

Every `/up`, `/status`, and `/loc` from a device configured with `authMode: "hmac"` MUST carry `n`
(replay counter) and `sig` (HMAC-SHA256 tag, truncated to 64 bits). The relay verifies the
signature and the replay window before parsing and storing the message.

### 14.1 Key material

- **`K_dev`:** 32 random bytes, generated by the relay when the device is created (phase 2b) and
  returned exactly once to the device in the encrypted provisioning bundle (§3.2). Stored server-side
  in the server-only Firestore collection `deviceSecrets/{deviceId}`, never in `devices/{d}` which
  the owner's browser can read.
- **One key per device, used in both directions.** The topic is part of the MAC input (§14.3), which
  separates directions and topics; no derived per-direction keys are needed.
- **MQTT password is separate.** Rotating one does not require rotating the other. Each secret is
  provisioned via the same one-time setup code but is managed independently at the broker and relay.
- **Rotation is a new setup code** (§3.5 of `DEVICE_PLAN.md`): the person types it on the device
  and the bootstrap fetch replaces the key. Silent over-the-air key rotation is not implemented.

### 14.2 Per-device, per-direction counter (`n`)

`n = (epoch << 20) | lo`. **v0.2 (`V02_DESIGN.md` §3): `epoch` widened from 12 to 32 bits, `lo`
stays 20 bits, so `n` is a 52-bit unsigned integer** (exact in JSON and in a Firestore int64;
§3.1's range is `0…2⁵³-1`, one bit of headroom above the tightest packing). It is strictly
increasing per publisher and is verified with a sliding window to absorb the jump when the epoch
increments. CBOR encodes `n` at minimal length, so nothing on the wire grows until the epoch passes
4095 — the same point at which the old 12-bit epoch would have wrapped and died. *(Why: TLS covers
only pager↔broker, and only when the server is authenticated, which §4.4 makes optional with a
deliberate fallback to no validation on a trust break — so TLS cannot be the sole replay defence;
the counter is what stops a third-party broker, or an on-path attacker during that fallback,
replaying an old signed `/down`. A 12-bit epoch's guaranteed exhaustion after 4096 cold boots — a
realistic lifetime for a device with a flaky battery — was the failure mode that forced the
widening; a 32-bit epoch is effectively unbounded for this project's purposes.)*

**Device side, `/up`, `/status`, `/loc`:**
- `lo` (low 20 bits) lives in RTC memory (§9.3 `auth.up_lo`) and increments per publish (free).
- `epoch` (now 32 bits, up from 12) lives in NVS and increments only on cold boot or when `lo`
  wraps. `n` itself is `uint64_t` end to end on the up path (signing, CBOR writer); only the NVS
  `epoch` field and the wire encoding change from v0.1. 20 bits of `lo` = 1 M envelopes per epoch.
  The relay's window (below) absorbs the jump at each cold boot regardless of epoch width.
- **Migration:** a device upgrading from v0.1 reads its old 12-bit-epoch NVS value once (a
  distinct, narrower key), then keeps the epoch going forward in a new, wider key — the two never
  share storage, so a half-migrated value can never be misread as either width.

**Device side, `/down`:**
- Device maintains `down_n` (highest accepted `n`) and a 64-bit bitmap of the 64 values below it in
  RTC (§9.3). This path may stay `uint32_t` internally (the relay's own `downN` counts by one from
  zero and has no epoch/lo split to begin with — see below), but MUST *parse* a `uint64_t` `n`
  without rejecting it, since the relay signs with the same widened field. A `/down` failing
  signature or window verification is treated as malformed (§3.4): count the error, log it, do
  **not** render, **do not ack**. The relay then sees the message stay `sent`.

**Relay side, per device (in `deviceSecrets/{d}`):**
- `upN` (highest accepted `n` for device-originated messages), `upBits` (64-bit bitmap of values
  below it). Both are plain integers with no epoch/lo split of their own — the split is a
  device-side NVS/RTC storage detail (how a device budgets its own limited non-volatile bits), not
  a wire or server-side concept; the relay only ever compares whole `n` values. Firestore stores
  `upN` as an int64, which holds every value up to `2⁵³-1` exactly, same as JSON.
- Accept an inbound `n` if `n > upN` (shift the window) or `upN − 64 < n ≤ upN` and its bit is clear;
  otherwise drop as a replay and log a security event. This arithmetic is unchanged by the width
  increase — `n` growing from 32 to 52 significant bits changes only which absolute values appear,
  never the comparison.
- The update is part of the same Firestore transaction that deduplicates by message `id`, so this adds
  no round trip.
- `downN` (§14.5, the relay's own per-device `/down` counter) is unaffected by any of the above: it
  counts by one from zero and is nowhere near either bound in practice, so it stays a plain
  incrementing integer with no epoch/lo split — the width increase exists for the *device's* counter,
  which is the one that has to survive thousands of cold boots on a coin-cell-scale clock, not the
  relay's.

**Still parked:** a signed resync handshake for the case where a device's NVS (and therefore its
whole identity, not just this counter) is lost outright — with a 32-bit epoch this is now only
needed if NVS itself is gone, at which point re-provisioning is required anyway (`DEVICE_PLAN.md`
§2.5).

### 14.3 Signature (`sig`)

**What is signed:** `HMAC-SHA256(K_dev, topic ‖ 0x00 ‖ P)[0:8]`, where `topic` is the full MQTT
topic string and `P` is defined below:

- **CBOR:** Publisher encodes the map with a header that includes the `sig` pair, writes every other
  pair, computes the tag over `topic ‖ 0x00 ‖ P` where `P` is everything written so far, then
  appends `sig` (key 13, byte string). No re-encoding, no canonical form: `sig` must be the last pair.
- **JSON:** Publisher serializes without `sig` (that is `P`, ending in `}`), MACs it, then emits
  `P[0:-1]` + `,"sig":"` + base64url(tag) + `"}`; `sig` is the only field with an ordering rule.

**Verification:** Malformed if missing `sig` or if the signature does not verify. Verifiers require
the payload to end with the expected tag bytes (CBOR: `0x0D 0x48` + 8 bytes; JSON: trailing tag in
the `sig` field) and MUST verify in constant time. Parsing happens **only after** verification
succeeds (§3.4).

**Size:** CBOR envelope with `n` and `sig` is ~15 bytes larger than unsigned; signed CBOR is ≈25%
smaller than unsigned JSON for the same message (§7.2).

### 14.4 Server-side verification

Order of operations (in `relay/app/ingest.py`):

1. Size check: ≤ 640 bytes (§3.3) — drop unparsed if not.
2. Look up `deviceSecrets` by device id from the topic (cache in-process). Record the result keyed by
   device id (changes only on key rotation).
3. If the device's `authMode` is `"hmac"`, extract and verify `sig` in constant time (§14.3).
   Failed signature → **count as `sigFailures`, log security event with topic and first 64 bytes,
   treat as malformed (do not render, do not store), still return 2xx to broker.**
4. Parse the message (now safe).
5. Check `n` in the sliding window (§14.2). Out-of-window → same as signature failure: count,
   log, drop, return 2xx.
6. Proceed with existing handling (dedup by `id`, route, store, etc.).

**Server-side per-device state:**

```
deviceSecrets/{deviceId}
{
  hmacKey,                    // 32 bytes, base64 in JSON
  mqttPasswordHash,           // from §6.1; also server-only
  upN,                        // highest accepted n for /up, /status, /loc
  upBits,                     // 64-bit sliding window bitmap
  downN,                      // last n issued for /down (incremented per publish)
  sigFailures,                // counter; reset on key rotation
  authMode,                   // "hmac" or unset (v1 device, unsigned)
  createdAt, rotatedAt,       // timestamps
  ...
}
```

**Alert on repeated signature failures:** More than 20 failures in 10 minutes on one device sets
a flag in `devices/{d}.status.authAlarm` for the admin UI.

### 14.5 Signing `/down` (relay-originated)

Every `/down` message published by the relay (via the broker's REST API) includes `n` (an
incrementing counter per device) and `sig` (computed as §14.3). The counter increment happens in
the same transaction that wrote `deviceSecrets.downN`, so it is atomic and never repeats.
`downN` is not reset on key rotation; it continues from where it left off, so the window can still
detect a replayed message from before the rotation.

Every `/down` kind (msg, book, cfg, loc_req) is signed if the device is `hmac`-authenticated. A v1
device configured without signatures sees an unsigned `/down`; one provisioned with `authMode:
"hmac"` sees all `/down` signed. The device's provisioning bundle (§3.2) sets the `req_sig` flag.

### 14.6 Unsigned LWT exception

The broker-generated LWT `{"v":1,"state":"offline",…}` cannot carry a signature. The relay accepts
an unsigned `/status` **only** when it is exactly `state:"offline"` with no live fields (no `mode`,
`batt_mv`, `rssi`, `session`, etc.). The worst a forger can do with it is mark a device offline in
the parent UI until the next signed `online` message arrives — accepted as tolerable.
