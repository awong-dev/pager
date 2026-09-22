# v0.3 plan — composer overflow, web auto-scroll, push + geofence

Written 22 Sep 2026 against `main` at `5150fc0` (v0.2 = `9d4feb8`). Companion task list for the
implementing agents: `docs/V03_TASKS.md`. Every "today" statement below was checked in the code on
this date; file:line references are to that commit.

Three asks from the owner, in the owner's order:

1. Typing beyond the end of the screen width on the device.
2. The web chat scrolls with newly received messages.
3. The web app is an installable PWA with desktop and mobile notifications for new chat messages
   and for geofence location changes.

Recommended build order is **2, 1, 3a, 3b, 3c** — smallest and web-only first, then the firmware
change (one bench check), then the push wiring that unlocks the rest, and geofencing last because it
is the only item that needs new design (nothing geofence-shaped exists today).

---

## 1. Composer overflow on the device

### Today

- The reply buffer is fine: 160 codepoints / 320 bytes, enforced atomically in `msg.c:85-86`, and
  the whole text is always sent. The problem is purely the viewport.
- `scr_chat.c:772-774` draws `> ` at x=0, then the text, then the `N/160` counter right-aligned at
  x=264 (`GFX_SCREEN_W - 32`), all on one line, no wrap, no scroll, no clip. `gfx_text()` walks off
  the right edge dropping pixels one at a time (`gfx.c:375-393`; only `gfx_set_pixel()` bounds-
  checks). The source comment at `scr_chat.c:741-747` calls this "a cosmetic viewport limitation";
  `DEVICE_PLAN.md` is silent on overflow.
- Because the counter shares the line, long text **overprints the counter from about character 29**,
  well before the 296 px edge. The font is proportional (5-8 px per glyph, `glyph_adv()`), so a line
  holds roughly 35-45 characters.
- There is no caret. The `_` in the `DEVICE_PLAN.md:836` mockup is not implemented.
- Vertical room: ~13 px spare at the normal font, ~25 px at large (`scr_chat.c:677-680`). A second
  composer line fits at large; at normal it costs one message row.
- Side finding from tonight's bench work (see `GOTCHAS.md`): the counter's leading digit changes on
  every keystroke ~250 px from the new glyph, so each keystroke's partial refresh spans 232-256 native
  rows. **This costs nothing** — partial BUSY time measured flat at ~447 ms from 8 rows to 296 —
  so it is not a performance task. It is still a reason to move the counter, because of the overprint.

### Refresh time: where the 0.45 s comes from, and the two levers

The panel is a **GDEY029T94-FL03** (front-lit GDEY029T94, SSD1680Z8, 0-50 °C;
good-display.com/product/346.html), specified at full 2 s / fast 1.5 s / **partial 0.3 s**. We measure
partial ≈ 0.447 s (six runs, flat across band heights) and full ≈ 1.40 s tonight, 3.43 s in an
earlier session.

- The 0.45 s partial is what the **OTP mode-2 waveform** delivers: GxEPD2's `GxEPD2_290_T94` (same
  panel family, `0x22 = 0xFC`) measures 0.458 s, and ours is 0xFC plus the analog+clock shutdown
  bits. The vendor's 0.3 s is their demo's **host-loaded partial LUT** (153 bytes written with `0x32`,
  then `0x22 = 0xC0`/`0x20` to power up and `0x22 = 0x0F`/`0x20` per update — the Waveshare
  `EPD_2in9_V2` sequence). Two levers, in order of risk:
  1. **`0xFC` instead of `0xFF`** for partials: keeps analog and clock powered between partials
     instead of shutting them down and re-ramping on the next one. No waveform change, no risk to the
     panel; gain unknown until measured. Costs standby power between partials while typing, which is
     already the awake state.
  2. **Host-loaded partial LUT**: the vendor path. Byte-exact table in `docs/reference/wf_partial_2in9.h`
     (verified against Waveshare C and GxEPD2 sources). Real gain if the vendor's 0.3 s is right,
     but a wrong or mis-sequenced LUT ghosts or stresses the panel, and it interacts with the
     temperature handling. Only after lever 1 is measured and only with `disptest again 0/1`-style A/B
     on one flash.
- The 1.40 s vs 3.43 s full refresh: OTP waveforms are **temperature-indexed** (internal sensor at
  `0x18`, loaded by bit 5 of `0x22`); a colder panel selects a longer full waveform. INFERRED, not
  verified — MISO is not wired (`disp.c` `miso_io_num = -1`), so the sensor cannot be read back. The
  practical consequence is that `PAGER_UI_BUSY_FALLBACK_FULL_MS` (3500) must keep the *slow* figure.

Task `1.4` in `V03_TASKS.md` makes the `0x22` byte selectable from `disptest` and measures lever 1
against the existing harness; lever 2 stays a described option until lever 1's number is in.

### Decision: single line, show the tail, counter only near the cap

- **Horizontal tail-scroll.** When the text is wider than the available span, draw the longest
  suffix that fits, preceded by a `…` marker so hidden text is visible as hidden. The composer has no
  cursor editing (only append and backspace at the end), so the tail is always the part being edited.
  This keeps the one-line layout, the message-row count, the `render_png` fixtures and the
  `DEVICE_PLAN.md` mockup intact.
- **Counter appears only when it matters**: draw `N/160` only when `N >= 120` (40 remaining). Below
  that the text gets the full span. When shown, the available span shrinks by the counter width plus
  a 6 px gap, and the tail-scroll accounts for it. This also removes the right-edge dirty island for
  ordinary typing (each keystroke then refreshes an 8-16 row band), which is tidy even if it is not
  faster.
- **Caret**: add a 2 px vertical bar after the pen, as the mockup shows. It moves with the new glyph,
  so it is free. Optional; drop it if the visual is noisy on the glass.
- **Not chosen**: wrapping to two lines. It changes the layout (loses a message row at the normal
  font), touches `visible_rows()`, `render_png.c:317-323,414-416` and the mockup, and a pager reply
  does not need the whole draft visible.

### Testability

The viewport choice is a pure function of per-codepoint advances, the available width and the
marker width — so it lives next to `chat_build_rows()`'s helpers and gets host tests without
linking `gfx.c` (which the 14 host suites do not link). The renderer supplies the advances.

### Accept when

- On the glass: type 60+ characters on the CardKB; the line shows `… <tail>`, never overprints,
  the counter appears at 120, and `enter` sends the full text (check the web thread). Backspace past
  the fold reveals hidden text again.
- Host: the viewport helper's tests cover fits / one-over / exact-fit / counter-shown / empty /
  multibyte codepoints.

### Prerequisite worth clearing first

The debug console's `key <text>` command with more than one character produced **no redraw and no
`partial_count` increment** in tonight's Run G (`build/bench-logs/phaseG.log`), while typing on the
CardKB works. Typing 60 characters by hand is fine for acceptance, but bench automation of this item
needs `key` to work for multi-character input. Timeboxed task in `V03_TASKS.md` (1.0).

---

## 2. Web chat scrolls with new messages

### Today

- Thread view: `web/app/chat/[alias]/ThreadPageClient.tsx`, `ThreadInner` (line 149). The list is a
  fixed-height `overflowY: "auto"` Box (line 401) inside a `calc(100vh - 140px)` Stack (line 354);
  the composer is a sibling below it (417-442), so it stays pinned while the list scrolls. Ordering is
  newest at the bottom (query `seq desc`, re-sorted ascending at line 196).
- **No scrolling logic at all**: zero hits for `scrollIntoView|scrollTop|scrollTo` in `web/`. The
  container opens at `scrollTop = 0` — the *oldest* loaded message — and new messages append below
  the fold.
- Each snapshot replaces the whole array (line 197); "Load older" grows `pageSize` and re-subscribes
  (402-408) rather than paging with a cursor, so older messages arrive as a prepend.
- Mark-read fires on every incoming message regardless of scroll position or tab visibility
  (286-298). `NotificationWatcher.tsx:56-62` already has the "visible and this thread is open"
  predicate that a foreground notification suppresses on.
- No web test runner (`ROADMAP.md:54`); verification is by hand.

### Decision: standard chat semantics

1. First snapshot with data: jump to the bottom (no animation).
2. New message appended: if the viewer was at the bottom (within 80 px) before the update, or the
   message is the viewer's own, scroll to the bottom (smooth, `auto` under `prefers-reduced-motion`).
   Otherwise hold position and show a "New messages ↓" chip over the list; clicking it scrolls down.
3. Older messages prepended by "Load older": preserve the viewer's place by adjusting `scrollTop` by
   the change in `scrollHeight` in a layout effect.
4. Mark-read: gate on `atBottom && document.visibilityState === "visible"`, and run it when the
   viewer scrolls to the bottom or the tab becomes visible. This is a behaviour change (today,
   opening the thread marks everything read even if it is off-screen) but it is what the chip makes
   honest; it uses the same predicate as `NotificationWatcher`. The unread count only feeds the web
   list badge (`conversations/*.unread[uid]`), nothing on the pager.

Append vs prepend is decided by comparing the first and last `seq` of the previous and new arrays,
not by array length.

### Accept when (by hand, two browsers or one browser plus the pager)

Open a thread with more than a screenful of history: it opens at the bottom. Reply from the pager
(`wake`, `key \n`, type, `enter`): the message appears and the view follows. Scroll up, have another
message arrive: the view holds and the chip appears; click it and it scrolls down; the unread badge on
the list page clears only then. "Load older" does not jump the view.

---

## 3. PWA, notifications, geofence

### Today (this is the part the docs overstate)

Installable-PWA scaffolding and the whole push code path exist, but push has never been live:

- Manifest: `web/public/manifest.webmanifest` (`display: standalone`, `start_url: /chat`, 192/512
  icons) linked from `app/layout.tsx:10-14`. The icon files are 413 B and 1.5 KB placeholders; no
  maskable variant. No install prompt handling anywhere.
- Service worker: `web/public/firebase-messaging-sw.js` — push only (`onBackgroundMessage` →
  `showNotification`, `notificationclick` → `/chat/{alias}`), no caching or `fetch` handler. **It
  hardcodes demo Firebase config with `messagingSenderId: "0"`** (lines 15-22, and its own comment
  says so). Served at the origin root with `Service-Worker-Allowed: /` (`firebase.json:28-33`).
- Web token flow works: `lib/notifications.ts:22-37` gets an FCM token with the VAPID key from
  `NEXT_PUBLIC_FIREBASE_VAPID_KEY` (unset everywhere) and POSTs it to `/api/me/push-tokens`; the relay
  stores it at `users/{uid}/pushTokens/{token}` (`relay/app/store/push_tokens.py:33`). The
  `/settings/notifications` page handles permission, unsupported browsers, and carries the iOS
  "install to Home Screen first" text (`SERVER_PLAN.md:715`).
- Relay send: `backends/webapp.py:65-85` calls `fcm.send_data(tokens, {convKey, id, senderUid,
  body})` on every message routed to a web user — but the only client is `NullFCMClient`, and
  `main.py:61` builds the registry without one, so it is a no-op by construction
  (`SERVER_PLAN.md:585` documents the stub). `firebase_admin.messaging` is imported nowhere. IAM for
  the relay service account is already in place (`infra/modules/relay-service/main.tf:40`).
- Payload mismatch: the SW reads `data.senderAlias` / `data.title`; the relay sends `senderUid`.
- Token hygiene ("remove after 3 consecutive unregistered errors", `SERVER_PLAN.md:557`) is not
  implemented.
- Foreground: no `onMessage`; the app uses a Firestore listener (`NotificationWatcher.tsx`) and
  shows a browser `Notification` itself unless the thread is open and visible. Keep that.
- Location: on-demand only, from a web button gated on `allow.locate`
  (`ThreadPageClient.tsx:331-374` → `POST /conversations/{alias}/locate`). Fixes are stored at
  `devices/{deviceId}/locations/{autoId}` as `{ts, fixTs, lat, lon, accM, src: gnss|cell, cached,
  reqId}` (`relay/app/store/locations.py:17-33`). Cell answers **are** resolved to lat/lon
  server-side (`relay/app/cellgeo.py`, Google Geolocation or OpenCelliD, cached in `cells`) — the
  OpenCelliD provider is unverified and production needs a real `CELL_GEO_API_KEY`. The web already
  has a Leaflet map with marker, accuracy circle and trail (`LocationMap.tsx`); `web/README.md:100-102`
  is stale in saying otherwise.
- **Geofence: nothing.** No code, no data model, no doc proposes one.
- Power bound on any periodic location: `PROTOCOL.md:1444-1446` — one 30 s fix ≈ 0.25 mAh, a
  15-minute cadence ≈ 24 mAh/day, about +25 % on the 95-107 mAh/day sleep budget. Energy per GNSS fix
  is still OPEN (`:1488`). `ROADMAP.md:80` defers periodic fixes.

### 3a. Make push real (chat messages)

Decision: keep the architecture as built; wire the two missing ends and fix the contract.

- **Relay**: a `FirebaseFCMClient` implementing the existing `send_data(tokens, data)` seam with
  `firebase_admin.messaging.send_each_for_multicast` (data-only, `webpush` headers `Urgency: high`,
  `TTL` a few hours so a phone that was off still gets the latest). Inject it in `main.py`'s
  `build_registry(...)` behind a setting (`PUSH_BACKEND=fcm|null`, default `null` in dev and tests so
  the e2e suite keeps running against the null client). On `UnregisteredError` /
  `SenderIdMismatchError` delete the token document; on transient errors do nothing. Implement the
  hygiene rule in `SERVER_PLAN.md:557` as "delete on the first `unregistered`" and change the doc —
  three-strike counting buys nothing for a token FCM has already declared dead.
- **Payload contract**, written into `SERVER_PLAN.md` §7.6 and honoured by both sides:
  `kind` (`message` | `geofence`), `convKey`, `id`, `senderUid`, `senderAlias`, `title`, `body`,
  `url` (path to open). The SW switches on `kind`. All values strings (FCM data maps are string-only).
- **Web**: generate `firebase-messaging-sw.js` at build time from the same `NEXT_PUBLIC_FIREBASE_*`
  variables the app uses (a `prebuild` script writing `public/firebase-messaging-sw.js` from a
  template), so the static export never ships demo config again. Add
  `NEXT_PUBLIC_FIREBASE_VAPID_KEY` to `.env.local.example` and to the deploy workflow's secrets.
  Keep the Firestore-listener foreground path.
- **One-time manual steps** (owner): create the Web Push certificate (VAPID key pair) in the Firebase
  console → Cloud Messaging; put the public key in CI as `NEXT_PUBLIC_FIREBASE_VAPID_KEY`. No new
  GCP resources are needed.
- **Verify**: relay unit tests with a fake `messaging` module (success, unregistered → token
  deleted, transient → kept). By hand: enable on desktop Chrome, close the tab, have the pager reply
  (`wake`, `key \n`, type, `enter`) — a system notification arrives and clicking it opens the thread.
  Repeat on Android Chrome and on iOS Safari **after** installing to the Home Screen; document the
  iOS result either way in `web/README.md`.

### 3b. Installability polish

- Real icons at 192 and 512 plus a `purpose: "maskable"` 512, replacing the placeholders. Add
  `id` and `scope: "/"` to the manifest.
- Check installability in Chrome's Application panel on the deployed site. Chrome no longer
  requires a `fetch` handler for the install prompt; do **not** add a caching service worker to a
  static export whose HTML is not content-hashed — a stale shell after a deploy is a worse bug than no
  offline mode, and nothing here needs to work offline.
- A small in-app hint on iOS Safari when not running standalone (`display-mode: standalone` media
  query), pointing at Share → Add to Home Screen, next to the existing notification-settings text.
- Optional `beforeinstallprompt` capture on Chromium to offer an Install button on the settings page.

### 3c. Geofence notifications

Nothing exists, so this needs a design decision before code, and the decision is dominated by power.

**Decision: fences are evaluated on the relay from whatever fixes it receives; the trigger for new
fixes is a serving-cell change reported by the firmware, not a timer.** The modem already tells the
host its cell (`+CEREG: 2,5,"<tac>","<ci>",...` is in every bench log) and the ESP32 already wakes
every 5 s to drain the modem; a cell change is therefore observable at zero extra radio-on time.
A cell change is also exactly the event a fence cares about at the granularity cell location can
deliver ("left the school's cell" / "arrived at the home cell"). GNSS stays on-demand for precision.

- **Firmware**: on a serving-cell change that persists for ≥ 60 s, publish a location report with
  `src: cell` and a new `reason: cell_change` field, reusing the existing locate-answer path
  (`loc.c`, the queued-until-session-usable behaviour from `eb6bb5d`). Rate-limit to one report per
  10 minutes so a train ride does not become a data bill. No GNSS. Behind a config flag, default off
  until the relay side exists.
- **Relay**: `devices/{deviceId}/geofences/{fenceId}` = `{name, lat, lon, radiusM, notifyUids[],
  state: inside|outside|unknown, since, lastFix}`. On every stored fix (any `src`), evaluate each
  fence with accuracy-aware hysteresis: **enter** when `dist + accM <= radiusM` (confidently inside),
  **exit** when `dist - accM >= radiusM` (confidently outside), otherwise keep the previous state.
  A state change sends `kind: geofence` push to `notifyUids` with `title` "<name>: arrived/left".
  Because cell accuracy is hundreds of metres to kilometres, a fence radius smaller than the local
  cell accuracy will simply sit in `unknown`; the web editor shows the last fix's `accM` next to the
  radius so the owner can see why.
- **Web**: a Geofences panel on the location card using the existing Leaflet map — click to place,
  radius slider, name, who to notify — and a fence list with current state. Firestore rules: fences
  are writable by users with `allow.locate` on the device's conversation; readable by the same.
- **Gates before building the UI** (cheap, in this order): (i) confirm on the bench that the
  serving cell changes observably when the pager moves between two known places (walk it around the
  block; watch `+CEREG`), and how long it dwells; (ii) confirm the production geo provider resolves
  those cells to positions with a stated `accM` (needs `CELL_GEO_API_KEY`; OpenCelliD is unverified —
  `ROADMAP.md:26-27`); (iii) decide the fence radius the owner actually wants ("at school") and check
  it against the observed `accM`. If (ii) fails, geofencing is not buildable at this power budget and
  the fallback is a scheduled GNSS fix (24 mAh/day at 15 min), which the owner must consciously accept.

### Accept when

- 3a: a pager reply raises a notification on a closed desktop Chrome tab and on an installed Android
  PWA; iOS result documented.
- 3b: the site installs from Chrome desktop and Android without warnings in the Application panel;
  installed app opens at `/chat`.
- 3c: with a fence around the bench and a second fence elsewhere, carrying the pager out and back
  produces one `left` and one `arrived` notification, no flapping, and the fence list shows the
  states. Data cost per cell-change report logged and under 1 KB.

---

## Open items carried from 22 Sep (not part of this plan, recorded so they are not lost)

- Debug console `key <multi-char>` feeds nothing to the composer (Run G). Console path only.
- A healthy full refresh measures ~1.40 s BUSY across six runs; an earlier session recorded 3.43 s
  and `PAGER_UI_BUSY_FALLBACK_FULL_MS` (3500) rests on that. Unexplained; do not change the constant
  on an unexplained number.
- `PAGER_UI_PARTIAL_ROW_ALIGN` stays at 8 for the reason now written in its comment; retire it only
  after a bench run shows unaligned windows stay clean.
