# Orchestration prompt for `docs/DEVICE_TASKS.md`

Paste everything below the line into a fresh Claude Code session started in the repo root.

Before starting: the firmware verify steps need an ESP-IDF 5.2 toolchain on the PATH or Docker
with the `espressif/idf:release-v5.2` image; the e2e suite needs Docker running for the emulators
and EMQX. Without either, the orchestrator falls back to "built, unverified" for those tasks.

---

Implement docs/DEVICE_PLAN.md by executing docs/DEVICE_TASKS.md, one task at a time, as an orchestrator.

Ground rules
- Read docs/DEVICE_TASKS.md fully first, then only the DEVICE_PLAN.md sections each task names. Do not re-litigate design decisions; every decision in DEVICE_PLAN.md §9 is final (64-bit HMAC tag, CBOR on the wire, no flash encryption, Noto Sans with Noto Sans CJK default `sc`, typed setup code only, no SMS path, admin-only contact approval, device-local nicknames, passcode lock).
- Work on a branch named `device-plan` off `main`. One commit per task, subject starting with the task id. Do not push and do not open a PR unless I ask.
- Delegate each task to the agent the task file suggests: docs-writer for D tasks, backend-dev for S and T tasks, web-dev for W tasks, firmware-dev for F tasks. Give each agent the task's text verbatim plus the rule "touch only the listed files; run every Verify command; report the last lines of each". If an agent is blocked twice on the same problem, escalate to server-architect or firmware-architect with the failing output, then continue.
- A task is done only when its Verify commands pass. If a Verify command cannot run in this environment (for example no ESP-IDF toolchain for `idf.py build`), try the documented Docker image `espressif/idf:release-v5.2` first; if that is not possible either, mark the task "built, unverified: <reason>" in your report and keep going. Never claim a check passed that did not run.
- Respect the dependency graph at the end of DEVICE_TASKS.md. Start D0.1 alone. When it is committed, run the server track (S1.1 → …) and the firmware track (F3.1 → …) in parallel with separate agents; W tasks after their S dependencies; D0.2 any time after D0.1; D8.1 last. Two agents must never edit the same file at the same time.
- The only cross-track artefact is tools/authvectors.json (created in S1.2, extended in S2b.1). Firmware tasks that need it must wait for the commit that adds it.
- If a task turns out to need a file it does not list, or a design question the plan does not answer, stop that task, write the question into a "Blocked" section of your running report with the exact file and line, and move to the next unblocked task. Do not guess at protocol or security behaviour.

Reporting
- Keep a running file docs/DEVICE_TASKS_LOG.md: one line per task with status (done / built-unverified / blocked), the commit hash, and any UNVERIFIED item from DEVICE_PLAN.md §10 that the task settled, with the answer.
- When every unblocked task is done, give me a summary: what is committed, what is unverified and why, what is blocked and the exact decision I need to make, and the output of the full CI commands from the top of DEVICE_TASKS.md.

Begin with D0.1.
