# Handoff: School Pickup Pager — text message relay (MVP)

> **Status (updated after Phase 6 + follow-ups): MVP is code-complete.** All phases in §5 below are
> done and committed (`git log --oneline` shows the phase-by-phase history). The relay, protocol
> doc, device simulator, parent web page, and firmware (network core, UI, message storage) are all
> implemented and verified as far as possible **without physical hardware** — end-to-end over a
> real local broker, independently rebuilt from fresh clones, with real bugs found and fixed along
> the way (see commit messages for specifics, e.g. a relay reconnect bug and several firmware
> power/correctness bugs). No Walter board has ever been attached to any session that built this.
>
> What's actually left is not a coding phase — it's hardware bring-up and a couple of standing
> decisions:
> - `firmware/README.md`'s measurement checklist (M1-M15) and "Residual risks" section — what to
>   check first once a real device exists, ranked by risk to battery life and message latency.
> - `docs/PROTOCOL.md` §12 — two open decisions (real broker free-tier limits unverified; LWT/
>   clean-session/retained-status not settable from the vendor library's public API) that need a
>   human call, not more code.
> - No tooling exists yet for provisioning per-device MQTT credentials/TLS certs at flash time
>   (§12 item 5) — needed before a second device can exist.
>
> If you're picking this project back up: read `docs/PROTOCOL.md` first (the authoritative wire
> contract — code must conform to it, not the other way around), then `firmware/README.md`'s
> residual risks if you're touching firmware, or `relay/README.md` if you're touching the relay.
> The rest of this document is the original planning brief, kept for the phase-by-phase rationale.

This document is the brief for a Claude Code session acting as **orchestrator**. It describes what to build, how the work is split, and which model runs each step. Read it fully before starting. Do not expand scope beyond what is marked MVP.

## 1. What we are building

A battery-powered pager for a student. The device (DPTechnics **Walter**: ESP32-S3 + Sequans GM02SP LTE-M modem + GNSS) shows short text messages from a parent on a 2.9" e-paper display and lets the student reply with a small I2C keyboard. All traffic goes through a cloud relay; the device never talks to the parent directly.

**MVP = text relay only.** GPS reporting, geofences, schedule-based mode switching and SMS fallback are explicitly out of scope for this handoff. Design so they can be added later (topic layout, message schema, mode state machine stub) but do not implement them.

### Hardware already decided
| Part | Role | Interface |
|---|---|---|
| Walter module | MCU + LTE-M modem | — |
| GDEY029T94-FT01 (SSD1680, 296×128) | E-paper display | SPI: SCK IO12, MOSI IO11, CS IO10, DC IO16, RST IO17, BUSY IO18; VCC gated by P-MOSFET on IO15 (active low) |
| M5Stack CardKB | Keyboard | I2C 0x5F on IO8 (SDA) / IO9 (SCL), polled |
| LIS3DH breakout | Motion wake (later) | I2C 0x18, INT1 on IO2 |
| Push button | Wake / open reply | IO1, active low, RTC GPIO |
| LiFePO4 18650 + LFP charger | Power | VIN 3.0–5.5 V |

GPIO numbers for IO1/IO2/IO11/IO12/IO15 are provisional; firmware must keep them in one `pins.h` so they can change without touching logic.

### Hard constraints
- SIM: 100 MB/month data, 100 SMS/month. Do not use SMS in the MVP. Keep one persistent TLS+MQTT session; never reconnect on a timer.
- Battery: device sleeps most of the day. In **sleep mode** the modem uses eDRX (target 20.48 s cycle) and the ESP32 deep sleeps; message delivery within ~30 s is fine. In **active mode** the modem stays connected and messages must show within 5 s.
- Free-tier cloud only.
- Payloads are compact JSON (MVP) with a documented path to CBOR. Max message body 160 characters.

## 2. Architecture (fixed — do not redesign)

```
Parent (browser/CLI) --HTTPS--> Relay API --MQTT publish--> Broker --MQTT--> Device
Device --MQTT publish--> Broker --MQTT--> Relay API --stores/serves--> Parent
```

- **Broker**: hosted MQTT over TLS with a free tier (default: HiveMQ Cloud Serverless; EMQX Cloud Serverless acceptable). Two credentials: `relay` (server) and one per device.
- **Relay API**: one small service, Python 3.12 + FastAPI + paho-mqtt + SQLite. Runs as a single long-lived process (needs a persistent MQTT connection, so not a serverless function). Deploy target: any free small VM or Fly.io free allowance; must also run locally with `docker compose up`.
- **Device firmware**: ESP-IDF 5.x, `dptechnics/walter-modem` component, MQTT client running **inside the Sequans modem** (via the library), not lwIP on the ESP32.
- **Parent client (MVP)**: a single static HTML page served by the Relay API (send message, see thread, see delivered/read status). No auth beyond a shared bearer token in the MVP.

### Topics
```
pager/{device_id}/down      relay → device   QoS 1, retained=false
pager/{device_id}/up        device → relay   QoS 1
pager/{device_id}/status    device → relay   retained (online/offline via LWT, mode, battery mV)
```

### Message schema (JSON, both directions)
```json
{ "id": "m_7f3a", "ts": 1757700000, "from": "parent", "body": "Pickup at 3:15 by the gym", "ack": null }
```
- Device acks a down message by publishing `{ "id": "...", "ack": "shown" }` on `/up` once rendered; later `"ack": "read"` when the student presses the button.
- Relay persists every message with states `queued → sent → shown → read`.
- Relay re-publishes unacked messages on device `online` status (broker QoS 1 handles most of this; the re-publish covers session loss).

### Device mode logic (MVP subset)
- Boot in **sleep mode**: request eDRX 20.48 s + PTW; ESP32 deep sleeps with wake sources = modem UART RX line (verify on hardware), button IO1, RTC timer for the MQTT keepalive.
- Enter **active mode** on: incoming message, button press. Leave after 10 min with no button/keyboard activity.
- Active mode: modem stays attached in connected mode; ESP32 uses automatic light sleep (tickless idle); CardKB polled every 100 ms only while the reply composer is open.
- If the modem-driven deep-sleep wake cannot be made to work, fall back to light sleep in both modes and document the measured current difference. Do not silently degrade.

## 3. Repository layout (create exactly this)
```
school-pager/
  README.md
  docs/PROTOCOL.md            # topics, schema, state machine, data budget
  relay/                      # Python service
    app/  tests/  Dockerfile  docker-compose.yml  pyproject.toml
    static/index.html         # parent MVP page
  firmware/                   # ESP-IDF project
    main/  components/  sdkconfig.defaults  README.md
    main/pins.h  main/net.c  main/ui.c  main/modes.c  main/msg.c
  tools/
    sim_device.py             # laptop-side fake device speaking the same MQTT protocol
    send.py                   # CLI to send a message via the relay API
  .claude/agents/             # subagent definitions (provided)
```

## 4. Model selection policy

Use the cheapest model that can do the step reliably. Aliases are Claude Code's `opus`, `sonnet`, `haiku` (they resolve to the current versions). Set them in subagent frontmatter (`model:` field) and use `/model` for the main session.

| Tier | Use for | Why |
|---|---|---|
| **opus** | Protocol/state-machine design review, ESP-IDF sleep + modem integration, anything touching power management or the modem AT flow, root-causing a failing hardware test | Highest reasoning need; mistakes here cost days on hardware |
| **sonnet** | All backend implementation, MQTT handling, tests, the device simulator, the static parent page, firmware modules that are pure logic (message store, UI rendering, keyboard) | Standard engineering work with clear specs |
| **haiku** | Boilerplate (pyproject, Dockerfile, CI yaml), README/PROTOCOL.md drafting from decisions already made, lint/format fixes, summarising logs, renaming, generating fixtures | Mechanical, low-ambiguity |

Main orchestrator session: run on **sonnet**. Escalate to `opus` only via the `firmware-architect` subagent or when a sonnet subagent reports being stuck twice on the same problem. Never let `haiku` edit `firmware/main/net.c`, `modes.c`, or anything under `relay/app/mqtt*`.

## 5. Work plan (execute in order; each phase ends with a commit)

### Phase 0 — Scaffold (haiku)
Create the repo layout, `pyproject.toml` (fastapi, uvicorn, paho-mqtt, pydantic, pytest), `Dockerfile`, `docker-compose.yml` (relay + a local `eclipse-mosquitto` broker for dev), ESP-IDF project skeleton with `idf_component.yml` depending on `dptechnics/walter-modem`, empty module files, `.gitignore`, `README.md` stub. Done when `pytest` runs (0 tests) and `idf.py build` succeeds on the empty app.

### Phase 1 — Protocol doc (opus writes, haiku formats)
`firmware-architect` produces `docs/PROTOCOL.md`: topics, JSON schema, ack state machine, LWT/status contract, keepalive and eDRX numbers, per-message byte budget, and a table of ESP32 wake sources. This is the contract; later phases must not deviate without updating it. `docs-writer` (haiku) cleans formatting only.

### Phase 2 — Relay API (sonnet: `backend-dev`)
- `POST /api/devices/{id}/messages` body `{body}` → stores, publishes to `/down`, returns id.
- `GET /api/devices/{id}/messages?since=` → thread with states.
- `GET /api/devices/{id}/status` → last retained status.
- MQTT loop: subscribe `pager/+/up` and `pager/+/status`; apply acks; on `online` re-publish `queued/sent` messages.
- SQLite via `sqlite3` stdlib; schema migrations as plain SQL files.
- Bearer token from env `RELAY_TOKEN`. Broker creds from env.
- Tests (pytest): use an in-process fake broker or mosquitto in docker; cover send → publish, ack transitions, re-publish on reconnect, malformed payload rejection.
Done when `docker compose up` + `tools/send.py` + `tools/sim_device.py` round-trip a message with all four states.

### Phase 3 — Device simulator (sonnet: `backend-dev`)
`tools/sim_device.py`: connects as a device, LWT on status, prints down messages, auto-acks `shown`, `--reply "text"` publishes up. Used as the firmware's reference behaviour.

### Phase 4 — Firmware: network core (opus: `firmware-architect` designs and reviews; sonnet: `firmware-dev` implements)
`net.c`: modem init, LTE-M attach, eDRX/PSM configuration, TLS cert provisioning to the modem, MQTT connect/subscribe with LWT, publish/receive callbacks, keepalive on RTC timer wake. `modes.c`: sleep/active state machine with the wake-source handling above. Every decision that costs power gets a one-line comment with the estimated mA or mAh.
Acceptance: on hardware, a message sent from `send.py` appears in the serial log within 30 s in sleep mode and 5 s in active mode; measured sleep-mode average current is logged in `firmware/README.md`.

### Phase 5 — Firmware: UI (sonnet: `firmware-dev`)
`ui.c`: SSD1680 driver (partial refresh for message pane, full refresh every 20 partials), thread view (last 3 messages), reply composer with CardKB; `msg.c`: RTC-memory ring buffer of last 10 messages surviving deep sleep. Button: short press = mark read / open composer; long press = send reply.

### Phase 6 — Integration + hardening (sonnet, opus for review)
End-to-end test script; power-on with no network; broker outage recovery; malformed message handling; data-usage estimate over 24 h of simulated use written to `docs/PROTOCOL.md`. `firmware-architect` does a final review of `net.c`/`modes.c` against the power and latency constraints and lists residual risks.

## 6. Subagents (files in `.claude/agents/`, provided alongside this doc)
- `firmware-architect` — opus, read-mostly, designs/reviews.
- `firmware-dev` — sonnet, implements ESP-IDF modules.
- `backend-dev` — sonnet, implements relay, tools, tests.
- `docs-writer` — haiku, docs/boilerplate/formatting only.
- `log-triage` — haiku, read-only, summarises build/serial logs and proposes the next check.

## 7. Orchestrator rules
1. One phase at a time; do not start Phase N+1 until Phase N's "done when" holds and is committed.
2. Delegate implementation to subagents; keep the main context for planning, verification and merging. Ask `log-triage` to summarise any output longer than ~200 lines before reading it yourself.
3. A subagent that reports "blocked" twice on the same issue → escalate to `firmware-architect` (opus) with the exact error and the last 50 lines of the relevant log.
4. Hardware-dependent steps: if no device is attached, implement, build, and mark the acceptance test `PENDING_HW` in `firmware/README.md`; never claim a hardware test passed without a serial log excerpt.
5. Never commit broker credentials or the bearer token; use `.env.example`.
6. Keep `docs/PROTOCOL.md` authoritative. Any schema/topic change requires editing it first, then code.
7. Stop and ask the human before: choosing a paid service, changing a GPIO assignment, or adding a dependency to the firmware beyond `walter-modem`, `esp_timer`, and a display driver.

## 8. Definition of done (MVP)
- `docker compose up` brings up broker + relay locally; `send.py` → `sim_device.py` round-trip passes in CI.
- Firmware builds clean; on hardware, messages appear on the e-paper within the latency targets for each mode; a reply typed on CardKB appears in the parent page.
- Sleep-mode current measured and recorded; estimated monthly data usage recorded and < 10 MB.
- README explains setup in under 15 lines per component.
