# DEVICE_TASKS_LOG.md — execution log for docs/DEVICE_TASKS.md

One line per task: status (done / built-unverified / blocked), commit hash, and any
`DEVICE_PLAN.md` §10 UNVERIFIED item the task settled (with the answer).

| Task | Status | Commit | Notes |
|------|--------|--------|-------|
| D0.1 | done | 90f7b7b | PROTOCOL.md §8 items 0–14 applied; §14 Device authentication added. |
| D0.2 | done | ec2212e | SERVER_PLAN.md: deviceSecrets/setupCodes/contactRequests, devices fields, S2.2/S2b.1/S4.1/S4.2 endpoints. |
| S1.1 | done | a5dbd34 | deviceSecrets store + 64-wide replay window transactions; mqttPasswordHash migration off devices.py deferred to S2.2 per task's own Files list (correct per dependency graph). Window arithmetic: implemented full 64-wide accept (0 < upN−n ≤ 64) reconciling §2.5's stated "64 wide" with its literal inequality. |
