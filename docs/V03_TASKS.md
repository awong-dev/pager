# v0.3 tasks

Companion to `docs/V03_PLAN.md`, which holds the reasoning and the decisions; this file holds only
what an implementing agent needs. Each task names its agent, the files to read first, the files it
may touch, what to do, and how to verify. Bench tasks obey `docs/HARDWARE_TESTING.md` and the
bench rules in `.overnight-handoff.md` verbatim: one reader on the port, one hardware agent at a time,
bounded foreground captures. Coding agents never open `/dev/cu.usbmodem*`.

Build (firmware): `export PATH=<pyshim>:$PATH; . ~/src/esp/esp-idf/export.sh; cd firmware;
PAGER_DEBUG_NO_LIGHT_SLEEP=1 idf.py reconfigure build`. Host tests: `make -C firmware/host test`
(14 suites; every firmware task must leave them green). Web: `cd web && npm run lint && npm run
build`. Relay: `cd relay && .venv/bin/pytest`.

Order: 2.1 → 2.2 → 2.3 → 1.0 → 1.1 → 1.2 → 1.3 → 1.4 → 3a.1 → 3a.2 → 3a.3 → 3a.4 → 3b.1 → 3b.2 →
3c.0 (gates) → 3c.1 → 3c.2 → 3c.3 → 3c.4.

---

## 1. Composer overflow on the device

### 1.0 Debug console `key <text>` feeds nothing for multi-character input — firmware-dev, timebox 1 h

**Read:** `firmware/main/main.c` `cmd_key()` (lines ~257-277), `firmware/main/input.c/h`
(`input_feed_key`, `input_arm_awake`, the key queue and its depth), `firmware/main/modes.c` where the
queue is drained and `ui_dispatch_key()` is called, `build/bench-logs/phaseG.log` (the failing run:
`key a`..`key d` each produced one partial; `key efghijkl` produced nothing, `dirty_rows=0`).
**Files:** `firmware/main/input.c`, `firmware/main/input.h`, `firmware/main/main.c` only.
**Do:** find why eight keys fed in one call never reach the composer while one key per call does.
Suspects, in order: queue depth smaller than 8 with silent drop; the drain loop taking one key per
wake iteration and the awake window or a debounce discarding the rest; the coalesced redraw from
`ea5236c` swallowing the batch. Fix only if the cause is in the console/queue path and the fix is a
few lines; if it touches the real keyboard path, stop and report. Add a host test in
`firmware/host/test_input.c` for "N keys fed at once are all delivered".
**Verify:** host tests green; report the cause with file:line. Bench check is part of 1.3.

### 1.1 Composer viewport helper — firmware-dev

**Read:** `firmware/main/scr_chat.c` lines 670-780 (layout budget, `chat_build_rows()`, the
composer draw at 772-774), `firmware/host/test_chat_layout.c` (the pattern: pure helpers, `rows`
passed in), `firmware/main/gfx.h` (`gfx_text`, `gfx_text_width`), `firmware/main/msg.h`
(`MSG_COMPOSER_MAX`, `MSG_COMPOSER_MAX_CODEPOINTS`), `docs/V03_PLAN.md` §1.
**Files:** a new pure header/impl pair (`firmware/main/chat_layout.h/.c` or wherever
`chat_build_rows()`'s helpers live — match the existing split), `firmware/host/test_chat_layout.c`,
`firmware/host/Makefile` if a new source needs listing.
**Do:** add

```c
/* Given per-codepoint advances adv[0..n-1] (px), the span available for text (px), and the
 * width of the "…" marker (px), return the index of the first codepoint to draw and whether the
 * marker is needed. Draws the longest suffix that fits; when everything fits, start = 0 and no
 * marker. n == 0 -> start 0, no marker. */
int chat_composer_viewport(const uint8_t *adv, int n, int avail_px, int marker_px, bool *marker);
```

Pure C, no gfx dependency. Walk from the end accumulating advances; the first suffix whose width
exceeds `avail_px - marker_px` stops the walk. Exact fit of the whole text needs no marker.
**Verify:** tests in `test_chat_layout.c`: empty; fits exactly; one px over (marker, drops one
codepoint); very narrow span (only the last glyph fits); marker wider than span (returns n, marker
true — renderer draws only the marker); advances of 0 (tofu/zero-width) do not loop forever.
`make -C firmware/host test` green.

### 1.2 Composer renderer: tail-scroll, counter at ≥120, caret — firmware-dev

**Read:** 1.1's helper, `firmware/main/scr_chat.c` 730-780, `firmware/main/gfx.c` 340-410
(`blit_glyph`, `gfx_text`, `gfx_text_width`, `glyph_adv`), `firmware/host/render_png.c` 300-330 and
410-420 (the composer fixture), `docs/DEVICE_PLAN.md` 822-840 (spec + mockup).
**Files:** `firmware/main/scr_chat.c`, `firmware/main/gfx.c/.h` (one new accessor), `render_png.c`.
**Do:**
1. Add `int gfx_glyph_advance(gfx_font_t font, uint32_t cp)` to gfx (thin wrapper over the existing
   internal lookup) so the renderer can fill the `adv[]` array per codepoint. Reuse whatever UTF-8
   decoder `gfx_text` already uses; do not write a second one.
2. In the composer draw: `pen = gfx_text(0, y, NORMAL, "> ")`; decide `show_counter = (codepoints
   >= 120)`; `avail = GFX_SCREEN_W - pen - (show_counter ? counter_w + 6 : 0) - caret_w`; call the
   helper; draw `…` if flagged, then the suffix from `start`; draw a 2 px × glyph-height caret at the
   pen; draw the counter right-aligned only when `show_counter`.
3. Update `render_png.c`'s composer fixture and add one long-draft fixture (70 chars) so the PNG
   shows the marker and the fold.
4. Update the comment at `scr_chat.c:741-747` (it describes the old overflow behaviour) and add one
   line to `docs/DEVICE_PLAN.md` §5's composer bullet: "longer than the line: shows the tail with a
   leading …; the counter appears from 120 characters."
**Verify:** builds clean; host tests green; `render_png` output eyeballed for the short and long
fixtures. Do not flash.

### 1.3 Bench acceptance — bench-tester, owner at the glass

**Read:** `docs/HARDWARE_TESTING.md` (bench rules, `disptest`, `key`), `docs/V03_PLAN.md` §1 accept
criteria, 1.0's report (whether `key <long text>` is usable).
**Do:** flash the 1.2 build. `wake`, `key \n`, then either `key` with a 70-character string (if 1.0
fixed it) or ask the owner to type ~70 characters on the CardKB. Owner reports: the `…` marker, no
overprint, the counter appearing at 120 (type to 125), backspace revealing hidden text, `enter`
sending the whole text (check in the web thread). Capture every `partial:` line — ordinary keystrokes
must now be 8-16 row bands (the counter island is gone below 120).
**Verify:** owner's read plus the band widths in the log. Record the result in
`docs/HARDWARE_TESTING.md` "Seen working".

### 1.4 Partial refresh time: measure lever 1 (0xFC vs 0xFF) — DEFERRED (owner, 22 Sep: "stay with 0.45 s, it's okay for now")

Not scheduled. Kept here so the measurement plan is ready if the owner reopens it. Agents: skip.

**Read:** `docs/V03_PLAN.md` §1 "Refresh time", `firmware/main/disp.c` `partial_refresh_locked()`
and the `disptest` command in `main.c`, `docs/reference/wf_partial_2in9.h` (lever 2, reference only).
**Files:** `firmware/main/disp.c/.h`, `firmware/main/main.c`.
**Do (firmware-dev):** add `disp_set_partial_update_byte(uint8_t)` (default 0xFF) and a
`disptest u22 <hex>` subcommand; print the byte in `disptest info` and in the `partial:` log line.
Nothing else changes. Build, host tests.
**Do (bench-tester, one flash, four looks at most):** `disptest bars`, `disptest seq 0 12 1500` at
`u22 ff`, then the same at `u22 fc`. Report BUSY elapsed per partial for both, and whether the panel
draws the same pattern (owner reads `b w ...` both times). Also note whether anything changes when
the panel sits idle for 60 s after a `0xFC` partial (with analog left on) and then a full refresh runs.
**Verify:** a table of elapsed times; a recommendation. If `0xFC` is materially faster and clean,
make it the default in a follow-up commit; if not, leave 0xFF and close lever 1. Lever 2 (host LUT)
is a separate decision by the owner, not part of this task.

---

## 2. Web chat scrolls with new messages

### 2.1 Auto-scroll — web-dev

**Read:** `web/app/chat/[alias]/ThreadPageClient.tsx` (all of `ThreadInner`, esp. 149-200 listener,
354 outer Stack, 401 scroll Box, 402-408 "Load older", 409 map, 417-442 composer),
`web/components/NotificationWatcher.tsx:40-70`, `docs/V03_PLAN.md` §2.
**Files:** `ThreadPageClient.tsx`; a new small `web/components/NewMessagesChip.tsx` if you want it
separate.
**Do:**
1. `const listRef = useRef<HTMLDivElement>(null)` on the scroll Box; `const atBottomRef =
   useRef(true)`; an `onScroll` handler sets `atBottomRef.current = scrollHeight - scrollTop -
   clientHeight < 80`.
2. Keep `prevSeqRef = { first, last }` of the last rendered array. In a `useLayoutEffect` on
   `messages`: if there was no previous array → `scrollTo({ top: scrollHeight })` (instant). Else if
   `newLast > prevLast` (append) → if `atBottomRef.current || lastMessage.senderUid === me.uid` →
   scroll to bottom (`behavior: reducedMotion ? "auto" : "smooth"`), else `setShowChip(true)`. Else if
   `newFirst < prevFirst` (prepend from Load older) → `scrollTop += scrollHeight - prevScrollHeight`
   (read `prevScrollHeight` right before `pageSize` changes).
3. Chip: absolutely positioned over the list's bottom edge, "New messages ↓", `onClick` scrolls to
   bottom and hides; also hide when the scroll handler sees the bottom.
4. `prefers-reduced-motion` via `useMediaQuery`.
**Verify:** `npm run lint && npm run build`. Manual per 2.3.

### 2.2 Mark-read gating — web-dev

**Read:** `ThreadPageClient.tsx:286-298` (mark-read), 2.1's `atBottomRef`,
`NotificationWatcher.tsx:56-62` (the visibility predicate — reuse the same shape).
**Files:** `ThreadPageClient.tsx`.
**Do:** mark read only when `atBottomRef.current && document.visibilityState === "visible"`; add
`visibilitychange` and the scroll-to-bottom transition as triggers so a thread left off-screen is
marked read when the viewer actually reaches the bottom. Keep the existing dedupe (`markedReadRef`).
**Verify:** lint/build; manual per 2.3. Note the behaviour change in `web/README.md` next to the
unread-badge paragraph (192-195).

### 2.3 Manual verification script — docs-writer, then the owner

**Do:** add a numbered checklist to `web/README.md` under the existing "live listener" steps
(180-195): opens at bottom; pager reply follows; scrolled-up + new message → chip, badge stays;
click chip → scrolls, badge clears; Load older holds position; reduced-motion honoured. Note that
there is no test runner (`ROADMAP.md:54`) so this list is the test.

---

## 3a. Push notifications for chat messages

### 3a.1 Real FCM client in the relay — backend-dev

**Read:** `relay/app/backends/webapp.py` (whole file; `NullFCMClient` 50-55, `deliver()` 65-85),
`relay/app/backends/registry.py` 1-30, `relay/app/main.py` ~55-70, `relay/app/store/push_tokens.py`,
`relay/app/settings.py` (or wherever env settings live), `docs/SERVER_PLAN.md` 550-590 and 700-720.
**Files:** new `relay/app/backends/fcm.py`, `webapp.py` (only to widen the client protocol if
needed), `registry.py`, `main.py`, settings module, tests under `relay/tests/`.
**Do:** `FirebaseFCMClient.send_data(tokens, data)` using
`firebase_admin.messaging.send_each_for_multicast(MulticastMessage(tokens=..., data=...,
webpush=WebpushConfig(headers={"Urgency": "high", "TTL": "14400"})))`. Chunk tokens at 500. For each
failed response: `UnregisteredError` or `SenderIdMismatchError` → delete that token via the existing
store; anything else → log at INFO and keep. Setting `PUSH_BACKEND` = `fcm` | `null`, default `null`;
`main.py` passes the chosen client into `build_registry`. All `data` values must be `str`.
**Verify:** pytest with `firebase_admin.messaging` monkeypatched: success; one unregistered token
deleted, others kept; transient error keeps the token; chunking at 501 tokens; `PUSH_BACKEND=null`
constructs the null client. Existing e2e suite unchanged and green.

### 3a.2 Payload contract and the service worker — backend-dev (relay side), web-dev (SW)

**Read:** `docs/SERVER_PLAN.md` §7.6 (708-715), `relay/app/backends/webapp.py:65-85`,
`web/public/firebase-messaging-sw.js` (whole file), `web/lib/notifications.ts`,
`web/lib/firebase-messaging.ts`, `web/.env.local.example`, `web/package.json` scripts,
`web/next.config.ts`.
**Files:** `SERVER_PLAN.md` §7.6, `webapp.py`, `web/public/firebase-messaging-sw.js` → becomes
`web/sw/firebase-messaging-sw.template.js` + a `web/scripts/gen-sw.mjs` prebuild that writes
`public/firebase-messaging-sw.js`; `.gitignore` the generated file; `package.json` (`prebuild`).
**Do:** contract, all string values: `kind` (`message`|`geofence`), `convKey`, `id`, `senderUid`,
`senderAlias`, `title`, `body`, `url`. Relay sends `title = senderAlias`, `body` = first 120 chars,
`url = /chat/{alias}`. SW: `onBackgroundMessage` switches on `kind`, uses `title`/`body`/`url`;
`notificationclick` focuses an existing client at `url` or opens it. The generator substitutes the
six `NEXT_PUBLIC_FIREBASE_*` values into the template; fail the build if any is missing (no more
demo config, ever). Add `NEXT_PUBLIC_FIREBASE_VAPID_KEY` to `.env.local.example` with a comment on
where it comes from (3a.3).
**Verify:** relay tests updated for the new fields; `npm run build` produces a SW with the real
project id and no `"0"` sender id; `grep -n '"0"' web/public/firebase-messaging-sw.js` is empty.

### 3a.3 Secrets and deploy — infra-dev, plus one manual step by the owner

**Read:** `.github/workflows/deploy.yml` (web build/deploy steps ~200-240), `infra/` for the relay
service env (`PUSH_BACKEND`), `web/README.md` env section.
**Files:** `deploy.yml`, relay Terraform env block, `web/README.md`, `infra/README`.
**Do:** pass `NEXT_PUBLIC_FIREBASE_VAPID_KEY` from a repository secret into the web build; set
`PUSH_BACKEND=fcm` on the Cloud Run service. Document the manual step: Firebase console → Project
settings → Cloud Messaging → Web Push certificates → Generate key pair → paste the public key into the
`NEXT_PUBLIC_FIREBASE_VAPID_KEY` secret. Terraform validate only.
**Verify:** `terraform validate`; workflow lints; a deploy after the secret is set produces a SW
with real config (3a.2's grep, run against the deployed file).

### 3a.4 Manual verification — owner, with docs-writer recording

Desktop Chrome: enable at `/settings/notifications`, close the tab, have the pager reply → system
notification → click opens the thread. Android Chrome: same, installed as PWA. iOS Safari: install to
Home Screen first (3b.2's hint), then enable, then test; record the result in `web/README.md` even if
it fails — that is the known iOS caveat and the owner needs to know where it stands.

---

## 3b. Installability

### 3b.1 Icons and manifest — web-dev (icons may need the owner's artwork)

**Read:** `web/public/manifest.webmanifest`, `web/public/icons/`, `web/app/layout.tsx:10-20`.
**Do:** real 192 and 512 PNGs plus a 512 `purpose: "maskable"`; add `id: "/"` and `scope: "/"`. If
no artwork exists, a flat two-colour glyph on the theme colour is fine — mark it as placeholder art in
the commit message so it can be replaced.
**Verify:** Chrome Application → Manifest shows no warnings; installs on desktop and Android.

### 3b.2 iOS standalone hint and Chromium install button — web-dev

**Read:** `web/app/settings/notifications/page.tsx` 60-96, `web/components/AppShell.tsx`.
**Do:** detect `!window.matchMedia("(display-mode: standalone)").matches` on iOS Safari and show a
one-line hint with the Share → Add to Home Screen steps on the settings page; capture
`beforeinstallprompt` on Chromium and show an Install button there. No caching service worker (see
`V03_PLAN.md` §3b for why).
**Verify:** lint/build; by hand on an iPhone and on Chrome desktop.

---

## 3c. Geofence notifications

### 3c.0 Gates — bench-tester (i), backend-dev (ii), owner (iii). Do these before any 3c code.

(i) With the pager registered, capture `+CEREG` URCs while the owner carries it around the block and
back: note the `<tac>`,`<ci>` values, when they change, and how long each cell dwells. Save the log
and a two-line summary in `docs/HARDWARE_TESTING.md`.
(ii) Feed those cell ids through `relay/app/cellgeo.py` with a real `CELL_GEO_API_KEY` (Google
Geolocation) and report lat/lon/`accM` per cell. If OpenCelliD is to be used, verify its request/
response shape first (`ROADMAP.md:26-27`).
(iii) Owner states the fence(s) wanted (place, radius). Compare radius to the `accM` from (ii). If
`accM` ≥ radius for the relevant cells, stop and discuss the scheduled-GNSS fallback and its
24 mAh/day cost (`PROTOCOL.md:1444-1446`) before building anything.

### 3c.1 Firmware: serving-cell-change report — firmware-architect (design), firmware-dev (build)

**Read:** `firmware/main/loc.c` (the locate answer path and the queued-until-session-usable
behaviour), `firmware/main/net.cpp` (where `+CEREG` is parsed / the network event handler),
`docs/PROTOCOL.md` location sections, `docs/V03_PLAN.md` §3c.
**Do:** on a serving-cell change stable for ≥ 60 s, publish a location report with `src: cell`,
`reason: cell_change`, rate-limited to one per 10 min, behind `PAGER_LOC_CELL_CHANGE_REPORTS`
(default off). Reuse the existing answer format and queueing; no GNSS. Host test for the
debounce/rate-limit logic as a pure helper.
**Verify:** host tests; bench: toggle the flag on, repeat (i) with the relay logging the reports;
data per report under 1 KB (`poll_emqx_loop.py` counters).

### 3c.2 Relay: fences, evaluation, push — server-architect (review), backend-dev (build)

**Read:** `relay/app/location.py` (`ingest_loc` 184-320, `_fix_doc`), `relay/app/store/locations.py`,
`firestore.rules`, 3a.1's client, `docs/V03_PLAN.md` §3c data model and hysteresis.
**Do:** `devices/{deviceId}/geofences/{fenceId}` store; evaluate all fences on every stored fix;
enter when `dist + accM <= radiusM`, exit when `dist - accM >= radiusM`, else hold; state changes
write `state/since/lastFix` and send `kind: geofence` push to `notifyUids`. Haversine is enough.
Rules: read/write for users with `allow.locate` on the device's conversation. PROTOCOL.md gets the
`reason` field and the fence doc shape.
**Verify:** pytest: inside/outside/ambiguous, no flapping across an `unknown` fix, push sent once per
transition, rules tested with the emulator if the e2e suite already does that.

### 3c.3 Web: fence editor — web-dev

**Read:** `web/components/LocationCard.tsx`, `web/components/LocationMap.tsx` (Leaflet; marker,
accuracy circle, trail), `web/lib/types.ts`, `web/lib/location.ts`.
**Do:** Geofences panel under the map: click to place, radius slider (100 m-5 km), name, notify
toggle per user in the conversation; list with state and the last fix's `accM` shown beside the
radius; delete. Firestore reads/writes per 3c.2's rules.
**Verify:** lint/build; by hand.

### 3c.4 Acceptance — owner

A fence around the bench and one elsewhere; carry the pager out and back: one `left`, one
`arrived`, no flapping; fence list shows states; per-report data under 1 KB. Record in
`docs/HARDWARE_TESTING.md`.
