# Chat UI and address-book design (24 Sep 2026)

Owner request, 24 Sep evening: the pager must show all chats, start a new chat, and start a new
group by adding people from the address book; the book must hold every user authorised to talk to
the device; plus three Home fixes (error text position, separator, preview format). This document
records the decisions and the split into tasks. Where it differs from `docs/DEVICE_PLAN.md` §5.5 or
`docs/GROUP_CHAT_DESIGN.md`, this document wins.

## 0. Decisions

1. **Group creation from the device is allowed** (reopens `GROUP_CHAT_DESIGN.md` "admin-only, do
   not re-open", by owner request). Members must already be in the device's book; the device owner
   becomes the group's creator; the relay keeps the alias-generation and fan-out it has today.
2. **The book is the owner's outgoing allow edges plus the owner's groups**, as today
   (`relay/app/devcfg.py:_approved_contacts`). Not changed: a user who may message the device but
   whom the device may not message is not listed, because a reply to them would be refused anyway.
   Flagged for the owner: the admin allow-list grid can be asymmetric.
3. **The book is pushed on every change that affects it**, and once at first contact for a fresh
   device. Today it is pushed only on contact approval and group edits.
4. **The book is pulled, not pushed** (owner, 24 Sep evening). The `/down` `book` message is
   replaced by a nudge: the relay bumps `bookVersion` and publishes a tiny `/down` `book` carrying
   only `bv` (no `c`/`p`). The device compares `bv` with its stored version (it already does this
   through `/status`) and, when behind, fetches `GET /api/device/book?bv=<n>` over the modem's TLS
   socket, reusing `cafetch.c`'s request/response path, authenticated with the device key that signs
   `/up` messages (`n` counter + `sig` over the query, verified like an up message). The response is
   the same CBOR book object as today, with no size cap; the device stores up to
   `BOOK_MAX_CONTACTS` = 32. This removes the 640-byte envelope from the book entirely and closes
   `ROADMAP.md:83-86`. A missed nudge is harmless: every `/status` still carries `bv`, and the relay
   re-nudges when it is behind. Cost: one TLS connection per book change.
   **Refined by server-architect, 24 Sep (PROTOCOL.md §3.7, §14.7):** the nudge also carries the
   relay `url` (key 57) and is sent only to devices whose `/status` advertises `bpull:1` (key 58),
   because today's firmware would store a contact-less book as empty; the HTTPS fetch does not
   validate the server certificate, so the response is a signed CBOR map echoing the request
   counter; the fetch consumes the device's `/up` counter `n`; the device buffer caps the book at
   32 contacts (relay sets `more:true` when truncating).
5. **Home is a single scrollable list**: one row per peer (newest activity first), then the menu
   rows. Chat is per peer. This is `DEVICE_PLAN.md` §5.5 as written; the current single merged row
   was the F7 stopgap.
6. **No pager-side member list for groups** (unchanged, `GROUP_CHAT_DESIGN.md` §4). The pager knows
   a group as a book entry with `t:"grp"` and the author of each message via `sndr`.

## 1. Relay: book population

- `bump_book_version` + `push_book` are added to: `PUT /admin/allowlist` (for every owner whose
  outgoing edges changed), device creation and any change of `defaultToUid`, and user
  `displayName` changes (for every owner who lists that user). Existing triggers stay.
- Bootstrap: in `ingest.py`'s status handler, if the device's `bookVersion` is 0, bump to 1 and push.
  This is what makes a fresh device get a book at all (today `bv 0 < bookVersion 0` never fires).
- `push_book` publishes the nudge only (`{kind:"book", bv}`); `build_book` keeps producing the
  full object, now served by `GET /api/device/book?bv=<n>` (decision 4): the device authenticates
  with `X-Device-Id`, `X-N` (its up-message counter, must be greater than the last one seen for
  that device) and `X-Sig` (the same signature scheme as `/up`, over `device_id|n|bv`). The relay
  answers the CBOR book with `bv` = current `bookVersion`, and no contact cap
  (`MAX_APPROVED_CONTACTS` removed; `p[]` keeps the newest 4). Order: `d` first, then groups,
  then people, both alphabetical by display name.

## 2. Wire: group creation request

New `/up` kind `grp_req` (add to `PROTOCOL.md` §3.1/§3.2; `server-architect` reviews the edit):

```
{"v":1,"id":"u_…","ts":…,"kind":"grp_req","name":"Cousins","m":["mom","ben"],"n":…,"sig":"…"}
```

- `name`: 1..16 code points, the group's display name. `m`: 1..8 aliases, each an entry of the
  device's current book with `t` in `web|chat` (not `grp`, not `sms`).
- Relay: verify like any up message; the device owner is the creator; members = owner + `m`
  resolved through the owner's outgoing allow edges (an alias that is not allowed fails the whole
  request); alias generated as today (`routers/conversations.py` creation path, factored so the
  admin route and this path share it); allow edges added both ways as today; `bump_book_version` +
  `push_book` for every member's devices. Reply: no dedicated ack; the resulting `book` push is the
  confirmation. Failure: a `/down` `cfg`-style message is not needed; the relay logs and the device
  shows nothing new (owner-visible failure mode accepted for now, flagged below).
- Dedup on `id`, rate limit 1 per minute per device (same mechanism as `contact_req`).

## 3. Firmware: screens

**Home (`scr_home.c`).** Rows, 12 px pitch from y=17, scroll window of 7 rows, cursor `>` at x=0:
- one row per peer alias with any message in the thread, newest first; text
  `[alias] last body` with the body clipped (ellipsis) so it never reaches the time column; time
  `HH:MM` right-aligned, `*` after it when unread; alias shown as the local nickname when set;
- a double underline (hlines at y and y+2) between the last peer row and the menu; 4 px padding
  after it;
- menu rows: `New message`, `New group`, `Address book`, `Device`, `Lock now`;
- no peers: one row `(no chats yet)`;
- footer hint unchanged at y=112; toast moved to y=98 (clears rows 96..110) so it never overlaps
  the footer;
- typing a letter on Home still jumps into the newest chat's composer (today's behaviour).

**Chat (`scr_chat.c`).** Takes a peer alias. Header: `[alias]` (nickname if set), or `[alias] ·
group` for `t:"grp"`. Rows from `msg_iter_peer(alias, …)`; the author column uses `sndr` for group
messages as today. Enter sends with `to = alias` (empty when alias is the book's default `d`).
The `@alias` prefix keeps working and re-targets the message. Opening from an incoming page opens
that page's peer.

**Pick (`scr_pick.c`)**, wired from `New message` and from `New group`:
- single-select mode (New message): Enter opens the per-peer chat with the composer focused;
- multi-select mode (New group): Enter toggles `[x]` on a `web|chat` entry (groups and SMS greyed
  out), the last row is `Create group…`; Enter there opens a one-line name entry (IME, 16 code
  points), Enter sends `grp_req`, toast `group requested`, back to Home. The group appears in Home
  when the book push arrives (decision 6; no local placeholder).

**Book (`scr_book.c`)** wired from `Address book` as is.

**Book capacity:** `BOOK_MAX_CONTACTS` 10 → 16 (`book.h`); NVS blob grows accordingly.

## 4. Home visual fixes (exact)

- Toast: y=98, height 12, clear 96..110 (`ui.c:460-477`). Footer stays at 112.
- Peer row: `[%s] %s` (was `%s %s` after a `>` cursor); body clipped to `time_x - 4`.
- Separator: `gfx_hline(0,295,y)` and `gfx_hline(0,295,y+2)`, then `y += 6` (was one line, `+3`).

## 5. Tasks

| id | owner | scope | verify |
|---|---|---|---|
| T1 | backend-dev | §1 book triggers, bootstrap, nudge + `GET /api/device/book` with device-signed auth | pytest: fresh device gets bv 1 on first status; allowlist PUT nudges; the endpoint serves 20 contacts; a bad signature or replayed `n` is 401 |
| T1f | firmware-dev | device side of decision 4: on a `book` nudge (or any `bv` ahead of the stored one) fetch the book through `cafetch.c`'s socket path, apply, ack `shown`; `BOOK_MAX_CONTACTS` 32 | bench: a nudge produces one TLS fetch and the book on the Book screen |
| T2 | server-architect then backend-dev | §2 PROTOCOL.md text; relay `grp_req` handler sharing the admin creation path | pytest: grp_req creates the conversation, edges, and pushes books to every member's device; non-allowed alias rejected |
| T3 | firmware-dev | §3 Home list + §4 fixes; wire Book and Pick from Home (done 24 Sep, host-tested; `BOOK_MAX_CONTACTS` moves to 32 in T1f) | host tests + on-glass check |
| T4 | firmware-dev | §3 per-peer Chat, picker single-select, `to` set from the peer | bench: page from two aliases, two rows on Home, reply lands on the right peer (relay log) |
| T5 | firmware-dev | §3 multi-select picker, name entry, `grp_req` publish | bench: create a group from the pager, book arrives, group row appears, message to it fans out |
| T6 | bench-tester | end-to-end on the release build; the `sleeptest 6` window must still pass 3/3 | logs under `build/bench-logs/phaseC*` |

Dependencies: T3 and T4 can start now; T5 needs T2's wire shape (fixed above, so it can start in
parallel and be tested once T2 lands); T1 is independent.

## 6. Flagged for the owner

- Decision 2's edge direction; decision 1 reverses a "do not re-open".
- `grp_req` failures are silent on the pager (no error path on the wire yet).
- The pending-ack queue holds 8 (`msg.h:77`); more than 8 unshown messages are acked only when the
  next page arrives. Independent of this design; noted during the 24 Sep hang review.
