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

Other:

| Document | Scope |
|---|---|
| [SORACOM_EVAL.md](SORACOM_EVAL.md) | Evaluation of Soracom as the carrier, including a design with no MQTT broker. Undecided |
| [VENDOR_BUG_REPORTS.md](VENDOR_BUG_REPORTS.md) | Bug reports drafted for Sequans and DPTechnics. Not yet sent |
| [history/](history/) | The task lists the first build was executed from. Code comments such as "DEVICE_TASKS.md F3.5" refer to `history/DEVICE_TASKS.md`. Not maintained |

Each top-level directory has its own README for running that part: `relay/`, `web/`,
`firmware/`, `infra/`.
