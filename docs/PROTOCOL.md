# PROTOCOL.md — School Pickup Pager wire contract (MVP + v2)

**Status:** authoritative. Phase 2–6 (relay, simulator, firmware `net.c`/`modes.c`/`msg.c`, parent UI)
MUST conform to this document. Per HANDOFF.md §7.6, any topic or schema change edits this file
*first*, then code. The same rule governs the v2 server stack (`docs/SERVER_PLAN.md`), whose wire
changes are all in this document.

**Scope:** MVP = text relay only. Geofences, schedule-based mode switching and device-side SMS
remain out of scope (HANDOFF.md §1); §11 records where the contract leaves room for them.
**v2 adds two things and nothing else**: named users on the wire (`from` as an alias, optional `to`,
§3.1) and location (`kind:"loc_req"` on `/down` §3.2, the `/loc` topic §13). Every v2 addition is
additive — an MVP device that ignores all of it stays conformant, and a v1 payload stays valid.

**Conventions used below**
- `(Phase 1 decision — see rationale)` = not fixed by HANDOFF.md; decided here, with a one-line reason.
- `(v2 decision — see rationale)` = the same, for the v2 server stack (`docs/SERVER_PLAN.md` §4).
  Every one of these is additive to the MVP wire and none of them moves the §3.3 byte limit, changes
  a QoS or retained flag, or adds a device subscription.
- `NEEDS HUMAN DECISION` = would require a paid service, a firmware dependency beyond
  `walter-modem` / `esp_timer` / a display driver, or a GPIO change. Not decided here. Collected in §12.
- `UNVERIFIED` = a hardware or vendor-library fact this document assumes but cannot confirm; each
  one names the cheapest experiment that settles it.
- Power numbers are labelled `(estimate)`, `(vendor)` or `(TBD — hardware measurement)`. `(vendor)`
  means a figure documented by DPTechnics in the `walter-modem` source; everything else is still an
  estimate. No number here is a *measured* number yet; Phase 4 replaces the estimates with
  measurements on hardware.
- `RESOLVED Phase 4` marks a Phase 1 assumption that was settled by reading
  `dptechnics/walter-modem` **v1.5.0** at
  `firmware/managed_components/dptechnics__walter-modem/`. Source line references are against that
  version; re-check them if the component is upgraded. v1.5.0 rewrote the MQTT API
  (event-driven receive, `mqttReceive()`, `mqttDidRing()` deprecated) — do not read conclusions
  drawn against older releases into this document.

---

## 1. Identifiers

| Thing | Format | Max len | Notes |
|---|---|---|---|
| `device_id` | `^[a-z0-9][a-z0-9-]{2,23}$` | 24 | lowercase; example `pgr-0001` *(Phase 1 decision — bounds max topic length to 37 bytes so device-side topic buffers are static)* |
| MQTT client id (device) | exactly `device_id` | 24 | *(Phase 1 decision — stable across reboots is required for `cleanSession=false` session resumption; see §6)* |
| MQTT client id (relay) | `relay-1` | — | *(Phase 1 decision — fixed id so the relay also resumes a persistent session and does not miss `/up` while restarting.)* *(v2 decision — the relay no longer holds an MQTT session at all; see §2's transport note. The id is retained only for whatever short-lived client the relay or a test harness opens, and nothing on the device depends on it.)* |
| Message id (relay-originated, down) | `m_` + 8 lowercase hex | 10 | 32 bits of `os.urandom`; UNIQUE in the relay store, regenerate on collision |
| Message id (device-originated, up) | `u_` + 8 lowercase hex | 10 | from `esp_random()` |
| Location id (device-originated, `/loc`) | `l_` + 8 lowercase hex | 10 | from `esp_random()` *(v2 decision — a distinct prefix so a `/loc` envelope is identifiable in logs without its topic; it is still just an `id` per the rule below.)* |
| Session id | `s_` + 8 lowercase hex | 10 | per **cold boot**, not per deep-sleep wake; lives in RTC memory (§9) |

`id` is opaque to every consumer. Validators accept `^[a-z0-9_]{3,16}$` so a future generator can
change the prefix scheme without a protocol version bump.

---

## 2. Topics

Fixed by HANDOFF.md §2. QoS/retained for `/status` and all LWT settings are Phase 1 decisions.

| Topic | Direction | QoS | Retained | Publisher | Subscriber |
|---|---|---|---|---|---|
| `pager/{device_id}/down` | relay → device | 1 | **false** (fixed) | relay | device only (`pager/{own_id}/down`) |
| `pager/{device_id}/up` | device → relay | 1 | **false** *(Phase 1 decision — a retained reply would be redelivered to the relay on every relay reconnect and double-post to the thread)* | device | relay (`pager/+/up`) |
| `pager/{device_id}/status` | device → relay | **1** *(Phase 1 decision — QoS 0 can silently lose the `online` edge that triggers re-publish of unacked messages)* | **true** (fixed) | device, and broker on LWT | relay (`pager/+/status`) |
| `pager/{device_id}/loc` | device → relay | **1** when `req` is non-null, **0** otherwise *(v2 decision — an answer to a location request is something a human is waiting on and must not be silently lost; an unsolicited periodic fix is superseded by the next one, so QoS 0 is right and cheaper)* | **false** *(v2 decision — a retained fix would be redelivered to the relay on every relay reconnect and re-post a stale position)* | device | relay (`pager/+/loc`) |

- The device subscribes to **exactly one** topic: `pager/{own_id}/down`, QoS 1. No wildcards on the
  device *(Phase 1 decision — a wildcard subscription on a metered link is an unbounded data risk)*.
  This is unchanged in v2: `/loc` is a **publish**, and location requests arrive on `/down` as a
  `kind` (§3.2), not on a second subscription.
- The relay receives `pager/+/up`, `pager/+/status` and (v2) `pager/+/loc`. In the MVP it received
  them as an MQTT subscriber at QoS 1; from v2 the broker pushes them to the relay — see the
  transport note below.
- Broker ACLs: device credentials may publish **only** to `pager/{own_id}/up`,
  `pager/{own_id}/status` and (v2) `pager/{own_id}/loc`, and subscribe **only** to
  `pager/{own_id}/down`. The `relay` credential gets the mirror image. Enforced at the broker, not
  just in code.
- Reserved-but-unused: `pager/{device_id}/cfg`, `/evt` (§11). Devices MUST NOT subscribe to them.

**Relay transport (v2 decision — the relay must not require a long-lived process).** The relay's
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

## 3. Message schema (JSON)

Payloads are **minified UTF-8 JSON objects**, no BOM, no trailing newline, no whitespace between
tokens. One JSON object per MQTT payload.

Base envelope (from HANDOFF.md §2, plus `v`):

```json
{"v":1,"id":"m_7f3a","ts":1757700000,"from":"parent","body":"Pickup at 3:15 by the gym","ack":null}
```

### 3.1 Fields

| Field | Type | Required | Range / max | Meaning |
|---|---|---|---|---|
| `v` | int | no (default `1`) | `1` | Schema version. *(Phase 1 decision — a one-key, 6-byte cost that makes §10 migration possible; absent MUST be read as `1` so the HANDOFF.md example stays valid.)* |
| `id` | string | **yes** | `^[a-z0-9_]{3,16}$` | Message id (§1). On an ack, this is the **down message's** id. |
| `ts` | int | **yes** | 0 or 1×10⁹…2×10⁹ | Unix epoch **seconds, UTC**. Set by the publisher. |
| `from` | string | yes on content messages, **absent** on acks | `^[a-z0-9][a-z0-9_-]{0,15}$` **or** the literal `system`, ≤16 chars | Author's **alias**. *(v2 decision — a deployment now has named users rather than one parent and one student, so `from` carries the sender's alias instead of a two-value enum. `parent` and `student` are still valid aliases, so every MVP payload stays valid and an existing device thread keeps rendering. **Firmware impact: none** — `msg.c` already accepts any 1–16 byte string and renders it verbatim.)* |
| `body` | string | yes on content messages, **absent** on acks | ≤ **160 Unicode code points** (fixed) **and** ≤ **320 UTF-8 bytes** *(Phase 1 decision — the code-point cap alone allows 640 bytes; the byte cap lets firmware size static buffers, and §9.4 turns it into the 161-byte RTC mirror by way of the ASCII-only CardKB)* | Message text. |
| `ack` | string \| null | yes; `null` on content messages | `shown` \| `read` | Ack state being reported. |
| `kind` | string | no (default `msg`) | `msg` \| `loc_req`; `/down` only | What the down message *is* (§3.2). Absent MUST be read as `msg`. *(v2 decision — a location request needs a down message the device does not render or ack; a field on `/down` costs 17 bytes, whereas a `/cmd` topic would cost the device a second subscription, which §2 exists to prevent.)* |
| `to` | string | no; `/up` content messages only | same regex as `from` | Recipient alias chosen by the device. Absent → the relay uses the device's configured default recipient, or broadcasts to every user the owner may message. *(v2 decision — the device can now address one of several users; optional so an MVP device that never sets it keeps working unchanged.)* |

Additional rules:
- `body` MUST NOT contain Unicode control characters `U+0000`–`U+001F` or `U+007F`. Relay strips
  them on ingest from the parent API and rejects if the result is empty. *(Phase 1 decision — keeps
  JSON escaping bounded, and the e-paper renderer has no control-char handling.)*
- `body` MUST NOT be empty on a content message.
- **Unknown fields MUST be ignored, not rejected.** This is the forward-compatibility rule that
  makes §11 additive.
- Field order is unspecified; a receiver MUST NOT depend on it. Publishers SHOULD emit
  `v,id,ts,kind,from,to,body,ack` in that order to keep logs diffable *(v2 decision — the two new
  keys are slotted into the existing order rather than appended, so an MVP payload is still a
  prefix-compatible subsequence of a v2 one)*.
- A publisher SHOULD omit `kind` when it is `msg` and omit `to` when it has no recipient to name;
  both defaults are defined precisely so the common payload does not grow.
- `system` matches the alias regex, so it is a **reserved alias**: the relay MUST NOT issue it to a
  user *(v2 decision — §11's `system` enum headroom is only headroom if nothing else can claim it,
  and a user able to publish as `system` could forge relay notices)*. A `to` of `system` is an
  unknown recipient and takes §4.2's drop path. *(v2 decision — the reservation needs a matching
  inbound rule or it does not achieve what its rationale claims.)* `from:"system"` is **shape-valid
  on the wire** — wire-format validation checks shape only — but the relay MUST NOT attribute a
  stored message to `system` on the strength of a device-supplied `from`. Attribution of an `/up`
  message is the relay's decision from the device→user mapping, never the device's claim; a
  `from` that disagrees with that mapping is logged as a **security event** (same class as §4.1
  rule 4's wrong-device ack). This is a relay-side rule; it is deliberately *not* a reason to
  reject the envelope, so nothing the device sees changes.

### 3.2 Message kinds

| Kind | Topic | Shape |
|---|---|---|
| Down message (sender → device) | `/down` | `{"v":1,"id":"m_7f3a","ts":…,"from":"parent","body":"…","ack":null}` — **99 bytes** for the example above |
| Location request (v2) | `/down` | `{"v":1,"id":"m_7f3a","ts":…,"kind":"loc_req","from":"mom","ack":null}` — **78 bytes**; no `body` |
| Ack (device → relay) | `/up` | `{"v":1,"id":"m_7f3a","ts":…,"ack":"shown"}` — **51 bytes**; no `from`, no `body` |
| Up message (device reply) | `/up` | `{"v":1,"id":"u_91c0","ts":…,"from":"student","body":"ok coming","ack":null}` — **84 bytes** |
| Up message addressed (v2) | `/up` | `{"v":1,"id":"u_91c0","ts":…,"from":"student","to":"mom","body":"ok coming","ack":null}` — **95 bytes** |
| Location (v2) | `/loc` | §13 |

A receiver distinguishes an ack from a content message by `ack !== null`. A payload with both a
non-null `ack` and a non-empty `body` is **malformed** (§3.4).

**`kind:"loc_req"` (v2 decision — see §13 for the answer it asks for).** A location request is a
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
  **15 minutes** after creation *(v2 decision — a location answer that is a quarter of an hour late
  answers a question nobody is still asking; it is also short enough that a derived state needs no
  timer)*.
- A `loc_req` is **not** re-published on an online edge (§5.3) — a stale location request is
  worthless, and re-asking is one API call for the requester.
- **Firmware note:** current firmware treats a body-less down message as malformed and drops it
  *without acking* (§3.4). That is exactly the right behaviour for a device that predates this
  field: the request is never answered, it simply expires, and no message state moves backwards.
  Parsing `kind` is a firmware follow-up, not a prerequisite for the rest of v2.

### 3.3 Envelope size limit

Worst case, with every field at its maximum and JSON escaping expanding `"` and `\` to two bytes.
**This table is a deliberately conservative per-field ceiling, not a payload that can exist**: it
sums each field's independent maximum, and several of those maxima are mutually exclusive (§3.2
makes a non-null `ack` and a non-empty `body` malformed, so lines 6 and 7 never co-occur), while
the `body` line over-counts — see the achievable figures below the limit.

```
{}                                   2
"v":1,                               6
"id":"<=16>",                       24
"ts":1757700000,                    16
"from":"<=16>",                     26
"body":"<=480 escaped>",           490   (320 UTF-8 bytes, up to 160 of them escaped 1->2)
"ack":"<=8>"                        16
                                  ----
                                   580   v1 worst case (MVP fields only)

"to":"<=16>",                       24   v2, /up content messages only
                                  ----
                                   604   v2 worst case: a maximal up message carrying `to`

"kind":"loc_req",                   17   v2, /down only; longest value of a closed 2-value enum
                                  ----
                                   621   arithmetic ceiling if every v2 field is present at maximum
```

**Hard limit: 640 bytes — unchanged.** Any payload larger than 640 bytes on any topic MUST be
dropped unparsed by both relay and device *(Phase 1 decision — 580 rounded up with headroom to a
size the firmware can statically allocate; a fixed limit means the device never mallocs on the RX
path)*.

*(v2 decision — the two added fields are sized against the existing limit rather than moving it,
because the limit is what lets the device statically allocate its RX buffer and any change to it
would be a firmware flag day.)*

*(v2 decision — the ceiling above is kept as-is, because 640 was derived from it in Phase 1 and
re-deriving the limit would be a firmware flag day for no gain; but the achievable numbers are
recorded here, because "604" was being read as a real payload size and it is not one.)* The
**largest payload that can actually exist is 438 bytes**: a `/up` content message with `id`,
`from` and `to` all 16 characters and `body` at its cap. The 490-byte `body` line is unreachable
because §3.1's two caps bind together — a character that escapes 1→2 (`"` or `\`) is one UTF-8
byte, so 160 of them consume the whole 160-code-point budget and leave nothing to spend the
remaining 160 UTF-8 bytes on. The maximum a `body` can occupy on the wire is therefore **320
bytes**, not 480, reached identically by 160 escaped quotes or by 160 two-byte code points.
Every other v2 shape is smaller: a maximal `/down` `msg` is 414 bytes, a `loc_req` 101 bytes
(78 in §3.2's example), a maximal §13 `/loc` envelope 188 bytes. Real headroom against the limit
is **≥ 202 bytes**, and the common shapes have more than 500.

*(v2 decision — the one escaping assumption the limit depends on, stated because it was previously
implicit.)* Publishers MUST serialise non-ASCII `body` characters as **raw UTF-8, not `\uXXXX`
escapes**. §3's "minified UTF-8 JSON" already implies this, but the consequence is load-bearing:
a legal 160-code-point non-ASCII `body` emitted with `\u` escaping is 1078 bytes and would be
dropped unparsed by every receiver, making §3.1's `body` caps and this limit jointly
unsatisfiable. The relay and the device both already emit raw UTF-8; this fixes that in writing.
The 621-byte line is arithmetic only: `kind` appears solely on
`/down`, where `to` never appears, and the only non-default `kind` is `loc_req`, which carries no
`body` at all (78 bytes in §3.2's example). §13's `/loc` envelope is ≤ ~200 bytes worst case. Every
v2 shape therefore has at least 19 bytes of headroom against the stated ceiling, and the common ones have
more than 500.

### 3.4 Malformed payload handling

A payload is malformed if it is >640 bytes, not valid UTF-8, not a JSON object, missing `id`/`ts`/
`ack`, has an out-of-range field, violates a `body` rule, or sets both `ack` and `body`.

*(v2 decision — unknown **values** need the same rule as unknown fields, or the enum additions in
§3.1 are not additive after all.)* A `kind` the receiver does not recognise is handled exactly like
`loc_req` is handled by MVP firmware: **do not render, do not ack, count it, drop it.** The message
then expires at the relay, which is the visible, correct outcome. This is the only place a v2
receiver may treat a well-formed envelope as undeliverable; it is deliberately the same code path
as §3.4's malformed handling below.

- **Device:** log, increment a counter, **do not ack**, do not render, do not reboot. The message
  stays `sent` at the relay and the parent UI shows it as undelivered.
- **Relay:** log with the topic and first 64 bytes, drop. Never crash the ingest path on a parse
  error — one bad payload must not take down the persistent session, and (v2) must not fail the
  broker's webhook request either: the relay logs it, drops it, and still answers 2xx, or the
  broker will redeliver the same bad payload forever. Never auto-reply on MQTT. *(v2 exception, and
  the only one: an up message whose `to` names an unknown or disallowed recipient is answered with
  one `system` down message — see §4.2. That is a routing failure of a well-formed payload, not a
  parse failure.)*
- The relay's HTTP API rejects oversize/invalid bodies with `400` **before** anything is stored, so
  a malformed message never reaches the air interface.

### 3.5 Clock

The device sets `ts` from the LTE network clock obtained at attach (NITZ / modem RTC via
`walter-modem`). If no network time is available yet, the device publishes `ts: 0` and the relay
substitutes its own receive time. *(Phase 1 decision — avoids adding an SNTP/`esp_netif` code path
purely for timestamps; the relay is already authoritative for thread ordering.)*
Thread order in the parent UI is the relay's insertion order (SQLite rowid), **not** `ts`.

---

## 4. Ack state machine (down messages)

**The relay is authoritative.** The device is a reporter of events; it never holds the canonical
state. States are **monotonic** — a message never moves backwards.

```
             relay stores        broker accepts       device /up ack       device /up ack
 (HTTP POST) ------------> queued --------------> sent --------------> shown -------------> read
                                (PUBACK, or 2xx
                                 from the publish
                                 API — §2)
                             |                      |
                             |  24 h, no ack        |  24 h, no ack
                             +----------------------+--------> expired  (terminal, relay-only)
```

| State | Owner | Entered when | Notes |
|---|---|---|---|
| `queued` | relay | message committed to the relay's store by the send API | Also the state of a message whose publish attempt failed (broker down). |
| `sent` | relay | broker returns **PUBACK** for the QoS 1 `/down` publish — (v2) equivalently, the broker's REST publish API accepts the QoS 1 publish with a 2xx | Means *the broker accepted it*, **not** that the device received it. The UI must not say "delivered" here. *(v2 decision — the two are the same fact carried over different transports (§2), so the state machine, its rules and its timings are untouched.)* |
| `shown` | relay, on device report | device publishes `{"id":…,"ack":"shown"}` on `/up` | Device publishes this **after the e-paper refresh completes** (BUSY deasserted), never before. |
| `read` | relay, on device report | device publishes `{"id":…,"ack":"read"}` on `/up` | Triggered by a short press of button IO1 while the message is on screen. |
| `expired` | relay | still `queued` or `sent` 24 h after creation | *(Phase 1 decision — a pickup pager delivering "be at the gym at 3:15" two days late is worse than not delivering it. Extends HANDOFF.md's four states with a terminal, relay-only state; the device never sees or acks it.)* |

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
   re-enter active mode. *(Phase 1 decision — prevents a QoS 1 redelivery storm after a reconnect
   from costing a full active-mode window per duplicate, which is the single most expensive
   failure mode in the power budget: ~2–3 mAh per spurious 10-minute window, estimate.)*

### 4.2 Up messages (student replies)

**Up messages have no relay-side lifecycle beyond storage, and the relay sends no ack back to the
device** *(Phase 1 decision — an ack-of-reply would cost one extra down message plus one extra
radio wake per reply for information the device already has from its own PUBACK; the parent page is
the endpoint that matters)*.

Device-side only, in RTC memory: a **2-entry** queue holding the reply body at full fidelity
(§9.3, §9.4). A reply is never truncated to fit; the composer refuses input past 160 bytes
instead.

```
compose --> pending --(modem PUBACK)--> done            (entry freed)
              |
              +--(3 failed attempts, or >2 h old)--> failed  (UI shows "not sent", entry freed)
```

Relay-side an up message is simply inserted with `direction='up'` and served to the reading UI. An
up message whose `id` already exists is a duplicate and is dropped (QoS 1 redelivery); the dedup
check on that `id` MUST be part of the same transaction that stores the message, or a QoS 1
redelivery that races the original posts the thread entry twice.

**Routing an up message's `to` (v2 decision — the allow-list is a server-side rule, so the device
is never trusted to know who it may talk to).**

1. `to` absent → the relay routes to the device's configured default recipient, or, if it has
   none, to every user the device's owner is allowed to message. This is the MVP behaviour, so an
   MVP device keeps working with no change.
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
| `mode` | string | yes when `online` | `sleep` \| `active` | Device mode (HANDOFF.md §2) |
| `batt_mv` | int | yes when `online` | 2000…4500 | Battery millivolts. *(Phase 1 decision — raw mV, not percent; LiFePO4 has a flat 3.2 V plateau so any percent mapping belongs in the UI where it can be changed without a firmware flash.)* |
| `rssi` | int | no | −140…0 | dBm, for field debugging |
| `session` | string | **yes** | `^s_[0-9a-f]{8}$` | Cold-boot session id (§1). Lets the relay tell a reboot from a deep-sleep cycle. |
| `ts` | int | yes when `online` | epoch s, or 0 | Same rule as §3.5 |
| `fw` | string | no | ≤16 chars | Firmware version |
| `loc_period_s` | int | no | 0…86400 | v2. The periodic `/loc` interval **the device has chosen** (§13); `0` = periodic location off. |
| `loc_min_s` | int | no | 0…86400 | v2. The device's own minimum gap between on-demand fixes (§13.3); default 120. |

*(v2 decision — both fields are **display and diagnosis only**; the relay stores the reported
values and never writes them back. The device owns its location duty cycle because the cost being
traded is GNSS power on its battery (§12 item 8), which the server cannot see. Making these
server-settable would need a `/cfg` topic, which §11 still only reserves.)* They are optional, so
an MVP `/status` remains valid and a relay MUST treat their absence as "unknown", not as `0`.

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
  offline→online edge: **re-publish unacked messages** (HANDOFF.md §2). Broker QoS 1 covers the
  common case; this covers session loss.
  - Order: oldest first.
  - Selection: state in (`queued`, `sent`), age < 24 h.
  - Cap: **at most 10 messages per online edge** *(Phase 1 decision — matches the device's
    10-entry thread ring, which Phase 5 moved from RTC memory to ordinary RAM (§9.5) but kept at
    depth 10 precisely so this cap still lines up; bounds the reconnect burst to ~8 kB / ~1
    active-mode window. Older unacked messages are left for the `expired` sweep)*.
  - Re-publish reuses the **same `id`** so device dedup (§4.1 rule 7) suppresses double-rendering.
  - **Selection excludes `kind:"loc_req"` (v2 decision — a location request that missed its window
    is worthless, and re-asking is one API call for the requester).** A `loc_req` that is still
    `sent` when the device comes back simply expires per §3.2.
  - *(v2 decision — same rule, new trigger.)* The edge is detected from the `/status` message
    however it reaches the relay: in v2 that is the broker's `/status` push (§2), not a
    subscriber callback. Selection, order, cap and the re-used `id` are unchanged.
- The relay MUST NOT publish anything to `/down` on a timer for liveness. There is no application
  ping. MQTT keepalive is the only liveness mechanism (§6).

### 5.4 Device publish cadence

Status is published: (a) immediately after MQTT connect, (b) on every mode change, (c) when
`batt_mv` has moved more than 50 mV since the last publish, and (d) as a heartbeat, at most
**once per 3600 s**.

*(Phase 1 decision — the heartbeat period is 2× the MQTT keepalive interval (§6). Marginal cost
≈ 0.33 kB of data and ≈ 0 extra radio sessions. An independent hourly status timer would add ~24
radio wakes/day ≈ 2.4 mAh/day (estimate) — a meaningful tax for nothing. **Phase 4 update:** the
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
| MQTT version | **3.1.1** | *(Phase 1 decision.)* **RESOLVED Phase 4 by header read**: the library exposes no version, no `sessionExpiry`, and no MQTT 5 property API anywhere. `mqttConnect()` emits `AT+SQNSMQTTCONNECT=0,<host>,<port>,<keepAlive>` (`src/proto/WalterMQTT.cpp:83-95`). 3.1.1 it is. |
| Clean session | **false** — **NOT SETTABLE from the library** | Fixed in spirit by HANDOFF.md's "one persistent session": with `cleanSession=false` and a stable client id the broker queues QoS 1 `/down` while the TCP link is briefly down. **Phase 4 finding:** neither `mqttConfig()` nor `mqttConnect()` exposes a clean-session flag, so the value is whatever the Sequans MQTT client defaults to. UNVERIFIED, and load-bearing for §4.1 rule 6 and §5.3. Weak evidence against us: `mqttConnect()` *deliberately clears the entire local subscription table* before connecting (`src/proto/WalterMQTT.cpp:87-89`), and the vendor's own `examples/mqtts` re-subscribes from inside the CONNECTED event handler — which is what a library assuming a **clean** session on every connect would look like. Experiment: connect, reset the ESP32 only, and have the relay publish while the device is down — if the queued message arrives on reconnect, the session is persistent. ~20 min on hardware. |
| LWT | **NOT SETTABLE from the library** | `mqttConfig()` emits `AT+SQNSMQTTCFG=0,"<clientId>"[,"<user>","<pass>"][,<tlsProfileId>]` and stops there (`src/proto/WalterMQTT.cpp:53-75`) — no will topic, message, QoS or retain argument, and grepping the whole of `src/` for `will`/`lastwill` returns nothing. §5.2's LWT contract therefore has no implementation path through the typed API. Fallback: `WalterModem::sendCmd()` (public) can queue a raw `AT+SQNSMQTTCFG=...` carrying the will parameters *before* `mqttConnect()`. UNVERIFIED against the Sequans AT manual. If that fails, the relay must fall back to inferring offline from keepalive expiry and §5.2's LWT becomes advisory. Tracked in §12. |
| TLS | server-authenticated, CA pinned in modem NVM; username/password per device | Free-tier HiveMQ Cloud model. Provisioning is supported and is exactly the vendor's `examples/mqtts` flow: `tlsWriteCredential(false, 12, ca_pem)` → `tlsConfigProfile(2, WALTER_MODEM_TLS_VALIDATION_CA, WALTER_MODEM_TLS_VERSION_12, 12)` → `mqttConfig(client_id, user, pass, 2)`. Both functions are public (`src/WalterModem.h:4147` and `:4404`; the `public:` block starts at `:4132`). **Slot discipline, from `examples/mqtts/main/mqtts.cpp:249-251`: certificate indices 0–10 and private-key index 1 are reserved for Sequans/BlueCherry — use certificate slot ≥ 11, and TLS profile ≥ 2 (profile 1 is BlueCherry's).** Note this is stricter than the `tlsWriteCredential` doc comment's "10–19"; follow the example. |
| Reconnect policy | **Only** on detected session loss. Never on a timer (HANDOFF.md §1). Backoff 5 s, 15 s, 60 s, 300 s, then 300 s steady. | Each reconnect costs a full TLS handshake ≈ 5 kB (§7) — reconnects are the largest single term in the data budget. |

*(v2 decision — scoping note, no behaviour change.)* Every row above describes the **device's**
session. The relay no longer keeps an MQTT session of its own (§2), so its former persistent
session, its `relay-1` client id and its reconnect behaviour are not part of this contract; the
broker-generated LWT of §5.2 still matters, because it is the broker, not the relay, that produces
it. The broker must therefore still support an LWT and QoS 1 in both directions (§12 item 2).

### 6.2 Keepalive

| Parameter | Value |
|---|---|
| MQTT keepalive | **1800 s (30 min)** *(Phase 1 decision — see arithmetic below)*, passed as the third argument of `mqttConnect(host, port, keepAlive)` |
| ESP32 RTC-timer wake for keepalive | **None. Removed in Phase 4.** The MQTT client runs *inside* the modem (HANDOFF.md §2) and the library exposes **no ping API at all** — there is no `mqttPing()`, and `keepAlive` is handed to the modem in the `AT+SQNSMQTTCONNECT` command. PINGREQ is therefore the modem's job by construction, not the ESP32's. The old wake source #3 does not exist. |
| Server Keep Alive override | If the broker documents a lower cap, the **smaller** value wins. Note `WALTER_MODEM_MQTT_MIN_PREF_KEEP_ALIVE` (`src/WalterModem.h:236`, value 20) is still declared but referenced nowhere in v1.5.0, so the library imposes no floor of its own. Whether the modem or broker silently clamps 1800 s is UNVERIFIED. |

Arithmetic behind 1800 s: a PINGREQ/PINGRESP pair costs ~0.18 kB of data (negligible) but requires
an RRC connection — roughly 3 s at ~120 mA ≈ 0.1 mAh (estimate). At 1800 s that is 48 pings/day
≈ **4.8 mAh/day**; at 900 s it is 96 pings/day ≈ 9.6 mAh/day. Against a sleep-mode budget of roughly
20 mAh/day, halving the keepalive would cost ~25 % of the battery for no latency benefit — down
messages arrive via paging (§6.3), not via the keepalive. Longer than 1800 s risks carrier NAT
timeout on the TCP flow. `PAGER_MQTT_KEEPALIVE_S` is a compile-time constant so Phase 4 can retune
it against a measurement.

Note the ~4.8 mAh/day keepalive term is now a **modem-side** cost only: the ESP32 does not wake for
it, so it is unaffected by the sleep-cycle rewrite in §8.

UNVERIFIED: carrier NAT idle timeout on this SIM. Cheapest experiment: leave the device idle with
keepalive disabled and log how long before the first publish fails; bisect 5/15/30/60 min. Costs one
overnight run, no extra hardware.

### 6.3 eDRX (sleep mode)

| Parameter | Value | Note |
|---|---|---|
| eDRX cycle | **20.48 s** (fixed, HANDOFF.md §1) | 3GPP WB-S1 eDRX value nibble `"0010"`. **RESOLVED Phase 4 by source read**: `configEDRX(mode, req_edrx_val, req_ptw)` takes `const char *` and splices both verbatim into `AT+SQNEDRX=<mode>,<actType>,"<req_edrx_val>","<req_ptw>"` (`src/WalterModem.cpp:4757-4774`). They are **raw 4-bit binary nibble strings**, not seconds. Firmware passes the literals `"0010"` / `"0001"`. |
| PTW (Paging Time Window) | **2.56 s** *(Phase 1 decision — with a 1.28 s idle DRX this gives 2 paging occasions per cycle, i.e. one retry, at ~0.2 mA average; PTW 5.12 s would give 4 occasions and roughly double the paging energy for a redundancy we do not need)* | WB-S1 PTW nibble `0001` |
| PSM | **disabled in MVP** *(Phase 1 decision — PSM suspends paging entirely, which breaks the ≤30 s sleep-mode delivery target. Revisit only if a scheduled/mailbox mode is added (§11).)* | Firmware calls `configPSM(WALTER_MODEM_PSM_DISABLE)` explicitly rather than relying on a default. Two traps if PSM is ever enabled, both still present in v1.5.0: `configPSM()` takes `const char *` for T3412/T3324 but the helpers `durationToTAU()` / `durationToActiveTime()` return a `uint8_t` (`src/WalterModem.h:5681,5699`), so the caller must format the byte as an 8-character binary string itself; and `_convertDuration()` (`src/WalterModem.cpp:4796`) writes `final_base * multiplier` — the *index* into the base-time table — into `actual_duration_seconds` instead of `base_times[final_base] * multiplier`, so the reported actual duration is wrong. Neither affects the MVP. |
| Carrier acceptance | The network may grant different eDRX/PTW values than requested. Firmware MUST log the **granted** values and use them for its latency math. | UNVERIFIED per SIM/carrier, but **v1.5.0 gives us a first-class API for it** — no log-scraping needed. Register `setNetworkEventHandler()` (`src/WalterModem.h:5924`); on `WALTER_MODEM_NETWORK_EVENT_EDRX_RECEIVED` (`:1348`) the handler receives `WMNetworkEventData.edrx { actType, requestedEdrx[16], nwProvidedEdrx[16], pagingTimeWindow[16] }` (`:2388-2405`), parsed by the library at `src/WalterModem.cpp:1914-1930`. **`nwProvidedEdrx` is the granted value and is the one §6.5's latency math must use.** The same handler's `cereg` branch also carries granted PSM values (`activeTime`, `periodicTau`, `hasPsmInfo`), which is how firmware confirms PSM really is off. Use `configEDRX(WALTER_MODEM_EDRX_ENABLE_WITH_RESULT, …)` (mode `2`) so the modem emits them. |

### 6.4 How these interact with "one persistent session"

The MQTT session lives **inside the modem** and survives ESP32 deep sleep. Therefore:

1. ESP32 deep sleep MUST NOT power-cycle or reset the modem. Any Phase 4 power-gating of the modem
   would destroy the session and force a ~5 kB TLS handshake on every wake — that alone would be
   ~150 kB/day at 30 wakes/day and would dominate everything else in §7.
2. eDRX is what makes a 30-minute keepalive compatible with 30-second delivery: the network pages
   the modem within one eDRX cycle (≤20.48 s), independent of the keepalive timer.
3. The ESP32 does **not** wake for the keepalive (§6.2). The only periodic ESP32 wake in sleep mode
   is the **5 s wake-and-drain cycle** of §8 — which exists because the library's incoming-message
   state does not survive an ESP32 restart, not because the session needs servicing.

### 6.5 Latency budget check

Rewritten in Phase 4: the ESP32 is woken by its own poll timer, not by the modem (§8).

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
2. An e-paper refresh slower than 1.5 s. Phase 5 must measure the partial-refresh time and report
   back here if it exceeds 1.5 s; at `T` = 5 s there is only 2.7 s of slack to spend. Phase 5 uses a
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

| Exchange | MQTT bytes | + TLS | + TCP/IP | Total |
|---|---|---|---|---|
| `/down` PUBLISH (99 B payload, topic 19) | 124 | 153 | 193 | |
| device PUBACK | 4 | 33 | 73 | |
| broker TCP ack | — | — | 40 | |
| **down message, delivered** | | | | **306 B** |
| `/up` ack PUBLISH (51 B payload, topic 17) | 74 | 103 | 143 | |
| broker PUBACK + device TCP ack | 4 | 33 | 113 | |
| **one ack (`shown` or `read`)** | | | | **256 B** |
| **down message fully acked (down + shown + read)** | | | | **818 B ≈ 0.82 kB** |
| `/up` reply PUBLISH (84 B payload) + PUBACK + ack | 107 | 136 | | **289 B ≈ 0.29 kB** |
| `/status` PUBLISH (117 B payload, topic 21) + PUBACK + ack | 145 | 174 | | **327 B ≈ 0.33 kB** |
| `/loc` periodic PUBLISH, QoS 0 (160 B payload, topic 18) + TCP ack; no PUBACK (v2) | 182 | 211 | | **291 B ≈ 0.29 kB** |
| `/loc` answering a `loc_req`, QoS 1 (160 B payload) + PUBACK + ack (v2) | 184 | 213 | | **366 B ≈ 0.37 kB** |
| one location request served (`/down` `loc_req` 78 B, acked, + QoS 1 `/loc` answer, v2) | | | | **651 B ≈ 0.65 kB** |
| keepalive PINGREQ + PINGRESP (+ TCP ack) | 4 | 62 | | **182 B ≈ 0.18 kB** |
| **TLS reconnect** (TCP handshake + full TLS 1.2 handshake with a 2-cert chain + MQTT CONNECT/CONNACK + SUBSCRIBE/SUBACK) | | | | **≈ 5 kB (estimate)** |

### 7.3 Monthly projection

Nominal school day: 20 down messages, 5 student replies, 4 reconnects, and (v2) a periodic fix
every 15 min plus 2 on-demand location requests.

```
20 down msgs fully acked  20 x 0.82 kB =  16.4 kB
 5 replies                 5 x 0.29 kB =   1.5 kB
48 keepalives             48 x 0.18 kB =   8.6 kB
34 status publishes       34 x 0.33 kB =  11.2 kB   (24 heartbeat + ~10 event-driven)
 4 reconnects              4 x 5.00 kB =  20.0 kB
                                        ---------
                                          57.7 kB/day  ->  1.69 MB / 30 days   (v1)
96 periodic /loc @15 min  96 x 0.29 kB =  27.8 kB                              (v2)
 2 location requests       2 x 0.65 kB =   1.3 kB                              (v2)
                                        ---------
                                          86.8 kB/day  ->  2.60 MB / 30 days   (v2)
```

Pessimistic day: 100 down messages, 20 replies, 24 reconnects (bad coverage), and (v2) a periodic
fix every 5 min plus 10 on-demand requests.

```
100 x 0.82 + 20 x 0.29 + 48 x 0.18 + 60 x 0.33 + 24 x 5.00
=  82.0 +  5.8 +  8.6 + 19.8 + 120.0  =  236 kB/day  ->  6.92 MB / 30 days   (v1)
+ 288 x 0.29 + 10 x 0.65 = 83.5 + 6.5  =  326 kB/day  ->  9.79 MB / 30 days   (v2)
```

**Verdict.** The 100 MB/month cap allows 3413 kB/day, i.e. ~39× the v2 nominal profile. MQTT+TLS
overhead is not a risk to the data constraint. The break-even point is ~4070 fully-acked
messages/day. *(v2 decision — location is added inside the existing budget, not by raising it.)*
Both profiles still satisfy HANDOFF.md §8's stricter "< 10 MB estimated monthly usage", but the
pessimistic v2 profile lands at **9.8 MB**, with essentially no margin, and the term that put it
there is the 5-minute periodic fix. **A periodic interval below ~5 min breaks HANDOFF.md §8's bar
on a bad-coverage day** — which is a second, independent reason (after GNSS power, §12 item 8) for
the interval to be device-chosen and long. `loc_period_s` = 0 returns both profiles to the v1
figures exactly.

**The dominant term is reconnects, not messages** — 20 of 58 kB nominal, 120 of 236 kB pessimistic.
This is the quantitative reason for HANDOFF.md's "never reconnect on a timer". **Phase 4 checked
whether `walter-modem` exposes TLS session resumption: it does not.** The entire public TLS surface
in v1.5.0 is `tlsWriteCredential()` (`src/WalterModem.h:4147`) and `tlsConfigProfile()` (`:4404`) —
profile id, validation level, TLS
version and three credential slot indices. There is no session-ticket or session-id parameter and no
resumption getter, so a reconnect is a full ~5 kB handshake every time and the ~40 % saving on the
pessimistic day is unavailable. This does not threaten the 100 MB cap (the pessimistic day is still
under 7 MB/month) but it does raise the energy cost of §8.3 (b), where each message would pay a full
handshake.

**SMS budget: 0 of 100 used.** SMS is out of scope (HANDOFF.md §1) **for the device**. Firmware
MUST NOT enable any SMS send or receive path. This line exists so a later phase cannot quietly
introduce one without editing this document.

*(v2 decision — the clarification the v2 SMS backend makes necessary.)* v2 adds SMS as a **server-
side delivery backend**: the relay hands a message to a third-party SMS provider over HTTPS, from
the server, to a human's phone. That traffic never touches this SIM, this modem or this budget, and
a message delivered to a user by SMS is still delivered to the *device* by MQTT exactly as
specified above. The 100-message SIM allowance stays at **0 used**, and the rule above is unchanged:
the day anything asks the modem to send an SMS, this document gets edited first.

### 7.4 Phase 6 measurement — validating the model

§7.3 is a theoretical count of JSON bytes plus assumed TLS/TCP framing overhead. Phase 6 adds one
real measurement to sanity-check it. **This does not replace §7.3** — it validates the
application-layer MQTT byte counts that §7.3's per-exchange table (§7.2) is built from; it does not,
and cannot, measure the TLS or LTE-M cellular framing §7.3 assumes for the real device↔broker link,
because there is no TLS and no cellular hop in this setup.

**What was measured.** `relay/docker-compose.yml`'s real `eclipse-mosquitto` broker and the real
relay container (no fake transport, no unit-test mocks), with one simulated device
(`paho-mqtt`, matching `tools/sim_device.py`'s shape) connected over plain TCP on the Docker
bridge network. Traffic sent, matching the "measurable, non-modem" slice of §7.3's nominal profile
per HANDOFF.md's Phase 6 scope: **20 down messages, each fully acked `shown` then `read`, plus 5
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
bytes sent in the run, so the comparison uses real payload sizes, not the HANDOFF.md example string.

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
| §7.2's worked example, same exchange, **with TLS**, HANDOFF.md's shorter example body | 818 B |
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
about it — leaves both profiles comfortably under the cap and under HANDOFF.md §8's stricter 10 MB
bar. **No change to §7.3's verdict is warranted by this measurement**; it is recorded here as
supporting evidence, not a revision.

---

## 8. ESP32 wake sources

**Rewritten in Phase 4** against `dptechnics/walter-modem` v1.5.0 (read at
`firmware/managed_components/dptechnics__walter-modem/`). Phase 1 assumed the modem could wake the
ESP32 from deep sleep on an incoming MQTT message and designed the whole sleep-mode power story
around it. The library does not support that, and the cost of the workaround is the single largest
correction in this document. RTC memory contents are defined in §9.

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

Consequence, unchanged from the first Phase 4 pass and if anything firmer on v1.5.0: **a `/down`
message that arrives while the ESP32 is in deep sleep is announced into a powered-down UART and is
then unreachable through the public API.** Deep sleep does not merely fail to wake us — it loses the
message, and it loses the `mid` needed to ever ask for it again.

The corollary that shapes the loop: **there is nothing to poll.** Phase 4's first draft described a
"poll cycle" calling `mqttDidRing()`; on v1.5.0 that is both deprecated and wrong for QoS 1. The
correct shape is a **wake-and-drain cycle** — the ESP32 wakes, the modem flushes the URC it was
holding while RTS was deasserted, the library's RX task parses it, the event task dispatches it, the
handler calls `mqttReceive()`. The ESP32 issues no speculative AT traffic at all. The energy model in
§8.2 is unaffected: what matters is the length of the awake window and how often it happens, not what
the ESP32 does inside it.

Two v1.5.0 features that Phase 1 did not know about and that the design should use:

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
| 2 | Button IO1 | `esp_sleep_enable_ext0_wakeup(IO1, 0)`, active low, RTC GPIO | Short press = mark read / open composer; long press = send reply | both | Yes — IO1 is an RTC GPIO (HANDOFF.md §1) | `mode`, `active_until`, msg ring, `pending_acks` (deep-sleep path only) |
| 3 | ~~RTC timer — keepalive~~ | — | **Deleted in Phase 4.** The MQTT client is in the modem and the library exposes no ping API; PINGREQ is the modem's job (§6.2). The status heartbeat rides an ordinary poll wake using a counter in RTC memory. | — | n/a | `status_pub_count` |
| 4 | RTC timer — active-mode exit | `esp_timer` while awake, not a sleep wake | 10 min with no button/keyboard activity → sleep mode | active | Yes | `mode`, `active_until` |
| 4b | MQTT event (no sleep involved) | `setMQTTEventHandler()` → `_eventProcessingTask` | `_MESSAGE` short-circuits the wake-and-drain latency while the ESP32 happens to be awake; `_DISCONNECTED` drives F3 recovery; `_MEMORY_FULL` flags a missed drain | both | Yes — v1.5.0 API | none (handler runs while awake) |
| 5 | ~~Modem URC / RI line into deep sleep~~ | `ext1` on the modem RX line | **Demoted in Phase 4: not supported by the library's public API; not pursued for the MVP.** See §8.3 for why, and for the one variant that could still work if someone wants the battery back. | — | n/a | — |
| 6 | LIS3DH INT1 (IO2) | `ext1` | Motion wake | — | **Out of scope for MVP.** Reserved only. | — |
| 7 | CardKB | — | **Cannot wake the ESP32.** No interrupt line to an RTC GPIO; polled at 100 ms only while the composer is open. A reply always starts with a button press. | active | Yes (by construction) | — |

**The GPIO question from Phase 1 is closed, and the answer is that there was never a question.** The
modem UART is board-fixed inside the component — RX 14, TX 48, RTS 21, CTS 47, RESET 45
(`src/WalterModem.cpp:82,87,92,97,102`, Kconfig-overridable but correct for Walter as shipped). None of those
collide with `firmware/main/pins.h` (1, 2, 8, 9, 10, 11, 12, 15, 16, 17, 18), and `begin()` takes
only a `uart_port_t`. No pin needs to be added to `pins.h` and **no GPIO reassignment is requested**.
For the record, GPIO14 and GPIO21 are both RTC-capable on the ESP32-S3 (RTC GPIOs are 0–21), so the
Phase 1 worry about a bodge wire was unfounded — the blocker is the library API, not the board.

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
I_light(T) = 1.0    + 40 x (0.050 / T)  =  1.0    + 2.0 / T   mA
I_deep(T)  = 0.0095 + 40 x (0.600 / T)  =  0.0095 + 24  / T   mA

crossover:  1.0 + 2/T = 0.0095 + 24/T  ->  0.99 = 22/T  ->  T = 22 s
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

### 8.3 Deep sleep: why it is not in the MVP, and the one way back

Two variants could make deep sleep safe, and both are blocked on something unverified:

- **(a) Hold RTS deasserted across deep sleep.** RTS is GPIO21, an RTC GPIO, so `rtc_gpio_hold_en()`
  plus `gpio_deep_sleep_hold_en()` can pin it high while the ESP32 is down. If the Sequans honours
  CTS and queues its URCs instead of dropping them, the URC is delivered intact after
  `WalterModem::begin()` releases the hold — which is exactly the trick the library already performs
  for light sleep (`src/WalterModem.cpp:4413-4450`). This fixes *correctness*, but it does not fix
  *energy*: the 600 ms re-init per cycle still applies, so the table above still says deep sleep
  loses at `T` ≤ 7.7 s. **Not worth pursuing at MVP latency.**
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

Phase 4 implements neither. It implements the light-sleep poll cycle, which is correct by
construction, needs no GPIO change and no unverified modem behaviour, and measures the result.

**HANDOFF.md §2's "do not silently degrade" clause is satisfied by this section.** The degradation
is real, it is roughly a 2× cut in idle battery life versus the Phase 1 estimate, and its cause is a
library limitation rather than a hardware one.

### 8.4 Sleep-mode current estimate

Vendor-documented where marked; everything else remains `(estimate)` pending a Phase 4 current trace.

| Term | Value | Assumption |
|---|---|---|
| ESP32-S3 light sleep, board level | **1.00 mA** *(vendor)* | `WalterModem::sleep()` doc comment; RAM retained, UART domain powered |
| Wake-and-drain overhead, `T` = 5 s | 0.40 mA (estimate) | 50 ms awake at 40 mA every 5 s (§8.2) |
| Modem eDRX paging | 0.2–0.5 mA (estimate) | Unchanged from Phase 1: 2 paging occasions per 20.48 s cycle, ~50 ms each at ~50 mA RX, plus warm-up |
| Modem idle floor | 0.01–0.05 mA (estimate) | Sequans GM02SP deep-sleep-between-paging |
| Keepalive, amortised | ~0.20 mA (estimate) | 48 PINGs/day × ~0.1 mAh (§6.2); modem-side only, the ESP32 no longer wakes for it |
| Display (gated off via IO15) | ~0 mA | e-paper VCC gated between refreshes |
| **Sleep-mode total** | **≈ 1.8–2.1 mA → 43–50 mAh/day** | On a ~1500 mAh LiFePO4 cell: **~30–35 days idle** |

> **Changed from Phase 1.** The previous estimate was 0.6–1.2 mA → 15–29 mAh/day → ~50–100 days,
> built on an ESP32 deep-sleep floor of 0.01–0.10 mA. That floor was only reachable with a
> modem-driven deep-sleep wake, which §8.0 shows the library cannot provide. The honest number is
> roughly **2× worse**. §8.3 (b) is the path back to something near the original figure.

Active mode, 10-minute window: the ESP32 light-sleeps at `T` = 2 s between drains rather than
spinning, so its contribution is `I_light(2)` ≈ **2.0 mA**, not the ~35 mA of a busy-wait. The modem
holding RRC/C-DRX dominates at an estimated 5–15 mA, giving **7–17 mA → 1.2–2.8 mAh per window**.
Six windows/day ≈ **7–17 mAh/day**, so active mode adds roughly 20–40 % on top of sleep mode rather
than the 100–200 % Phase 1 projected. Carve-out: while the reply composer is open the CardKB needs
100 ms polling, so the device stays fully awake for that window — a Phase 5 concern, but budget for
it here.

Firmware MUST count `WALTER_MODEM_MQTT_EVENT_MEMORY_FULL` events and publish the counter in the
`/status` heartbeat if it is ever non-zero: it is the only direct evidence that the wake-and-drain
cycle is losing messages, and it is cheap.

Phase 4 should still measure whether idle-mode eDRX at 5.12 s meets the <5 s target; if it does it
is likely several× cheaper than holding RRC. That is a measurement to take, **not** a change to
HANDOFF.md §2's architecture.

## 9. Storage contract: RTC memory vs. ordinary RAM

**Rewritten in Phase 5.** Phase 1 put the whole message store in RTC memory. Phase 4 measured that
this does not fit (the vendor library takes 7003 of the 8192 bytes) and shipped a stopgap that
linked but truncated every body to 48 bytes. Phase 5 resolves it by **splitting the store**: RTC
memory holds only what must survive a reset to keep §4's delivery guarantees honest; the bodies and
the thread history live in ordinary static RAM, which the MVP's light-sleep cycle (§8) retains.

### 9.1 The measured budget

ESP32-S3 RTC slow memory is **8192 B** (`rtc_slow_seg`, `0x50000000`, length `0x2000`). All figures
below are read from `firmware/build/school_pager.map` after a clean
`idf.py set-target esp32s3 && idf.py build` on `espressif/idf:release-v5.2`, re-confirmed in Phase 5:

| Consumer | Bytes | Map symbol |
|---|---|---|
| `walter-modem` v1.5.0 (`_pdpCtxSetRTC`, `_mqttTopicSetRTC`, `_socketCtxSetRTC`, `_coapCtxSetRTC`, `blueCherryRTC`) | **7003** | `.rtc.data.0-.4` under `WalterModem.cpp.obj` (`0xc80 + 0x910 + 0x3c0 + 0x208 + 0x3`) |
| IDF/linker remainder | 5 | `_rtc_slow_length` (`0x1f58`) minus `.rtc.data` (`0x1f54`) |
| **Available to the pager** | **1184** | `0x2000 - 0x1f58 + sizeof(g_rtc)` |

Reclaiming the library's 7003 B is not an option: `CONFIG_WALTER_MODEM_ENABLE_SOCKETS`/`_HTTP`/
`_COAP`/`_BLUECHERRY` each **fail to build the vendor library itself** (unguarded references in
`WalterBlueCherry.cpp` and `_dispatchEvent()`), and HANDOFF.md §7.7 forbids patching a managed
component. See `sdkconfig.defaults` for the full finding.

### 9.2 The split, and the rule that decides it

> **Rule.** A datum lives in RTC memory if losing it to a reset would make the relay's view of
> delivery *wrong* — i.e. the relay believes a message was delivered or a reply was sent when the
> student will never see it / never sent it. Everything else lives in RAM.

Under that rule three things are RTC-resident: the **dedup digest ring** (§4.1 rule 7 — without it a
post-reset redelivery storm costs a full active-mode window per duplicate), the **pending ack queue**
(§4.1 rule 6), and the **pending reply queue** (§4.2). One more is RTC-resident by judgement rather
than by rule: the **newest unread down message**, because §5.3 only re-publishes messages in state
`queued`/`sent` — a message already acked `shown` but not yet `read` is unrecoverable from the relay,
so losing it means the student never sees "pickup at 3:15" and nobody finds out.

### 9.3 RTC-resident layout (`pager_rtc_t`, owned by `modes.c`)

| Field | Bytes | Purpose |
|---|---|---|
| `magic` + `crc32` | 8 | Validity check. On mismatch, treat as cold boot: regenerate `session_id`, clear everything. **Bump the layout digit in `magic` whenever this table changes** — the Phase 4 layout is not compatible with this one. |
| `boot_count` | 4 | Diagnostics; distinguishes cold boot from reset recovery |
| `session_id[12]` | 12 | §1; regenerated **only** on cold boot |
| `mode`, `active_until_epoch` | 16 | Mode state machine |
| `status_pub_count`, `last_status_epoch` | 16 | §5.4 heartbeat cadence |
| `mqtt_memfull_count`, `oversize_drop_count`, `modem_resets`, `last_modem_reset_us`, `attach_fail_cycles`, `wake_cycle_count` | 32 | §8.4 M6, §3.4, F4/F1 counters |
| `msg.seen_ids[16]` (`uint32_t`) + `seen_head` | 68 | Dedup ring (§4.1 rule 7) — **a 32-bit digest of the id, not the id string.** Message ids carry 32 bits of entropy by construction (§1), so the digest is the id's own randomness; 16 entries collide with probability ≈3×10⁻⁸, and the only consequence of a collision is one suppressed render. Costs 68 B where the literal strings cost 276 B. `id_hash == 0` means "empty slot"; a real digest of 0 is stored as 1. |
| `msg.pending_acks[8]` (`id[17]`, `state`, `attempts`) | 152 | Ack retry queue (§4.1 rule 6). Ids are stored in full — the ack has to put the real id on the wire. |
| `msg.pending_up[2]` (`created_epoch`, `id[17]`, `body[161]`, `body_len`, `attempts`, `in_use`) | 384 | Unsent student replies (§4.2), **full fidelity, never truncated** — see §9.4 |
| `msg.unread[1]` (`ts`, `id[17]`, `from[17]`, `body[161]`, `body_len`, `flags`, `in_use`) | 208 | Newest down message acked `shown` but not yet `read` (§9.2) |
| `msg.dedup_hits`, `msg.malformed_drops`, `msg.reply_failed` | 12 | Diagnostics for the `/status` heartbeat |
| **Total** | **≈ 920 of 1184** | ~264 B headroom, against Phase 4's 168 B |

`modes.c` remains the sole owner of the struct, its single `magic`/`crc32` pair and `rtc_save()`
(§11's "one transition funnel" discipline applies to RTC writes too). `msg.c` receives a typed
pointer to the nested `msg` sub-struct and calls back into `modes.c` to re-CRC. `modes_boot()` MUST
log `sizeof(pager_rtc_t)` at boot and MUST `_Static_assert(sizeof(pager_rtc_t) <= 1184)`.

### 9.4 Why 161 bytes is full fidelity for a reply, and lossy only for a parent message

§3.1 caps a body at 160 Unicode code points **and** 320 UTF-8 bytes. 320 is the worst case for
non-ASCII text; 161 B (160 + NUL) is the exact requirement for ASCII.

- **`pending_up` (student reply):** the only input device is the CardKB (HANDOFF.md §1), which emits
  one byte per keypress and has no IME. A composed reply is ASCII by construction, so 160 bytes is
  160 characters and nothing is ever lost. The composer MUST therefore **refuse input past 160
  bytes** rather than truncate at send time; `msg_queue_reply()` MUST reject a longer body with an
  error, never truncate. A truncated reply on the wire would be a silent correctness failure and is
  the one case this design refuses to accept.
- **`unread` (parent message):** a parent typing on a browser can emit up to 320 UTF-8 bytes. The RTC
  mirror is truncated at a **UTF-8 code-point boundary** to ≤160 bytes and flagged
  `MSG_F_TRUNCATED`. This is lossy only on the reset path: in normal operation the UI renders from
  the RAM copy, which holds the full 320 bytes. After a reset the student sees the message with a
  trailing ellipsis rather than not seeing it at all.

### 9.5 RAM-resident store (`msg.c`, ordinary `.bss`)

| Field | Bytes | Purpose |
|---|---|---|
| `s_thread[10]` — `msg_t` = `ts`, `id[17]`, `from[17]`, `body[321]`, `body_len`, `dir`, `ack_state`, `flags`, `in_use` | 3760 | Thread history, both directions, full 320-byte bodies. **10 entries**, matching HANDOFF.md §5 and §5.3's 10-per-online-edge re-publish cap. |
| `s_composer[161]` + cursor/length | 168 | In-progress reply text (§9.4) |
| `ui.c` frame buffers: new plane + shadow (old) plane, 16 B/row × 296 rows each | 9472 | SSD1680 differential partial refresh needs both planes; see §9.6 |

≈ 13.4 kB of the ESP32-S3's ~512 kB SRAM. RAM is not the scarce resource here and firmware-dev
should not optimise this; the scarce resource is the 1184 B above.

### 9.6 What survives what

| Event | RTC struct | `.bss` (thread, composer, frame buffers) | Modem TLS+MQTT session |
|---|---|---|---|
| Light-sleep wake (every 2 s / 5 s — the everyday case, §8) | survives | **survives** | survives |
| `esp_restart()`, watchdog reset, panic/crash | survives | **lost** | survives (modem is never power-gated, §6.4) |
| Brownout, EN reset, battery removal, first power-on | **lost** (CRC fails → cold boot) | lost | lost |

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

> **Optional follow-up, not adopted in the MVP (relay-side change, needs an owner).** Widening
> §5.3's re-publish selection from `state in (queued, sent)` to
> `state in (queued, sent, shown) AND age < 2 h` on a **session change only** would let the relay
> recover *all* unread messages after a cold boot, not just the ones that never reached `shown`, and
> would make `msg.unread[]` redundant rather than merely shallow. It costs nothing in the common
> case (dedup suppresses re-render when RTC survived; a session change only happens on cold boot).
> Not done in Phase 5 because it changes Phase 2 relay code and §5.3's contract. Raised in §12.

### 9.7 Standing rules

- Firmware MUST NOT assume the modem's MQTT session state is mirrored in RTC memory. On every wake
  it re-reads the modem's actual connection state and drains the URC the modem was holding (§8.0).
- **Do not shrink `seen_ids`** below 16 entries — see the duplicate-storm cost in §4.1 rule 7.
- Keeping this layout in RTC rather than moving all of it to RAM still buys the two things §8.3(b)
  needs: if the clean-session experiment (§12 item 4) passes and deep sleep returns, the durability
  half of the store already has the right shape and the change is a sleep-call swap, not a data-model
  rewrite.
---

## 10. Path to CBOR (documented, not designed)

CBOR is **not** implemented in the MVP. The migration principles the MVP must preserve:

1. **Short keys.** Every field name is ≤4 ASCII characters (`v`, `id`, `ts`, `from`, `body`, `ack`)
   so each maps 1:1 to a CBOR text key with no renaming, and later to a fixed integer keymap:
   `v=0, id=1, ts=2, from=3, body=4, ack=5`. New fields (§11) MUST also be ≤4 characters and MUST
   claim the next free integer in this table when they are added.
   *(v2 decision — the v2 fields claim their integers now, while the table is still cheap to
   extend.)* Envelope keys: `kind=6, to=7`. `/loc` keys (§13): `loc=8, req=9, cached=10, err=11`,
   and inside the `loc` object a separate sub-keymap `lat=0, lon=1, acc=2, fix_ts=3, src=4`.
   **Two v2 names break the ≤4-character rule: `cached` (6) and `fix_ts` (6).** They are kept
   because legibility on a wire nobody has to hand-decode is worth more than two bytes each on a
   ≤200-byte envelope that is itself under 6 % of the data budget, and because the integer keymap
   above removes the cost entirely the day CBOR is implemented. The rule still binds for the
   `/down` and `/up` envelopes, where the device's static buffers live.
2. **Self-describing framing.** A JSON payload always begins with `0x7B` (`{`); a CBOR map always
   begins with `0xA0`–`0xBF` or `0xBF`. A receiver can therefore dispatch on the **first byte** with
   no negotiation, no new topic and no flag day. *(Phase 1 decision — the alternative, a separate
   `/down-cbor` topic, doubles subscription count and broker ACL surface for no benefit.)*
3. **Version field.** `v` distinguishes semantic changes from encoding changes. Encoding change
   alone does **not** bump `v`; a field-meaning change does. `v:1` is the only defined value.
4. **Ignore-unknown.** §3.1's rule means a v1 receiver survives a v1-plus-extras publisher.
5. Expected saving: the 99-byte down message becomes roughly 60–70 bytes with text keys and roughly
   45–55 bytes with integer keys (estimate). At the nominal profile in §7.3 that saves ~1 kB/day of
   58 kB/day — **under 2 %**. CBOR is therefore not justified by the data budget; it would be
   justified only by device-side parse cost or RTC memory pressure. Do not implement it without a
   measurement that names which of those it is fixing.

---

## 11. Future extension points (do not design now)

These are reservations only. MVP code MUST NOT implement, subscribe to, or emit any of them.

**Topic namespace**
| Reserved topic | Intended use | Notes |
|---|---|---|
| ~~`pager/{id}/loc`~~ | ~~GNSS fixes, device → relay~~ | **Spent in v2 — this is now a live topic, specified in §13.** QoS ended up 1-when-answering / 0-when-periodic rather than "QoS 0 likely". |
| `pager/{id}/cfg` | relay → device config (mode schedule, geofences) | Retained=true so a waking device gets current config with no request |
| `pager/{id}/evt` | geofence enter/exit, motion, low-battery alerts | Separate from `/up` so the parent thread stays human messages only |

**Schema slots** — reserved field names, not to be reused for anything else:
~~`loc` (lat/lon/acc)~~ — **spent in v2, see §13**, along with `kind`, `to`, `req`, `cached`, `err`,
`loc_period_s` and `loc_min_s`, which are now defined fields and not reservations —
`prio` (priority / alert level), `exp` (message TTL, would supersede §4's fixed 24 h `expired`
sweep), `sched` (mode schedule id).

**Enum headroom** — `from` already allows `system`, which is where geofence and low-battery
notifications will render in the thread without a schema change; v2 widened the rest of `from` to
an alias (§3.1), which is an enum removal rather than an addition, but `system` is preserved
verbatim precisely so this reservation still holds. `kind` is likewise a string enum with exactly
two defined values, so `/evt`-style kinds can be added to `/down` later under §3.4's
unknown-`kind`-is-dropped rule. `mode` is a string, not a boolean,
specifically so `school` / `travel` / `night` can be added later; every consumer MUST treat an
unknown `mode` value as `sleep` for display purposes rather than erroring.

**Mode state machine stub** — MVP has exactly two modes and three transitions (HANDOFF.md §2): boot
→ sleep; sleep → active on incoming message or button; active → sleep after 10 min idle. Later
schedule- or geofence-driven switching enters at the same `set_mode()` edge, driven by `/cfg` and
`/evt`, so `modes.c` MUST funnel every transition through one function with a reason code rather
than setting the mode variable in-line. That is the only structural requirement the MVP owes the
future.

---

## 12. Open questions for the human

1. **~~`NEEDS HUMAN DECISION` — modem wake line GPIO.~~ CLOSED in Phase 4 — no decision needed.**
   The question was whether the modem's ring-indicator / UART RX line is routed to an RTC-capable
   GPIO, and whether enabling deep-sleep wake needs a bodge wire. It does not: the modem UART is
   board-fixed inside the component at RX 14 / TX 48 / RTS 21 / CTS 47 / RESET 45
   (`src/WalterModem.cpp:82,87,92,97,102`), GPIO14 and GPIO21 are both RTC-capable on the ESP32-S3,
   and nothing collides with `pins.h`. **No GPIO change is requested and none is needed.**
   What replaced it is not a hardware question but a library one, and it is not a decision for the
   human either — it is an experiment (item 4 below). See §8.0: the library discards
   incoming-message state across an ESP32 restart, so deep sleep loses messages regardless of how we
   wake. The documented consequence is §8.4's revised estimate: **≈1.8–2.1 mA / 43–50 mAh/day, about
   2× worse than Phase 1's 0.6–1.2 mA**, i.e. ~30–35 days of idle life instead of ~50–100.

2. **`NEEDS HUMAN DECISION` — broker free-tier limits.** Unchanged from Phase 1. This contract needs,
   per device: QoS 1 both directions, a retained `/status`, an LWT, `cleanSession=false` with a
   persistent session that outlives a coverage gap, and a 1800 s keepalive. If the chosen free tier
   caps session expiry below a useful window, caps keepalive below 1800 s, or limits
   credentials/connections such that a second device needs a paid plan, that is a paid-service
   decision. Phase 4 adds a wrinkle: two of those five properties (LWT, clean session) may be
   unreachable from the device side regardless of what the broker allows — see item 3.

3. **`NEEDS HUMAN DECISION` — LWT and clean session are not settable from `walter-modem` v1.5.0.**
   §5.2's LWT contract and §6.1's `cleanSession=false` both assumed an API the library does not
   expose (`mqttConfig()` stops after the TLS profile id; `mqttConnect()` has no session flag; a grep
   of `src/` for `will`/`lastwill`/`clean_session` returns nothing). The options, cheapest first:
   **(a)** send a raw `AT+SQNSMQTTCFG` with the will parameters via the public `sendCmd()` before
   connecting — free if the Sequans AT manual supports it, needs the manual to confirm;
   **(b)** upgrade the component if a future release exposes them; **(c)** drop the LWT and have the
   relay infer offline from keepalive expiry, which costs up to 1800 s of staleness in the parent UI;
   **(d)** patch the managed component, which HANDOFF.md §7.7's dependency rule argues against.
   Someone should pick between (a)-(c). MVP default if nobody picks: (a) attempted, (c) as the
   fallback, and §5.2 is then advisory rather than binding.

4. **Not a decision, but the highest-value experiment in this document.** Settle whether the modem's
   MQTT session is persistent across an ESP32 restart (§6.1 clean session). If it is, §8.3 (b)
   becomes implementable and idle battery life goes from ~30–35 days back to roughly 80–120 days.
   ~20 minutes on hardware. Nobody should redesign anything for it until it has been run.

5. **Not a decision, but needs an owner:** per-device MQTT credentials and the broker CA must be
   provisioned to the device at flash time and never committed (HANDOFF.md §7.5). The device-side
   mechanism is settled — `tlsWriteCredential()` into modem NVRAM certificate slot ≥ 11, once per
   device, persistent across reboots (§6.1) — but no tooling for getting the per-device credentials
   *to* that call exists in the repo yet. NVS plus a flash-time provisioning step is the obvious
   shape; nobody owns it.

6. **Resolved post-MVP-review — no dedicated battery-voltage sense pin, but option (b) exists and
   is now wired in.** §5.1's `/status` schema requires `batt_mv`, and HANDOFF.md's hardware table
   (§1) does not list an ADC-capable GPIO wired to the battery for that purpose — option (a), a
   dedicated sense pin + resistor divider, would still need a GPIO change and a human decision per
   HANDOFF.md §7.7, and is **not** pursued.

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
   change?** Phase 5's §9.6 note. Today a down message that reached `shown` and then died in a
   crash is unrecoverable: §5.3 only re-publishes `queued`/`sent`, and the device only keeps the
   **newest** unread message in RTC memory (`msg.unread[1]`, 208 B — a second slot does not fit in
   the 1184 B §9.1 measures). Widening §5.3's selection to
   `state in (queued, sent, shown) AND age < 2 h`, **on a session change only**, closes the gap for
   free: dedup suppresses re-render whenever RTC survived, and a session change only happens on a
   cold boot, when dedup is empty anyway. It is a Phase 2 relay change plus a §5.3 edit, so Phase 5
   did not make it unilaterally. If nobody picks this up, the documented behaviour stands: **a
   crash can lose all but the most recent unread message.**

8. **`UNVERIFIED`, and it must be measured before the first periodic interval is chosen — GNSS
   power (v2 decision — §13 deliberately leaves the interval to the device, so this measurement is
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

> **Retracted in Phase 4 review.** An earlier revision of this section carried a fifth
> `NEEDS HUMAN DECISION` claiming `tlsWriteCredential()` was private and that application code
> therefore could not provision a CA, forcing a choice between unauthenticated TLS and a separate
> provisioning firmware. **That was wrong** — it was read against v1.2.0, where the symbol was
> briefly private. In v1.5.0 it is public (`src/WalterModem.h:4147`, inside the `public:` block that
> starts at `:4132`) and is demonstrated in the vendor's own `examples/mqtts`. There is no decision
> to make: the Phase 1 TLS provisioning flow works as originally specified. The episode is recorded
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
| 8.3 | Sequans queues URCs rather than dropping them when CTS is deasserted | **OPEN, and the whole MVP sleep design rests on it** | Awake ESP32, drive RTS high manually, send a message, wait 30 s, drop RTS, see whether the URC arrives |
| 8.4 | All current figures except the two vendor ones | OPEN | Phase 4 current-trace measurement; record in `firmware/README.md` |
| 6.5 | SSD1680 partial refresh completes in <1.5 s at room temperature | **OPEN (M7)** — §6.5 breaks if it does not; only 2.7 s of slack at `T`=5 s | Toggle a GPIO around `ui_refresh()`, scope the pulse width; also log `esp_timer` deltas around the BUSY wait |
| 9.4 | The M5Stack CardKB emits one ASCII byte per keypress (no multi-byte sequences) | **OPEN, load-bearing for §9.4** — if it can emit >0x7F, a 160-byte RTC reply slot is no longer 160 characters | Poll 0x5F over I2C, dump every non-zero byte for a full pass over the keyboard incl. Fn/sym combos; 15 min on hardware |
| 8.4 | ESP32 draws ~40 mA awake and ~50 ms per wake-and-drain cycle | OPEN — the overhead term (0.40 mA of 1.8–2.1 mA) rests entirely on this, and the 50 ms floor is set by the library event task's 10 ms tick + 10 ms settle | Toggle a GPIO around the awake window and read the duty cycle on a scope; a current trace gives both numbers at once |
| 12.6 | `getVoltage()`/`AT+SQNVMON` reports real battery voltage, not a fixed regulated rail | **OPEN, cheap to confirm** — inferred from Walter's public schematics (§12 item 6), not from Walter's own unpublished internal routing | Compare `getVoltage()`'s reported `batt_mv` against a multimeter reading of the actual battery on first hardware bring-up; 5 min, no code change either way |
| 13 | Energy per GNSS fix (time-to-fix and mean current) — **no figure exists**, and §13's periodic interval cannot be chosen without one | **OPEN (§12 item 8), load-bearing for the v2 battery budget** | Trigger a fix inside §8.4's current trace; log time-to-fix cold and warm, mean current, mAh per fix |
| 13 | `walter-modem` v1.5.0 exposes a usable GNSS fix API with a bounded timeout and assistance data | **OPEN** — assumed by §13, not yet read against the component source the way §6/§8 were | Grep `src/` for the GNSS/GPS API and read it, as Phase 4 did for MQTT; 30 min, no hardware |

---

## 13. Location (v2)

**Added in v2.** Spends the `pager/{id}/loc` topic and the `loc` schema slot that §11 reserved.
Everything in this section is additive: a device that never publishes `/loc` and never parses
`kind` (§3.2) is still conformant, it simply has no location feature.

*(v2 decision — location is a separate topic rather than a field on `/up`, because a fix is not a
thread entry: it has a different lifetime, a different QoS, a different retention class and a
different read permission from a human message, and folding it into `/up` would put all four of
those decisions inside one parser.)*

### 13.1 Topic

`pager/{device_id}/loc`, **device → relay**, retained **false**.
QoS **1** when `req` is non-null, QoS **0** otherwise (§2's table carries the reason).
Broker ACL: the device credential may **publish** this topic and nothing more; it MUST NOT be
subscribable by the device. The relay receives it the same way it receives `/up` and `/status` —
in v2, a broker rule forwards it to the relay's authenticated HTTPS endpoint (§2).

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

The example is ~160 bytes; the worst case is ≤ ~200 bytes, so §3.3's 640-byte limit applies
unchanged and is nowhere near binding. A `/loc` payload that violates any rule above is malformed
and is handled per §3.4 — logged and dropped, never crashing the ingest path.

Relay-side: dedup on `id` exactly as §4.2 dedups an up message, in the same transaction that
stores the fix. A periodic fix is **not** a thread entry; it updates the device's last-known
position. A fix answering a `req` additionally resolves that request (§13.4).

### 13.3 Device-side rate limit — **normative**

*(v2 decision — written normatively here, not left to the implementation, so that firmware, the
Python test client and the relay's mirrored limit agree on one set of numbers. A limit that the
device and the server disagree about is a limit that produces phantom `expired` requests.)*

1. A `loc_req` arriving **less than `loc_min_s`** (default **120 s**) after the last fix *attempt*
   is answered **immediately** from the last fix, with `cached:true`, **without powering GNSS**.
   The window runs from the last *attempt*, not the last success, so a device in a basement cannot
   be made to retry continuously by a user pressing a button.
2. Otherwise the device attempts a fix, **bounded by 60 s**, and then answers: the fix if it got
   one, otherwise `loc:null, err:"no_fix"`. The device always answers a `loc_req` it accepted;
   silence is reserved for firmware that does not implement `kind` at all (§3.2).
3. **At most one fix attempt is in flight.** A second `loc_req` arriving during an attempt does not
   start another one; it is answered by the same result, as a separate `/loc` publish with its own
   `req`.
4. `loc_min_s` and the periodic interval `loc_period_s` are reported in `/status` (§5.1) for
   display. They are the **device's** choices — see §12 item 8 for why.

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
bad-coverage day breaks HANDOFF.md §8's 10 MB bar. `loc_period_s` = 0 disables periodic location
entirely and is a valid, fully conformant configuration; on-demand `loc_req` still works.
