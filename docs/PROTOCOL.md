# PROTOCOL.md — School Pickup Pager wire contract (MVP)

**Status:** authoritative. Phase 2–6 (relay, simulator, firmware `net.c`/`modes.c`/`msg.c`, parent UI)
MUST conform to this document. Per HANDOFF.md §7.6, any topic or schema change edits this file
*first*, then code.

**Scope:** MVP = text relay only. GPS, geofences, schedule-based mode switching and SMS fallback
are out of scope (HANDOFF.md §1). §11 records where the contract leaves room for them.

**Conventions used below**
- `(Phase 1 decision — see rationale)` = not fixed by HANDOFF.md; decided here, with a one-line reason.
- `NEEDS HUMAN DECISION` = would require a paid service, a firmware dependency beyond
  `walter-modem` / `esp_timer` / a display driver, or a GPIO change. Not decided here. Collected in §12.
- `UNVERIFIED` = a hardware or vendor-library fact this document assumes but cannot confirm; each
  one names the cheapest experiment that settles it.
- Power numbers are labelled `(estimate)` or `(TBD — hardware measurement)`. No number in this
  document is a measured number yet; Phase 4 replaces the estimates with measurements.

---

## 1. Identifiers

| Thing | Format | Max len | Notes |
|---|---|---|---|
| `device_id` | `^[a-z0-9][a-z0-9-]{2,23}$` | 24 | lowercase; example `pgr-0001` *(Phase 1 decision — bounds max topic length to 37 bytes so device-side topic buffers are static)* |
| MQTT client id (device) | exactly `device_id` | 24 | *(Phase 1 decision — stable across reboots is required for `cleanSession=false` session resumption; see §6)* |
| MQTT client id (relay) | `relay-1` | — | *(Phase 1 decision — fixed id so the relay also resumes a persistent session and does not miss `/up` while restarting)* |
| Message id (relay-originated, down) | `m_` + 8 lowercase hex | 10 | 32 bits of `os.urandom`; UNIQUE in SQLite, regenerate on collision |
| Message id (device-originated, up) | `u_` + 8 lowercase hex | 10 | from `esp_random()` |
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

- The device subscribes to **exactly one** topic: `pager/{own_id}/down`, QoS 1. No wildcards on the
  device *(Phase 1 decision — a wildcard subscription on a metered link is an unbounded data risk)*.
- The relay subscribes to `pager/+/up` and `pager/+/status`, both QoS 1.
- Broker ACLs: device credentials may publish **only** to `pager/{own_id}/up` and
  `pager/{own_id}/status`, and subscribe **only** to `pager/{own_id}/down`. The `relay` credential
  gets the mirror image. Enforced at the broker, not just in code.
- Reserved-but-unused in MVP: `pager/{device_id}/loc`, `/cfg`, `/evt` (§11). Devices MUST NOT
  subscribe to them in MVP.

---

## 3. Message schema (JSON, MVP)

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
| `from` | string | yes on content messages, **absent** on acks | `parent` \| `student` \| `system`, ≤16 chars | Author. |
| `body` | string | yes on content messages, **absent** on acks | ≤ **160 Unicode code points** (fixed) **and** ≤ **320 UTF-8 bytes** *(Phase 1 decision — the code-point cap alone allows 640 bytes; the byte cap lets firmware size static buffers and the RTC ring at §9)* | Message text. |
| `ack` | string \| null | yes; `null` on content messages | `shown` \| `read` | Ack state being reported. |

Additional rules:
- `body` MUST NOT contain Unicode control characters `U+0000`–`U+001F` or `U+007F`. Relay strips
  them on ingest from the parent API and rejects if the result is empty. *(Phase 1 decision — keeps
  JSON escaping bounded, and the e-paper renderer has no control-char handling.)*
- `body` MUST NOT be empty on a content message.
- **Unknown fields MUST be ignored, not rejected.** This is the forward-compatibility rule that
  makes §11 additive.
- Field order is unspecified; a receiver MUST NOT depend on it. Publishers SHOULD emit
  `v,id,ts,from,body,ack` in that order to keep logs diffable.

### 3.2 Message kinds

| Kind | Topic | Shape |
|---|---|---|
| Down message (parent → student) | `/down` | `{"v":1,"id":"m_7f3a","ts":…,"from":"parent","body":"…","ack":null}` — **99 bytes** for the example above |
| Ack (device → relay) | `/up` | `{"v":1,"id":"m_7f3a","ts":…,"ack":"shown"}` — **51 bytes**; no `from`, no `body` |
| Up message (student reply) | `/up` | `{"v":1,"id":"u_91c0","ts":…,"from":"student","body":"ok coming","ack":null}` — **84 bytes** |

A receiver distinguishes an ack from a content message by `ack !== null`. A payload with both a
non-null `ack` and a non-empty `body` is **malformed** (§3.4).

### 3.3 Envelope size limit

Worst case, with every field at its maximum and JSON escaping expanding `"` and `\` to two bytes:

```
{}                                   2
"v":1,                               6
"id":"<=16>",                       24
"ts":1757700000,                    16
"from":"<=16>",                     26
"body":"<=480 escaped>",           490   (320 UTF-8 bytes, up to 160 of them escaped 1->2)
"ack":"<=8>"                        16
                                  ----
                                   580
```

**Hard limit: 640 bytes.** Any payload larger than 640 bytes on any topic MUST be dropped
unparsed by both relay and device *(Phase 1 decision — 580 rounded up with headroom to a size the
firmware can statically allocate; a fixed limit means the device never mallocs on the RX path)*.

### 3.4 Malformed payload handling

A payload is malformed if it is >640 bytes, not valid UTF-8, not a JSON object, missing `id`/`ts`/
`ack`, has an out-of-range field, violates a `body` rule, or sets both `ack` and `body`.

- **Device:** log, increment a counter, **do not ack**, do not render, do not reboot. The message
  stays `sent` at the relay and the parent UI shows it as undelivered.
- **Relay:** log with the topic and first 64 bytes, drop. Never crash the MQTT loop on a parse
  error — one bad payload must not take down the persistent session. Never auto-reply on MQTT.
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
             relay stores          broker PUBACK        device /up ack       device /up ack
 (HTTP POST) ------------> queued --------------> sent --------------> shown -------------> read
                             |                      |
                             |  24 h, no ack        |  24 h, no ack
                             +----------------------+--------> expired  (terminal, relay-only)
```

| State | Owner | Entered when | Notes |
|---|---|---|---|
| `queued` | relay | row committed to SQLite by `POST /api/devices/{id}/messages` | Also the state of a message whose publish attempt failed (broker down). |
| `sent` | relay | broker returns **PUBACK** for the QoS 1 `/down` publish | Means *the broker accepted it*, **not** that the device received it. The UI must not say "delivered" here. |
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
7. **Device-side dedup.** The device keeps the last **16** received message ids in RTC memory. A
   duplicate delivery is **re-acked** (relay is idempotent) but MUST NOT re-render, re-alert or
   re-enter active mode. *(Phase 1 decision — prevents a QoS 1 redelivery storm after a reconnect
   from costing a full active-mode window per duplicate, which is the single most expensive
   failure mode in the power budget: ~2–3 mAh per spurious 10-minute window, estimate.)*

### 4.2 Up messages (student replies)

**Up messages have no relay-side lifecycle beyond storage, and the relay sends no ack back to the
device** *(Phase 1 decision — an ack-of-reply would cost one extra down message plus one extra
radio wake per reply for information the device already has from its own PUBACK; the parent page is
the endpoint that matters)*.

Device-side only, in RTC memory:

```
compose --> pending --(modem PUBACK)--> done            (entry freed)
              |
              +--(3 failed attempts, or >2 h old)--> failed  (UI shows "not sent", entry freed)
```

Relay-side an up message is simply inserted with `direction='up'` and served by
`GET /api/devices/{id}/messages`. An up message whose `id` already exists is a duplicate and is
dropped (QoS 1 redelivery).

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
  - Cap: **at most 10 messages per online edge** *(Phase 1 decision — matches the device's 10-entry
    RTC ring from HANDOFF.md §5 Phase 5, and bounds the reconnect burst to ~8 kB / ~1 active-mode
    window; older unacked messages are left for the `expired` sweep)*.
  - Re-publish reuses the **same `id`** so device dedup (§4.1 rule 7) suppresses double-rendering.
- The relay MUST NOT publish anything to `/down` on a timer for liveness. There is no application
  ping. MQTT keepalive is the only liveness mechanism (§6).

### 5.4 Device publish cadence

Status is published: (a) immediately after MQTT connect, (b) on every mode change, (c) when
`batt_mv` has moved more than 50 mV since the last publish, and (d) as a heartbeat on the keepalive
wake, at most **once per 3600 s**.

*(Phase 1 decision — the heartbeat period is set to exactly 2× the MQTT keepalive interval (§6) so
it is always piggybacked on a wake the device was making anyway. Marginal cost ≈ 0.33 kB of data and
≈ 0 extra radio sessions. An independent hourly status timer would add ~24 radio wakes/day ≈
2.4 mAh/day (estimate) against a sleep budget of roughly 20 mAh/day — a ~12 % battery tax for
nothing.)*

Status is **never** published on a plain paging wake or on receipt of a down message.

---

## 6. Session, keepalive and eDRX

### 6.1 Session

| Parameter | Value | Rationale |
|---|---|---|
| MQTT version | **3.1.1** | *(Phase 1 decision — the Sequans in-modem MQTT client is assumed 3.1.1; MQTT 5 features are not relied on anywhere in this document.)* UNVERIFIED: whether `walter-modem` exposes MQTT 5. Experiment: read the component's public header for a version/`sessionExpiry` parameter — 10 minutes, no hardware. |
| Clean session | **false** | Fixed in spirit by HANDOFF.md's "one persistent session". With `cleanSession=false` and a stable client id, the broker queues QoS 1 `/down` messages while the TCP link is briefly down, so a coverage gap does not lose a message. |
| TLS | server-authenticated, CA pinned in modem NVM; username/password per device | Free-tier HiveMQ Cloud model. |
| Reconnect policy | **Only** on detected session loss. Never on a timer (HANDOFF.md §1). Backoff 5 s, 15 s, 60 s, 300 s, then 300 s steady. | Each reconnect costs a full TLS handshake ≈ 5 kB (§7) — reconnects are the largest single term in the data budget. |

### 6.2 Keepalive

| Parameter | Value |
|---|---|
| MQTT keepalive | **1800 s (30 min)** *(Phase 1 decision — see arithmetic below)* |
| ESP32 RTC-timer wake for keepalive | 1500 s (25 min), i.e. 0.83 × keepalive, **only if** the modem does not PINGREQ autonomously (UNVERIFIED, §8) |
| Server Keep Alive override | If CONNACK carries one (MQTT 5) or the broker documents a lower cap, the **smaller** value wins and the RTC wake period is recomputed as 0.83 × it |

Arithmetic behind 1800 s: a PINGREQ/PINGRESP pair costs ~0.18 kB of data (negligible) but requires
an RRC connection — roughly 3 s at ~120 mA ≈ 0.1 mAh (estimate). At 1800 s that is 48 pings/day
≈ **4.8 mAh/day**; at 900 s it is 96 pings/day ≈ 9.6 mAh/day. Against a sleep-mode budget of roughly
20 mAh/day, halving the keepalive would cost ~25 % of the battery for no latency benefit — down
messages arrive via paging (§6.3), not via the keepalive. Longer than 1800 s risks carrier NAT
timeout on the TCP flow. `PAGER_MQTT_KEEPALIVE_S` is a compile-time constant so Phase 4 can retune
it against a measurement.

UNVERIFIED: carrier NAT idle timeout on this SIM. Cheapest experiment: leave the device idle with
keepalive disabled and log how long before the first publish fails; bisect 5/15/30/60 min. Costs one
overnight run, no extra hardware.

### 6.3 eDRX (sleep mode)

| Parameter | Value | Note |
|---|---|---|
| eDRX cycle | **20.48 s** (fixed, HANDOFF.md §1) | 3GPP WB-S1 eDRX value nibble `0010`. UNVERIFIED whether `walter-modem` takes seconds or the raw nibble — check the header signature before writing `net.c`. |
| PTW (Paging Time Window) | **2.56 s** *(Phase 1 decision — with a 1.28 s idle DRX this gives 2 paging occasions per cycle, i.e. one retry, at ~0.2 mA average; PTW 5.12 s would give 4 occasions and roughly double the paging energy for a redundancy we do not need)* | WB-S1 PTW nibble `0001` |
| PSM | **disabled in MVP** *(Phase 1 decision — PSM suspends paging entirely, which breaks the ≤30 s sleep-mode delivery target. Revisit only if a scheduled/mailbox mode is added (§11).)* | |
| Carrier acceptance | The network may grant different eDRX/PTW values than requested. Firmware MUST log the **granted** values and use them for its latency math. | UNVERIFIED per SIM/carrier. Experiment: log the `+CEDRXRDP`-equivalent readback right after attach — free, first bring-up. |

### 6.4 How these interact with "one persistent session"

The MQTT session lives **inside the modem** and survives ESP32 deep sleep. Therefore:

1. ESP32 deep sleep MUST NOT power-cycle or reset the modem. Any Phase 4 power-gating of the modem
   would destroy the session and force a ~5 kB TLS handshake on every wake — that alone would be
   ~150 kB/day at 30 wakes/day and would dominate everything else in §7.
2. eDRX is what makes a 30-minute keepalive compatible with 30-second delivery: the network pages
   the modem within one eDRX cycle (≤20.48 s), independent of the keepalive timer.
3. The keepalive RTC wake (if needed at all, §8) is the *only* periodic ESP32 wake in sleep mode.

### 6.5 Latency budget check

| Sleep mode (target ~30 s) | Active mode (target < 5 s) |
|---|---|
| paging delay ≤ 20.48 s (eDRX cycle) | C-DRX / connected paging ≤ ~0.5 s |
| RRC connection setup 1–2 s | already connected, 0 s |
| modem delivers URC, ESP32 wake from deep sleep ~0.3 s | ESP32 light-sleep wake < 10 ms |
| JSON parse + partial e-paper refresh 0.5–1.5 s | same, 0.5–1.5 s |
| **worst ≈ 24 s — fits 30 s with ~6 s margin** | **worst ≈ 2 s — fits 5 s** |

The margin in sleep mode is thin. If the granted eDRX cycle is larger than requested (§6.3), the
budget breaks; that is why the granted value must be logged, not assumed.

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
| keepalive PINGREQ + PINGRESP (+ TCP ack) | 4 | 62 | | **182 B ≈ 0.18 kB** |
| **TLS reconnect** (TCP handshake + full TLS 1.2 handshake with a 2-cert chain + MQTT CONNECT/CONNACK + SUBSCRIBE/SUBACK) | | | | **≈ 5 kB (estimate)** |

### 7.3 Monthly projection

Nominal school day: 20 down messages, 5 student replies, 4 reconnects.

```
20 down msgs fully acked  20 x 0.82 kB =  16.4 kB
 5 replies                 5 x 0.29 kB =   1.5 kB
48 keepalives             48 x 0.18 kB =   8.6 kB
34 status publishes       34 x 0.33 kB =  11.2 kB   (24 heartbeat + ~10 event-driven)
 4 reconnects              4 x 5.00 kB =  20.0 kB
                                        ---------
                                          57.7 kB/day  ->  1.69 MB / 30 days
```

Pessimistic day: 100 down messages, 20 replies, 24 reconnects (bad coverage).

```
100 x 0.82 + 20 x 0.29 + 48 x 0.18 + 60 x 0.33 + 24 x 5.00
=  82.0 +  5.8 +  8.6 + 19.8 + 120.0  =  236 kB/day  ->  6.92 MB / 30 days
```

**Verdict.** The 100 MB/month cap allows 3413 kB/day, i.e. ~59× the nominal profile. MQTT+TLS
overhead is not a risk to the data constraint. Both profiles also satisfy HANDOFF.md §8's stricter
"< 10 MB estimated monthly usage". The break-even point is ~4100 fully-acked messages/day.

**The dominant term is reconnects, not messages** — 20 of 58 kB nominal, 120 of 236 kB pessimistic.
This is the quantitative reason for HANDOFF.md's "never reconnect on a timer". Phase 4 should check
whether `walter-modem` exposes TLS session resumption (session ticket / session id); if so, a
resumed handshake costs ~1 kB instead of ~5 kB and cuts the pessimistic day by ~40 %.

**SMS budget: 0 of 100 used.** SMS is out of scope (HANDOFF.md §1). Firmware MUST NOT enable any
SMS send path, and the relay has no SMS code path. This line exists so a later phase cannot quietly
introduce one without editing this document.

---

## 8. ESP32 wake sources

RTC memory contents are defined in §9. "Survives" below means the field must be valid *before* the
wake handler runs.

| # | Wake source | Mechanism | Trigger | Mode | Verified? | RTC state that must survive |
|---|---|---|---|---|---|---|
| 1 | Modem URC / RI line | `esp_sleep_enable_ext1_wakeup()` on the modem's ring-indicator or UART RX line | Modem received an MQTT PUBLISH on `/down` and raises a URC | sleep | **UNVERIFIED** (HANDOFF.md §2 "verify on hardware") | `mode`, `session_id`, `seen_ids[16]`, `pending_acks`, msg ring |
| 2 | Button IO1 | `ext0` wake, active low, RTC GPIO | Short press = mark read / open composer; long press = send reply | sleep (deep) and active (light) | Yes — IO1 is an RTC GPIO per HANDOFF.md §1 | `mode`, `active_until`, msg ring, `pending_acks` |
| 3 | RTC timer — keepalive | `esp_sleep_enable_timer_wakeup(1500 s)` | Issue MQTT PINGREQ; piggyback status heartbeat every 2nd wake (§5.4) | sleep | Yes (mechanism); **need only exists if** the modem does not self-ping — UNVERIFIED | `keepalive_epoch`, `status_pub_count`, `session_id` |
| 4 | RTC timer — active-mode exit | `esp_timer` while awake, not a deep-sleep wake | 10 min with no button/keyboard activity → return to sleep mode | active | Yes | `mode`, `active_until` |
| 5 | LIS3DH INT1 (IO2) | `ext1` | Motion wake | — | **Out of scope for MVP.** Reserved only. | — |
| 6 | CardKB | — | **Cannot wake the ESP32.** CardKB has no interrupt line to an RTC GPIO; it is polled at 100 ms only while the composer is open (HANDOFF.md §2). A reply always starts with a button press. | active | Yes (by construction) | — |

### 8.1 The one experiment that matters

Whether source #1 works determines the whole sleep-mode power story.

**Experiment (cheapest first, ~1 hour, no extra hardware):**
1. Keep the ESP32 awake, modem attached with the MQTT session up. Configure a GPIO interrupt on the
   candidate modem line(s) and log every edge with a timestamp.
2. `tools/send.py` a message. Confirm an edge appears at the same moment as the URC on the UART.
3. If yes: repeat with `esp_light_sleep_start()`, wake on that GPIO, and check
   `esp_sleep_get_wakeup_cause()`.
4. If that works: repeat with `esp_deep_sleep_start()` + `ext1`, and confirm the boot count in RTC
   memory increments and the URC is still retrievable from the modem after the ESP32 reboots.

Step 4 has a second failure mode worth calling out: even if the wake fires, the URC may have been
lost while the UART peripheral was powered down. If so, the device must re-poll the modem's MQTT
receive buffer on wake rather than rely on the URC text. Design `net.c` to **poll-on-wake**
regardless, so correctness does not depend on capturing the URC bytes.

**If the line is not routed to an RTC-capable GPIO on the Walter board**, making it work needs a
bodge wire / pin reassignment → `NEEDS HUMAN DECISION` (HANDOFF.md §7.7 forbids a GPIO change
without asking).

**Fallback if #1 cannot be made to work** (HANDOFF.md §2 mandates documenting this, not silently
degrading): light sleep in both modes, UART RX as the wake source. Estimated cost: ESP32-S3 light
sleep with RAM retention and the UART clocked is roughly 0.3–1.0 mA (estimate) versus roughly
10–100 µA in deep sleep (estimate), so the sleep-mode average would rise from an estimated
0.6–1.2 mA to roughly 1.2–2.5 mA — a **2×-ish battery-life cut**. Phase 4 must measure both and
record the delta in `firmware/README.md`, per HANDOFF.md §2.

### 8.2 Sleep-mode current estimate (all `(estimate)`, TBD — hardware measurement)

| Term | Estimate | Assumption |
|---|---|---|
| Modem eDRX paging | 0.2–0.5 mA | 2 paging occasions per 20.48 s cycle, ~50 ms each at ~50 mA RX, plus warm-up |
| Modem idle floor | 0.01–0.05 mA | Sequans GM02SP deep-sleep-between-paging |
| ESP32-S3 deep sleep | 0.01–0.10 mA | RTC slow memory retained; board leakage unknown |
| Keepalive amortised | ~0.20 mA | 48/day × ~0.1 mAh (§6.2) |
| Display (powered off via IO15 gate) | ~0 mA | e-paper VCC gated between refreshes |
| **Sleep-mode total** | **≈ 0.6–1.2 mA → ~15–29 mAh/day** | On a ~1500 mAh LiFePO4 cell: **~50–100 days idle** |

Active mode: 10-minute window, modem RRC-connected with C-DRX plus ESP32 tickless light sleep,
estimated **15–40 mA → 2.5–6.7 mAh per window**. Six windows/day ≈ 15–40 mAh/day, i.e. active mode
roughly doubles to triples daily consumption. Phase 4 should measure whether idle-mode eDRX at
5.12 s also meets the <5 s target; if it does, it is likely several× cheaper than holding RRC. That
is a measurement to take, **not** a change to HANDOFF.md §2's architecture.

---

## 9. RTC memory contract

ESP32-S3 RTC slow memory is **8 kB total**. Contents survive deep sleep; they are lost on power
cycle, brownout and EN reset.

| Field | Size | Purpose |
|---|---|---|
| `magic` + `crc32` | 8 B | Validity check. On mismatch, treat as cold boot: regenerate `session_id`, clear everything. |
| `boot_count` | 4 B | Diagnostics; distinguishes cold boot from deep-sleep wake |
| `session_id[11]` | 12 B | §1; regenerated **only** on cold boot |
| `mode`, `active_until_epoch` | 12 B | Mode state machine |
| `keepalive_epoch`, `status_pub_count` | 8 B | §5.4, §6.2 |
| `seen_ids[16][12]` + head | 196 B | Dedup ring (§4.1 rule 7) |
| `pending_acks[8]` (id + state + attempts) | 128 B | Ack retry queue (§4.1 rule 6) |
| `pending_up[4]` (id + ts + body + attempts) | ~1.4 kB | Unsent student replies (§4.2) |
| `msg_ring[10]` (Phase 5: id, ts, from, body, flags) | ~3.6 kB | Last 10 messages for the thread view |
| **Total** | **≈ 5.4 kB of 8 kB** | ~2.6 kB headroom |

If Phase 5 finds this does not fit, the first lever is truncating the body retained in RTC to 160
bytes (display-visible portion), which saves ~1.6 kB. **Do not** shrink `seen_ids` — see the
duplicate-storm cost in §4.1 rule 7.

Firmware MUST NOT assume the modem's MQTT session state is mirrored in RTC memory. On every wake it
re-reads the modem's actual connection state and **polls the modem's MQTT receive buffer** (§8.1).

---

## 10. Path to CBOR (documented, not designed)

CBOR is **not** implemented in the MVP. The migration principles the MVP must preserve:

1. **Short keys.** Every field name is ≤4 ASCII characters (`v`, `id`, `ts`, `from`, `body`, `ack`)
   so each maps 1:1 to a CBOR text key with no renaming, and later to a fixed integer keymap:
   `v=0, id=1, ts=2, from=3, body=4, ack=5`. New fields (§11) MUST also be ≤4 characters and MUST
   claim the next free integer in this table when they are added.
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
| `pager/{id}/loc` | GNSS fixes, device → relay | Retained=false, QoS 0 likely (a stale fix is worthless) |
| `pager/{id}/cfg` | relay → device config (mode schedule, geofences) | Retained=true so a waking device gets current config with no request |
| `pager/{id}/evt` | geofence enter/exit, motion, low-battery alerts | Separate from `/up` so the parent thread stays human messages only |

**Schema slots** — reserved field names, not to be reused for anything else: `loc` (lat/lon/acc),
`prio` (priority / alert level), `exp` (message TTL, would supersede §4's fixed 24 h `expired`
sweep), `sched` (mode schedule id).

**Enum headroom** — `from` already allows `system`, which is where geofence and low-battery
notifications will render in the thread without a schema change. `mode` is a string, not a boolean,
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

1. **`NEEDS HUMAN DECISION` — modem wake line GPIO.** If the GM02SP ring-indicator / UART RX line is
   not already routed to an RTC-capable GPIO on the Walter board, enabling deep-sleep wake on an
   incoming MQTT message needs a pin reassignment or a bodge wire. HANDOFF.md §7.7 forbids a GPIO
   change without asking. Impact if declined: the light-sleep fallback in §8.1, roughly halving
   battery life (estimate). Run the §8.1 experiment before asking — it may turn out to be a non-issue.
2. **`NEEDS HUMAN DECISION` — broker free-tier limits.** This contract needs, per device: QoS 1 both
   directions, a retained `/status`, an LWT, `cleanSession=false` with a persistent session that
   outlives a coverage gap, and a 1800 s keepalive. If the chosen free tier caps session expiry
   below a useful window, caps keepalive below 1800 s, or limits credentials/connections such that a
   second device needs a paid plan, that is a paid-service decision. Someone should confirm these
   five properties on the actual HiveMQ Cloud Serverless free tier before Phase 2 hardens against it.
3. **Not a decision, but needs an owner:** per-device MQTT credentials and the broker CA must be
   provisioned to the device at flash time (NVS) and never committed (HANDOFF.md §7.5). No tooling
   for this exists yet in the repo.

### Unverified assumptions tracked in this document

| § | Assumption | Cheapest experiment |
|---|---|---|
| 6.1 | `walter-modem` MQTT is 3.1.1, not 5 | Read the component header — 10 min, no hardware |
| 6.1 | `walter-modem` exposes TLS session resumption | Same header read; worth ~40 % of the pessimistic data budget |
| 6.2 | Modem issues PINGREQ autonomously (would remove wake source #3 entirely) | Attach, set a short keepalive, hold the ESP32 in deep sleep past it, see if the session survives |
| 6.2 | Carrier NAT tolerates a 1800 s idle TCP flow | Overnight idle run with keepalive off, bisect the timeout |
| 6.3 | `walter-modem` eDRX API takes seconds vs. the raw 3GPP nibble | Header read |
| 6.3 | Carrier grants the requested 20.48 s / 2.56 s | Log the granted values at attach — free, first bring-up |
| 8 | Modem URC line can wake the ESP32 from deep sleep | §8.1, ~1 hour |
| 8.2 | All current figures | Phase 4 current-trace measurement; record in `firmware/README.md` |
