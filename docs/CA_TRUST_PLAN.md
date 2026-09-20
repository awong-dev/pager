# CA trust: graceful failure and over-the-air CA push (future work)

Status: **implemented on branch `v0.2-dev` (2026-09-20), not yet run on hardware.** The owner
lifted the park once v0.1 delivered end to end. `V02_DESIGN.md` §4 is the spec that was built and
is authoritative where it differs from this file: the CA travels as a URL plus SHA-256 (task C1's
"prove the fetch on hardware" became a debug console command, `cafetch`, instead of a gate), the
bootstrap bundle carries the same pointer, and the fallback, padlock, `/status` fields, push and
two-phase apply are all in. Kept for its reasoning. If the project moves to Soracom
(`SORACOM_EVAL.md`), none of this is needed there.

## 1. Problem

Today a pager that pins a CA has exactly one way to get a different CA: a person types a new setup
code on it. If the broker moves to a different root, every pinned pager fails its TLS handshake
(`+SQNSMQTTONCONNECT:0,-8`), `modes.c`'s `handle_mqtt_loss()` files that under
`NET_MQTT_RC_TLS_FAIL` and retries every 300 s forever. The pager is silently dead until someone
walks up to it. Two changes fix that.

## 2. Requirements (from the owner)

- **R1. Messages still go through when CA validation fails.** A trust break must not take the
  pager offline.
- **R2. The person is told.** The pager shows a broken-padlock symbol (or similar) while it is
  running without a validated server. The web app shows the same state for that device.
- **R3. A new CA can be pushed from the web app**, without anyone touching the pager.

## 3. Design

### 3.1 Trust states

| State | Meaning | TLS profile | Indicator |
|---|---|---|---|
| `unpinned` | bundle carried no CA | validation off | none (this is the chosen default, not a fault) |
| `pinned` | CA set, last connect validated | validation on | closed padlock, or none |
| `broken` | CA set, validation failed, running on the fallback | validation off | **broken padlock** |

`broken` is sticky across reboots (one NVS byte in `ident`), and clears only on a connect that
validates against the pinned CA.

### 3.2 Fallback (R1)

On `NET_MQTT_RC_TLS_FAIL` while `pinned`: retry once with validation on (rules out a one-off
handshake failure), then reconfigure profile 2 with validation off **keeping slot 12 named**
(§3.3 of `DEVICE_PLAN.md`), enter `broken`, connect. While `broken`, every Nth reconnect
(suggest N = 1 per day, not per connect: each attempt costs a full handshake of airtime) first
tries validation on, so a transient cause heals by itself.

Known limits, stated so nobody is surprised later:

- **This makes pinning advisory.** An attacker on the LTE→broker path only has to present a bad
  certificate to push the pager into `broken`, after which they can read bodies and location fixes
  and capture the MQTT password. Envelopes stay HMAC-signed (`PROTOCOL.md` §2.4), so pages still
  cannot be forged or altered. The owner has accepted this trade: reachability over
  confidentiality, with the indicator as the tell. `DEVICE_PLAN.md` §2.2 option E (AEAD bodies
  under `K_dev`) is the real answer to confidentiality and is independent of this plan.
- **`-8` is not specific.** The modem reports the same code for a bad certificate and for any
  other handshake failure (observed: a server that accepts TCP and never answers the ClientHello
  also yields `-8`). The pager cannot tell "untrusted" from "broken". The indicator therefore
  means "the server was not verified", not "you are under attack". Word any UI text accordingly.

### 3.3 Reporting (R2)

- Pager: a padlock glyph in `ui.c`'s `draw_status_bar()`, broken variant while `broken`. Needs
  two glyphs added to the assets build. E-paper: redraw only on state change.
- Wire: `/status` gains an optional `tls` field, one of `unpinned`/`pinned`/`broken`
  (`PROTOCOL.md` §5 table and §10 keymap: next free key). Absent means an older firmware.
- Relay: store it on `devices/{id}` alongside the other status fields. On a transition into
  `broken`, log a security event and (optional) notify the device's owner through their normal
  delivery backends.
- Web: padlock chip per device on *Admin → Devices*, with the last-changed time.

### 3.4 CA push (R3)

- Web: *Admin → Devices → Push CA*. Pushes the relay's current CA (`BROKER_CA_PEM`, or
  `ca_resolve.get_broker_ca_pem()` once that is wired in). Also an "un-pin" action that pushes an
  empty CA. Both confirm first and show the CA subject and fingerprint.
- Relay: `POST /api/admin/devices/{id}/ca`. Admin only.
- **Transport: push a pointer, fetch the CA (owner's proposal, preferred).** The `/down` message
  carries `{url, sha256}` and nothing else; the pager fetches the CA from the URL and accepts it
  only if its SHA-256 matches. It fits the ordinary 640-byte envelope as an ordinary signed `cfg`
  message, so no new topic, no chunking and no ACL change.
  - **Trust comes from the hash, not from the fetch.** The hash arrives inside a signed message
    (or, at bootstrap, inside the encrypted bundle), so the download needs no server
    authentication at all: fetch over HTTPS with validation off, or plain HTTP. This matters,
    because validating the download host would need a second pinned CA and would recreate the
    problem one level up.
  - **Make the URL content-addressed**, e.g. `https://<relay>/ca/<sha256>.pem`: immutable,
    cacheable, and the pager can sanity-check the URL against the hash it was given.
  - **This should probably replace the in-bundle CA at bootstrap too.** Measured root sizes
    (PEM / DER bytes): DigiCert Global Root G2 1294 / 914, Amazon Root CA 1 1188 / 837,
    ISRG Root X2 790 / 543, **ISRG Root X1 1939 / 1391, GTS Root R1 1911 / 1371**. The vendor
    library's receive buffer is 1540 bytes (`WALTER_MODEM_RSP_BUF_SIZE`; `mqttReceive()` copies
    out of it with no bounds check), and the DigiCert bundle measured 1468 bytes on the wire on
    2026-09-19, about 70 bytes of headroom. **A Let's Encrypt (ISRG Root X1) or Google root
    cannot fit in the bundle at all, even as DER**, so the in-bundle design already cannot serve
    `infra/modules/broker-gce`. With a pointer, the bundle drops back to about 250 bytes for any
    CA.
  - `UNVERIFIED`, resolve in task C1 on hardware before building on it:
    - Whether the modem's HTTP client (`AT+SQNHTTP*`, the library's `httpConfigProfile()` /
      `httpQuery()` / `httpDidRing()`) can return a 2 kB body. It reads into the same 1540-byte
      buffers. If not, fetch with a raw TCP/TLS socket and read in chunks (`AT+SQNSRECV`), which
      has no total size limit; `nettest` already drives that layer.
    - TLS profile contention. The library caps TLS profile ids at 0-2, 1 is BlueCherry's and 2 is
      the live MQTT session's. Does reconfiguring profile 2 disturb a connected MQTT session? The
      modem itself supports ids 1-6, so a raw `sendCmd("AT+SQNSPCFG=3,...")` may sidestep the
      cap; check that the HTTP and socket calls accept such an id. A plain-HTTP endpoint avoids
      the question entirely, but Cloud Run and Firebase Hosting both force HTTPS.
    - Tonight's lesson applies: confirm on the wire what the modem actually sends.
  - Fallbacks if the fetch proves unworkable: a dedicated retained topic `pager/{id}/ca` with the
    boot topic's 4 kB cap (still bounded by the 1540-byte buffer, so DER only and not every
    root), or chunking across several `cfg` messages.
  - Cost: one extra TCP+TLS connection per CA change, roughly 5-7 kB of airtime. CA changes are
    rare, so this is negligible against the power budget. It adds a dependency on the relay's
    HTTP endpoint being reachable during setup.
- **The push MUST be signed** with the device key and verified before use, exactly like any other
  `/down`. This matters more than usual: the push will often arrive while the pager is in
  `broken`, i.e. over a session nobody verified.
- **Two-phase apply on the pager**, so a bad push cannot make things worse:
  1. write the new CA to a scratch slot (13), configure a profile against it, reconnect;
  2. if that connect validates, commit: `ident_store()` the new CA and hash, copy to slot 12,
     state `pinned`; otherwise discard it, keep the old CA, stay in the previous state, and
     report the failure in `/status`.
- Ack back to the relay so the web app can show "applied" or "rejected", not just "sent".

## 4. Tasks

Same shape as `DEVICE_TASKS.md`: one task at a time, do not start a task before its
dependencies. Tracks can run in parallel.

Dependency graph: `D1 → {F1, S1}`, `F1 → F2`, `S1 → W1`, `C1 → {F3, B1}`, `S0 → {S2, B1}`,
`S2 → W2`, `{F2, F3, S2} → E1`. `C1` and `S0` have no dependencies and can start first.

### D1 Protocol edits
- Read: `PROTOCOL.md` §3.3, §5, §6.1, §10; this file §3.
- Files: `docs/PROTOCOL.md`.
- Do: add the `/status` `tls` field and its keymap number; nothing else yet.
- Verify: `grep -n '`tls`' docs/PROTOCOL.md` shows the §5 row and the §10 row with one number.

### F1 Trust state and fallback
- Read: this file §3.1, §3.2; `firmware/main/modes.c` `handle_mqtt_loss()`; `net.cpp`
  `net_init()`.
- Files: `firmware/main/net.cpp`, `net.h`, `modes.c`, `ident.c`, `ident.h`.
- Do: add the state, the NVS byte, the retry-then-fallback path and the daily re-try. Keep slot 12
  named in every profile.
- Verify: host tests pass (`make -C firmware/host test`). On hardware: set up with a **wrong**
  CA in `BROKER_CA_PEM`, confirm the log shows one validated attempt, then the fallback, then
  `MQTT connected`, and that a page arrives.

### F2 Padlock in the status bar
- Read: this file §3.3; `firmware/main/ui.c` `draw_status_bar()`; the assets build.
- Files: `firmware/main/ui.c`, asset sources, `firmware/host/render_png.c` expectations.
- Do: two glyphs, drawn from the trust state; redraw on change only. Publish `tls` in `/status`.
- Verify: host PNG render shows each glyph; on hardware the broken padlock appears in F1's test.

### S1 Relay stores and surfaces `tls`
- Read: `PROTOCOL.md` §5.3; `relay/app` status ingest.
- Files: relay status ingest, `devices_store`, tests.
- Do: persist the field, log a security event on a transition into `broken`.
- Verify: `pytest relay` passes with a new test that posts a status with `tls: "broken"`.

### W1 Web: padlock chip
- Files: `web/app/admin/devices/page.tsx`, `web/lib/types.ts`.
- Verify: `npm --prefix web run build`; the chip renders for all three states and for "absent".

### C1 Prove the CA fetch on hardware
- Read: this file §3.4; the vendor `examples/https`; `net.cpp` `net_check_tcp()`.
- Files: `firmware/main/net.cpp`, `main.c` (a temporary `cafetch <url> <sha256>` console command).
- Do: fetch a 2 kB file (use ISRG Root X1) over the cellular link while an MQTT session is up,
  first with the library's HTTP client, then, if the body is truncated, with a chunked socket
  read. Verify the SHA-256 on the pager. Answer every `UNVERIFIED` bullet in §3.4 and record the
  results there and in `PROTOCOL.md`.
- Verify: the log shows the computed hash equal to the expected one, and the MQTT session still
  delivers a page afterwards.

### S0 Relay serves CAs by hash
- Files: a new public `GET /ca/{sha256}.pem` route in `relay/app`, tests. Wire in
  `ca_resolve.get_broker_ca_pem()` so the relay knows its CA without `BROKER_CA_PEM`.
- Verify: `pytest relay`; `curl` the route and compare `shasum -a 256` with the path.

### B1 Bootstrap bundle carries a pointer, not the CA
- Depends on: C1, S0.
- Files: `relay/app/devsetup.py`, `relay/app/wirecbor.py` (two new boot keys), `firmware/main/setup.c`,
  `docs/PROTOCOL.md` §10, `tools/setup_code_vectors.json`.
- Do: add `ca_url` and `ca_sha256` to the bundle; keep accepting an inline `ca` for one release.
- Verify: host tests and `pytest relay` pass; a real `setup` against a broker behind ISRG Root X1
  ends `pinned`.

### F3 Pager applies a pushed CA
- Read: this file §3.4; `setup.c` (bundle receive path); `auth.c` (signature verify).
- Do: verify signature, two-phase apply, ack.
- Verify: on hardware, push the correct CA to a pager in `broken`; it returns to `pinned` and the
  padlock closes. Push a wrong CA; the pager rejects it and keeps working.

### S2 Relay endpoint for the push
- Files: `relay/app/routers/admin.py`, `relay/app/devcfg.py` (or a new module), `emqx_admin.py`
  ACLs if option A, tests.
- Verify: `pytest relay`; the e2e suite gains a `ca_push` scenario against `pager_client.py`.

### W2 Web: Push CA and un-pin buttons
- Files: `web/app/admin/devices/page.tsx`.
- Verify: `npm --prefix web run build`; the button shows subject, fingerprint and the ack result.

### E1 End-to-end rehearsal of a root change
- Do: on a real pager pinned to CA X, change the relay to CA Y where the broker really serves Y.
  Expect: `broken` padlock, pages still arrive, one click pushes Y, padlock closes.

## 5. Definition of done

A pinned pager whose broker changes roots keeps receiving pages, shows a broken padlock on the
pager and in the web app within one status interval, and is repaired from the web app with no one
touching the pager. `PROTOCOL.md` and `DEVICE_PLAN.md` §3.3 describe the shipped behaviour.
