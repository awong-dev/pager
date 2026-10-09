# Documentation

Start with the first two. The rest is reference.

| Read | For |
|---|---|
| [OVERVIEW.md](OVERVIEW.md) | How the whole system works, end to end, in one sitting |
| [GOTCHAS.md](GOTCHAS.md) | The things that will bite you: the carrier APN, the modem's TLS quirks, secrets, the broker rule, flashing |
| [HARDWARE_TESTING.md](HARDWARE_TESTING.md) | Building, flashing, the debug console, and what has and has not been seen working on a real pager |
| [ROADMAP.md](ROADMAP.md) | What is unfinished or unverified |

Reference, cited by section number throughout the code (do not renumber):

| Document | Scope |
|---|---|
| [PROTOCOL.md](PROTOCOL.md) | The wire contract: topics, envelopes, the CBOR keymap, acks, status, location, SMS audit, signing, data and power budgets. Code conforms to this, not the reverse |
| [DEVICE_PLAN.md](DEVICE_PLAN.md) | Pager-side design: setup codes and the bootstrap bundle, per-device signing, the address book, the UI, the lock |
| [SERVER_PLAN.md](SERVER_PLAN.md) | Server-side design: data model, routing, allow-lists, delivery backends, the web app, security rules, cost |
| [V02_DESIGN.md](V02_DESIGN.md) | The v0.2 additions: vendored modem library, wider replay counter, CA trust and delivery, location, device SMS |
| [FAMILIES_DESIGN.md](FAMILIES_DESIGN.md) | Multi-family tenancy: families, super/admin/member roles, conversation policies, SMS externals, admin alerts, and the web UI per role. Tasks in [FAMILIES_TASKS.md](FAMILIES_TASKS.md) |

Other:

| Document | Scope |
|---|---|
| [SORACOM_EVAL.md](SORACOM_EVAL.md) | Evaluation; superseded by SORACOM_DESIGN.md for the Beam shape |
| [SORACOM_DESIGN.md](SORACOM_DESIGN.md) | Soracom bearer: plain MQTT through Beam, chosen by the inserted SIM. Decided 8 Oct 2026 |
| [SORACOM_TASKS.md](SORACOM_TASKS.md) | Execution tasks for SORACOM_DESIGN.md |
| [VENDOR_BUG_REPORTS.md](VENDOR_BUG_REPORTS.md) | Bug reports drafted for Sequans and DPTechnics. Not yet sent |
| [history/](history/) | The task lists the first build was executed from. Code comments such as "DEVICE_TASKS.md F3.5" refer to `history/DEVICE_TASKS.md`. Not maintained |

Design notes and task lists, one per feature (scope from each file's opening lines):

| Document | Scope |
|---|---|
| [OTA_DESIGN.md](OTA_DESIGN.md) | Over-the-air firmware update: signed `cfg.ota` job, HTTPS download from the public bucket, full or delta image, rollback. Shipped; factory slot still open |
| [RELAY_SMS_DESIGN.md](RELAY_SMS_DESIGN.md) | Relay SMS: one Twilio number per user, outbound by policy, unknown inbound held for parent approval. Decided 8 Oct 2026 |
| [BRIDGE_PHONE_DESIGN.md](BRIDGE_PHONE_DESIGN.md) | Bridge phone: a headless Android phone carries a member's SIM texts, Google Voice and subscribed Google Chat through the relay. Decided 8–9 Oct 2026; built, not yet run on a phone |
| [BRIDGE_PHONE_TASKS.md](BRIDGE_PHONE_TASKS.md) | Execution tasks for BRIDGE_PHONE_DESIGN.md: relay B1–B9, web W1–W5, Android A1–A7, docs D1–D2 |
| [LOCATION_TRACKING_DESIGN.md](LOCATION_TRACKING_DESIGN.md) | Location tracking: hourly cell fix when stationary; cell first while moving, GNSS in eDRX gaps every 10 min |
| [GNSS_DISABLE_DESIGN.md](GNSS_DISABLE_DESIGN.md) | Per-device `cfg.loc.gnss` setting that stops the pager from ever powering the GNSS receiver |
| [BATTERY_STATS_DESIGN.md](BATTERY_STATS_DESIGN.md) | On-device state-time counters (`/status` key 68) and the modelled mAh/day drain shown in the web app |
| [SHAKE_WAKE_DESIGN.md](SHAKE_WAKE_DESIGN.md) | A deliberate shake wakes the UI and opens a short hot keyboard window; ordinary motion only feeds location |
| [ADDRESS_BOOK_DESIGN.md](ADDRESS_BOOK_DESIGN.md) | The pager's address book: built by the relay, editable from the web app by a family admin, synced to the pager |
| [CONTACT_REQ_DESIGN.md](CONTACT_REQ_DESIGN.md) | Contact requests from the pager, SMS contacts per family, and family rename |
| [CHAT_UI_DESIGN.md](CHAT_UI_DESIGN.md) | Pager chat UI and address-book design: all chats, new chat, new group from the book (24 Sep 2026) |
| [GROUP_CHAT_DESIGN.md](GROUP_CHAT_DESIGN.md) | Group chat basics: conversation store, routing, `sndr` on the pager, web UI |
| [WIFI_DESIGN.md](WIFI_DESIGN.md) | WiFi as an alternate MQTT transport carrying the same session; LTE-M stays the default. Tasks in [WIFI_TASKS.md](WIFI_TASKS.md) |
| [V03_PLAN.md](V03_PLAN.md) | v0.3 plan: composer overflow, web auto-scroll, push and geofence. Tasks in [V03_TASKS.md](V03_TASKS.md) |
| [DEVICE_NEXT_TASKS.md](DEVICE_NEXT_TASKS.md) | Next device tasks: group `sndr`, accelerometer, SMS |
| [SLEEP_URC_DESIGN.md](SLEEP_URC_DESIGN.md) | Delivering a page URC through light sleep. Tasks in [SLEEP_URC_TASKS.md](SLEEP_URC_TASKS.md) |
| [SLEEP_PAGE_LOSS_BRIEF.md](SLEEP_PAGE_LOSS_BRIEF.md) | Self-contained brief on pages lost while the pager light-sleeps (resolved 24 Sep 2026) |
| [RCA_SLEEP_PUBLISH.md](RCA_SLEEP_PUBLISH.md) | Root-cause analysis: release-only "publish payload replaced by AT command text" (mechanism narrowed, not proven) |
| [RCA_SLEEP_URC.md](RCA_SLEEP_URC.md) | Root-cause analysis: URCs lost or held at the light-sleep boundary |

Each top-level directory has its own README for running that part: `relay/`, `web/`,
`firmware/`, `infra/`.
