# Pager web app

Next.js (App Router, `output: 'export'`) + MUI + the Firebase JS SDK --
docs/SERVER_PLAN.md §7. No SSR, no server actions, no middleware: every page
is a client component; every read is a Firestore listener; every write is a
`fetch` to `/api/*` with `Authorization: Bearer <Firebase ID token>`
(`lib/api.ts`). Never write to Firestore from the browser.

## Stack

- Next 16 (App Router, `output: 'export'`), React 19, TypeScript.
- MUI 9 (`@mui/material`, `@mui/icons-material`, `@mui/material-nextjs` for
  the emotion cache provider). No other UI kit.
- `firebase` 12 (`auth`, `firestore`, `messaging`).
- No state library beyond React context (`lib/auth-context.tsx`,
  `lib/directory.tsx`) -- Firestore listeners are the state.

## Setup

```bash
cd web
npm install
cp .env.local.example .env.local   # values already match relay/docker-compose.yml
```

`.env.local.example`'s `NEXT_PUBLIC_FIREBASE_VAPID_KEY` (used by `lib/notifications.ts`'s
`registerForPush()` to subscribe for real push notifications, docs/V03_PLAN.md §3a) is empty by
default -- fine for the emulator stack, where push isn't wired up either way. In production, this
comes from the `NEXT_PUBLIC_FIREBASE_VAPID_KEY` **GitHub Actions secret** (not a repo variable,
unlike the other six `NEXT_PUBLIC_FIREBASE_*` values -- see `infra/README.md` step 9), created by
hand: Firebase console → Project settings → Cloud Messaging → Web Push certificates → Generate key
pair → paste the public key in as that secret's value. `.github/workflows/deploy.yml`'s build step
passes it into `npm run build`; leaving the secret unset fails that build, since the prebuild
service-worker generator (3a.2) rejects any missing `NEXT_PUBLIC_FIREBASE_*` value.

## Local dev against the emulator stack

```bash
# from repo root
docker compose -f relay/docker-compose.yml up -d
python3 tools/emqx_setup.py        # one-off per `docker compose up`, provisions the rule engine

# from web/
npm run dev                        # http://localhost:3000
```

`next.config.ts` rewrites `/api/**` and `/webhooks/**` to
`http://localhost:8000` (the relay) in dev. `lib/firebase.ts` connects to the
Firestore/Auth emulators when `NEXT_PUBLIC_USE_EMULATORS=1` (the
`.env.local.example` default).

To create the first admin user against a fresh emulator stack (the registry
gate means nobody can sign in until they exist as a `users/{uid}` doc --
docs/SERVER_PLAN.md §5.3), run the relay's own bootstrap job from `relay/`,
pointed at the emulators. As of the multi-family tenancy work
(docs/FAMILIES_TASKS.md 1.6), this also creates the default `families/{fid}`
doc and makes `--admin-email` a `super` user (not just a family `admin`):

```bash
cd relay
FIRESTORE_EMULATOR_HOST=localhost:8080 FIREBASE_AUTH_EMULATOR_HOST=localhost:9099 \
  GOOGLE_CLOUD_PROJECT=demo-pager .venv/bin/python -m app.bootstrap \
  --admin-email admin@example.com --alias admin
```

Additional (non-admin) users are then created from `/admin/users` once
signed in as that admin, or via `relay/.venv/bin/python
tools/pager_client.py admin user-add <alias> "<name>" --email/--phone
[--admin] --as admin --api http://localhost:8000` (needs `DEV_MODE=1` on the
relay, already set in `relay/docker-compose.yml`, to sign the script in
without a real email/SMS flow -- see `docs/SERVER_PLAN.md` §8). That script
needs the relay virtualenv's interpreter, not a bare `python3`.

Firebase Auth's email-link sign-in needs a real link click, which the
emulator UI (`http://localhost:4000/auth`) lets you copy without sending a
real email -- see the checklist below.

## Build / typecheck / lint

```bash
npm run build     # next build -> web/out/
npx tsc --noEmit
npm run lint       # eslint
```

`next build` prints `Specified "rewrites" will not automatically work with
"output: export"` -- expected and harmless. Production gets its rewrites
from `web/firebase.json` (Firebase Hosting), not `next.config.ts`; the
config's `rewrites()` only matters to `next dev`.

## Known limitations (read before testing as a non-admin member)

- **Contact alias resolution for non-admin members is best-effort,
  client-side only.** `firestore.rules`' `users/{uid}` read rule
  (`request.auth.uid == uid || isAdmin()`) does not let a regular member
  read a conversation partner's `alias`/`displayName`, and no API route
  exposes it either. `lib/directory.tsx` documents the gap and its
  workaround in full; in short: an **admin's** browser gets the whole
  alias/uid directory for free (it can list `users`); a **member's**
  browser only resolves an alias after the *first* message is sent to it
  from that browser (learned from the message's own doc, which the sender
  may always read back), and remembers it in `localStorage` after that.
  Until then, `/chat`'s contact list shows unresolved peers as
  `uid:xxxxxxxx` and disables opening them -- use the "Open conversation by
  alias" box instead, which works from a cold start because it only needs
  the alias, not the uid.
  The fix is either to loosen `users/{uid}`'s read rule to `registered()`,
  or to add a `GET /api/me/contacts`-shaped endpoint.
- **The notifications "test" button is local-only.** There is no relay
  endpoint that sends a real push on demand, so it only proves permission +
  display work in this browser, not the full FCM round trip.
- **No embedded map** (docs/SERVER_PLAN.md §7.7 explicitly defers this) --
  the location card is lat/lon/accuracy/age + "open in Google/Apple Maps"
  links only.

## Device provisioning (docs/DEVICE_PLAN.md §3.2)

The **Add device** flow (`/admin/devices` -> click the "Add device" button) walks a
household admin through creating a new pager:

1. Enter the **Device ID** (e.g. `pgr-0001`), a **Label** (e.g. "Kid's pager"), select the
   **Owner** (the student who will use it), and optionally a **Default recipient** (where
   messages go if not explicitly addressed).
2. Click **Create**. The page generates a one-time **setup code** (40–55 characters), displays
   it as both text (for typing) and a QR code (for camera scan), and shows a countdown
   expiring in 10 minutes. The code encodes a device ID, the relay's broker hostname, and a
   secret bootstrap token.
3. The device owner or admin types (or scans) this code into a brand-new pager over LTE. The
   pager decodes it, fetches an encrypted bootstrap bundle from the relay using a key derived
   from the code, decrypts it, stores the device credentials and relay's root CA certificate,
   and publishes its first signed status message.
4. Once the device connects, the setup-code panel's banner flips from "Waiting for the
   device..." to "`<deviceId>` is online" with no page reload.

**Rotate credentials** (click **Rotate** on an existing device) follows the same flow: a new
code is issued, the old device credential is revoked at the broker, and the device must fetch
a fresh bundle. **Revoke** (click **Revoke**) immediately deletes the broker credential so a
lost or stolen device cannot connect; a new setup code and full provisioning step is needed if
the device is later recovered or replaced.

**Lock controls** (on the same row): **Auto-lock** (dropdown menu, default 5 min) sends the
device a policy to automatically lock after N minutes of inactivity; **Clear passcode** (button)
immediately removes the passcode on the device, unlocking it.

Contact requests and the address book are managed on `/family/alerts` (see the table in the
main checklist below, step 12) -- superseded from `/admin/contacts`, deleted in
docs/FAMILIES_TASKS.md 5.3.

## Manual checklist (walk through against `docker compose up`)

Prerequisites: `docker compose -f relay/docker-compose.yml up -d`,
`python3 tools/emqx_setup.py`, `npm run dev` in `web/`, an admin user
created (see Setup above). Open two browser profiles (or one normal + one
incognito window) to act as two different people at once where noted.

1. **Registry gate**: sign in with an email that has *not* been registered
   via `admin user-add`/`/admin/users` (email-link flow: enter the email on
   `/login`, open `http://localhost:4000/auth`, find the sign-in link Auth
   generated for that address, open it). Expect: "ask your admin to add
   you" and the app signs you back out.
2. **Admin sign-in**: sign in as the admin the same way. Expect: redirected
   to `/chat`; the Admin menu is visible in the top bar.
3. **Create users**: `/admin/users` -> Create user for two members (e.g.
   `mom`, `student`), one with `--email`, one with `--phone`. Confirm both
   appear in the table.
4. **Allow-list**: `/admin/allowlist` -> check Message + Locate for
   `mom -> student` and `student -> mom` -> Save. Reload the page and
   confirm the checkboxes persisted.
5. **Create a device**: `/admin/devices` -> Add device, owner = student.
   Confirm the setup-code panel appears with the big code, a working Copy
   button, a QR code rendered below it, the two-step "on the pager"
   instructions, and a live "expires in mm:ss" countdown; confirm the code
   is gone on reopen/reload (only Rotate can issue a new one). If the relay
   logs `brokerPush: "manual"` for this device (no EMQX admin API in this
   compose stack), confirm the panel instead shows the manual ACL lines to
   enter by hand.
5a. **Provisioning flips to online**: with the setup-code panel still open,
    run `relay/.venv/bin/python tools/e2e_v2.py setup_code` (or
    `tools/pager_client.py --bootstrap "<the code shown>"`) to simulate the
    device's bootstrap fetch and first signed `/status`. Expect the panel's
    banner to flip from "Waiting for the device to connect..." to
    "`<deviceId>` is online" with no page reload, and the table's
    Provisioned column to read `online`.
5b. **Rotate and revoke, on a second throwaway device** (rotating or
    revoking the `student` device here would break its connection for steps
    6-9 below): Add a second device, e.g. owner = mom, id `scratch-1`. Click
    Rotate -> confirm the same setup-code panel reappears with a fresh
    code/QR/countdown and the table's Provisioned column reverts to
    `issued`. Click Revoke -> confirm the browser confirmation prompt, then
    confirm the table's Revoked column flips to `yes` with no page reload;
    delete `scratch-1` afterwards to keep the table clean for later steps.
6. **Lock controls**: from the same Devices table, adjust the Auto-lock
   dropdown to a different value and confirm the selection persists; click
   Clear passcode and confirm the browser confirmation prompt.
7. **Sign in as a member and send a message**: sign in as `mom` (a second
   browser profile), `/chat` -> "Open conversation by alias" -> `student` ->
   send a message. Expect: the message appears immediately (own
   optimistic-free listener render) with a `webapp: sent` (or similar)
   delivery chip; no pager chip yet if the device has never connected.
8. **Reply and delivery chips**: use `relay/.venv/bin/python tools/e2e_v2.py
   text_roundtrip` (or individual commands with `tools/pager_client.py`) to
   connect the `student` device and ack the message `shown`/`read`; watch
   mom's open thread update the delivery chip live within a second or two,
   with no page reload.
9. **Unread badge**: with mom's thread closed (back on `/chat`), have the
   simulated student device send a message to mom (`msg` command). Expect
   `/chat`'s contact list to show an unread badge, and a browser notification
   if `/settings/notifications` was enabled first and the tab isn't focused.
   A thread now marks messages read only when the viewer is at the bottom of
   the list and the tab is visible; before this change, opening the thread
   marked everything read even if it was off-screen.
10. **On-demand OTA delta**: push to a device on an older published build shows
   "delta (made at push)" in the dialog; after the push the job reads delta.

## Thread auto-scroll behaviour

1. Open a thread with more than a screenful of history → it opens scrolled to
   the bottom (newest message visible), no animation.
2. Have the pager reply (bench: `wake`, `key \n`, type, `enter`; or type on
   the CardKB) while the thread is at the bottom → the new message appears and
   the view follows it.
3. Scroll up a screenful; have another message arrive → the view holds its
   place and a "New messages ↓" chip appears at the bottom edge of the list;
   the conversation's unread badge on the list page (open it in a second tab)
   does NOT clear.
4. Click the chip → the view scrolls to the bottom, the chip disappears, and
   the unread badge clears.
5. Send a message yourself while scrolled up → the view scrolls to the bottom
   (own messages always follow).
6. Click "Load older" → older messages appear above and the message you were
   looking at stays where it was on screen (no jump).
7. With the OS "reduce motion" setting on, repeat 2 → the scroll is instant,
   not animated.
8. Switch to another tab while a message arrives with the thread at the
   bottom, then switch back → it is marked read only once the tab is visible
   again.

10. **Request location**: open mom's thread with student -> "Request
    location" button should be visible (locate was allowed in step 4) ->
    click it -> confirm a `location requested` marker appears in the thread,
    and (once the simulated device answers a `loc` fix) a location card with
    lat/lon and "Open in Google Maps"/"Open in Apple Maps" links.
11. **Backends**: `/settings/backends` → Add Google Chat → the row appears disabled until linked.
12. **Contact requests and address book** (new in device plan): `/admin/contacts`
    shows pending requests from all devices. With the student device running
    (`tools/e2e_v2.py address_book`, or `pager_client.py` REPL `contactreq`
    command), confirm a pending request appears. Click Approve, select Link
    to existing (or Create new contact with the phone number), optionally
    enable Locate. Confirm the device's book syncs (run `pager_client.py`
    `ack shown` command to simulate acking the book). Confirm a second
    request to the same contact is rejected as a duplicate.
13. **Notifications**: `/settings/notifications` -> Enable notifications
    (grant the browser permission prompt) -> Send test notification -> a
    native OS notification should appear.
14. **Retention settings**: `/admin/settings` -> change messages retention
    to `2 weeks`, save, reload, confirm it persisted; read the weekly-sweep
    note.
15. **Sign out**: confirm "Sign out" returns to `/login` and that navigating
    back to `/chat` redirects to `/login` rather than showing stale data.
16. **Soracom SIMs**: Admin -> Devices -> Soracom SIMs lists the account's SIMs;
    Enroll moves one into pager-beam; Enroll all handles the rest; an
    unconfigured relay shows the setup alert.

## Multi-family tenancy checklist (docs/FAMILIES_TASKS.md)

16. **Navigation by role** (1.7): sign in as super -> switcher visible, changing it updates
    `?family=`; as family admin -> Family menu, no Admin menu, no switcher; as member -> neither;
    a role change on the server followed by reload lands on the new nav after the forced token
    refresh.
17. **Superadmin Families page and filters** (1.8): create a second family, move a member into
    it, promote them to admin, sign in as them -> they see only their family on `/family/people`;
    allow-list shows cross-family Locate disabled.
18. **Family People/Devices pages and directory rewrite** (1.9): as a family admin, People lists
    only the family; adding a person appears live; creating a device shows the setup code and the
    device appears in the table with the right owner; as a member, `/chat` peers resolve by alias
    without any `localStorage` key.
19. **Family tab and read-only monitor thread** (2.4): as family admin, Family tab shows a
    member's DM with a user in another family and the member's SMS conversation; opening one shows
    messages read-only; as the other family's admin the same DM appears and the SMS one does not;
    as a member, no Family tab; DM rows never show `uid:xxxx`.
20. **Group-handling fixes** (2.5): a group message while on another page notifies with the group
    name and opens the group; a group with more than one page of messages offers Load older; a
    family admin creates a group from family members.
21. **Location page scoping** (2.6): family admin sees all family devices on the map and can
    Locate now; the other family's devices never appear, including for super until the switcher
    is changed; a member sees only own and granted devices.
22. **Policy and approved editors in the member drawer** (3.4): change a member's policy to Open; every family contact appears in `devices/{id}.smsContacts`; switch back
    to People and only contacts picked in Approved remain.
23. **New chat dialog** (3.5): member on `people` types an unrelated alias, sends, sees "Your
    family admin has limited who you can message."; typing a phone number shows 'Numbers are texted
    from the pager…' and does not navigate.
24. **Family Contacts page** (3.6): an external created through the member drawer appears;
    renaming it changes the name shown on `/chat`; adding a second contact with the same name (any case) shows the 409 message; Delete removes it from
    every member's pager list.
25. **Alerts page and badge** (4.4): a device `sms_log` `in` from an unlisted number (Python test pager
    `sms in <phone> <text>`) raises one `sms_unknown` card; Approve with a name creates the contact and puts it on that
    member's pager; Block adds the number to the family's blocked list; a `contact_req` card approves into an SMS
    contact or a link. On the device page, the SMS log card (Refresh button) lists every text newest first; a
    malformed audit upload (bad `sms_log` payload from the pager) shows a red "malformed" chip, its reason in italics,
    "—" for unknown fields, and a "raw" toggle revealing the hex. A chat with an SMS contact interleaves the pager's modem texts for that number as bubbles (out = right, in = left,
    "modem" chip, status, malformed rows with reason/raw); Refresh or window focus reloads them.
26. **Relay SMS** (docs/RELAY_SMS_DESIGN.md, 8 Oct 2026). The Twilio transport was removed 9 Oct 2026 (last present at
    commit 05ec3ed; review with `git show 05ec3ed:<path>`); SMS numbers now come from bridge phones and are read-only:
    People -> member drawer and Admin -> Users (column "SMS number") show the number or "none". A held `sms_unknown`
    card reads "to @kid · +1 ...", the newest text, "N messages waiting" (expander lists each text; status chip only
    when not `held`); Approve (name) shows "Delivered N" and a 409 keeps the dialog open. Delivery chips: "waiting to
    send SMS" / "sent by SMS" / "SMS failed". New chat lists sendable SMS contacts only for a user with an SMS number;
    the address book shows "needs an SMS number" for unsendable ones.

27. **Bridge phone** (docs/BRIDGE_PHONE_DESIGN.md, docs/BRIDGE_PHONE_TASKS.md W2-W5; needs the relay's bridge
    routes, `tools/bridge_sim.py` can stand in for the phone):
    - Family -> Devices -> "Bridge phones": Add bridge phone (member + label) opens the pairing panel with an 8-digit
      code, a 10-minute countdown and the phone checklist; once the phone pairs the panel flips to "paired". The
      table polls every 10 s: Google account, SIM and Voice numbers, the member's SMS number, "Voice only" chip when
      there is no SIM, last seen in red past 15 min, battery, listener / SMS app / accessibility chips, tier-2 count.
      Numbers edits both numbers (empty clears), Reassign and Unpair (confirm) work, New code appears only for an
      unpaired phone, Accept SIM only when the phone reports a different SIM.
    - People -> member drawer -> "Google Chat..." (and Family -> Google Chat) opens `/family/chat?uid=`. An unknown
      Chat group message shows under "Seen, not subscribed" and as a "Google Chat" alert card (title, Group (N people
      seen) or DM, people chips, newest text, "N messages waiting"; Subscribe, Ignore, Dismiss, no Block).
    - Subscribe: name defaults to the first 16 characters of the title (409 text shows inline), roster nicks default
      to lowercase slugs and are checked inline (shape, unique); with the member's outbound policy "No people" the
      "Kid can reply" switch is off and disabled ("@kid's policy does not allow outbound messages"). Success toasts
      "<name> is on @kid's pager; N waiting messages delivered". Rename, Edit roster, Pause / Resume, Unsubscribe
      work from Manage; entries past the pager's 32 show "Not on pager". Add by link: paste a chat.google.com or
      voice.google.com link, Inspect; the dialog opens within ~30 s when the phone reports it.
    - WhatsApp is a third bridge source (9 Oct 2026): "WhatsApp" caps chip on the phone row, "WhatsApp" labels on chats/alerts/book;
      Add by link refuses wa.me / whatsapp.com links (wait for a message from the chat).
    - Address book shows a "Google Chat" / "Google Voice" chip on bridged contacts; New chat lists sendable ones
      without needing an SMS number. Delivery chips: "waiting for the phone" / "sent on Google Chat".

`npm run build && npx tsc --noEmit && npm run lint` should all be clean.
