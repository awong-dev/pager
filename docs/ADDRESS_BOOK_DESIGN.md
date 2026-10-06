# Address book design (7 Oct 2026)

Owner, 7 Oct ~1:30 am PDT: the address book must be editable from the web, by a family admin for
every member; *New chat* is a search-as-you-type select over it; it lists everyone in the family by
default; the pager checks for a book sync periodically. Tasks: `build/bench-logs/TASK_book_{backend,web,firmware}.md`.

## What exists (from the code)
- The pager's book is built by `relay/app/devcfg.py` `_approved_contacts()`: the owner's outgoing
  `allow` message edges (`allow_store.allowed_recipients`) plus their groups, names cut to 16 code
  points, ordered by `_ordered_contacts()`, capped at 10 (legacy envelope) or 32 (`build_book_body`,
  §3.7 pull) with `more:true` past 32. A family member with no edge is **not** in it.
- Every book change is `contacts_store.bump_book_version(d)` (one transaction on `devices/{d}`)
  then `devcfg.push_book(d)` (nudge or full book). Callers: approve/reject contact, allow-list PUT,
  `/approved` PUT, group create/join/leave (`conversations._push_book_to_members`), device create,
  displayName change (edge holders only). **Gap:** `PATCH /api/family/contacts/{uid}` (external
  rename) bumps nothing, so the pager keeps the old name.
- The pager already sends `bv` in every `/status` (`modes.c:857`), including the hourly heartbeat
  (`maybe_publish_heartbeat`, §5.4(d)); `ingest.handle_status` re-nudges whenever the reported `bv` is
  behind (`renudge_if_behind`). The Device screen's "Re-sync address book" publishes `/status` now.
- `bookpull.c` retries a failed fetch (5xx, timeout, bad MAC) **every 60 s with no limit**
  (`finish_attempt` → `requeue_pending`); each try is a ~2.6 s TLS exchange (bench: `elapsed_ms=2615`).
- `NewChatDialog.tsx` is already an MUI `Autocomplete`, but `freeSolo` over `useDirectory()`.

## Decisions
1. **The book is derived, not stored.** Entries = same-family persons (not self, not disabled) ∪ the
   owner's outgoing message-edge peers (cross-family people, externals) ∪ the owner's groups. One
   function, `app/book.py` `entries_for(owner_uid)`, feeds the pager (`devcfg`), `GET /api/book` and
   New chat. *(two lists that must agree will drift; the pager and the web must show the same people.)*
2. **Same-family persons count as approved.** In `routing` the `approved` people rule passes on an
   edge **or** both ends being persons with the same non-null `familyId`. `none` rules (policy `sms`,
   `any_sms`) still refuse. *(an entry the owner asked to be listed by default but cannot message is
   worse than no entry; family members are the trusted set every policy preset was written to
   restrict outsiders, not siblings.)* Supersedes the "people" column reading in FAMILIES_DESIGN §2;
   flag to the owner. Each entry carries `sendable` = `policy.check(...) is None`; the pager and
   New chat list only sendable entries, the Address book page lists all and says why.
3. **Only the nickname is stored.** `users/{ownerUid}/book/{peerUid}` = `{nick, familyId (owner's,
   copied for the rules), updatedAt, updatedBy}`; a missing doc means "no nickname". People and
   externals only; a group is named by its group name. The pager's `n` = `nick or displayName`.
   *(membership is policy and edges, already edited in People → Approved; a second list of who is in
   the book would be a second allow-list.)* Adding a number stays in People → Approved (admin).
4. **Nickname bounds = the wire's `n`:** trimmed, 1–16 code points, ≤ 48 UTF-8 bytes, no control
   characters; violations are 422 (rejected, not truncated, because a person is typing it).
   A device-local nickname (§5.5, ≤12 cp) still wins on that pager.
5. **API** (`app/routers/book.py`, `require_user`; the owner, a family admin of the owner's family,
   or super; anyone else 404): `GET /api/book?uid=` (default self) → `{ownerUid, bv, pagerCap: 32,
   truncated, entries:[{uid, alias, kind, displayName, nick, label, phone?, inFamily, sendable,
   reason?, onPager}]}`; `PUT /api/book/{ownerUid}/entries/{peerUid}` `{nick}`; `DELETE` same path
   clears it. PUT/DELETE on a peer not in `entries_for(owner)` → 404. Rate-limited with the existing
   `"admin:{uid}"` window. `onPager` = the entry's index in `_ordered_contacts` < 32.
6. **What bumps `bv`** (helper `book.bump_and_push(owner_uids, broker)`, the old
   `_push_book_to_members` moved and de-duplicated by device): nickname PUT/DELETE (owner);
   member create, `familyId` move (old and new family), `displayName`, `disabled` or `policy`
   change (every member of that family plus existing edge holders); external rename (edge holders).
7. **Transactions.** Nickname PUT/DELETE is **one** Firestore transaction: write/delete the
   `book` doc and `bookVersion += 1` on each of the owner's devices; the push follows outside it.
   *(a lost push is repaired by the next `/status`, which compares `bv`; a lost bump would never be
   repaired.)* Family-wide bumps run after the primary write and before the 200, each device its own
   existing transaction; a failure returns 5xx and the client's retry is idempotent (extra bump).
8. **Rules.** Add under `match /users/{uid}`:
   ```
   match /book/{peer} {
     allow read: if request.auth != null && (isSuper() || request.auth.uid == uid ||
       isFamAdmin(resource.data.get('familyId', null)));
   }
   ```
   Writes stay denied by the catch-all; the relay is the only writer. The web reads through the API
   (the merged view needs users a member cannot read); the rule bounds any direct read.
9. **Periodic sync = the hourly heartbeat `/status`,** which already carries `bv` and already makes
   the relay re-nudge when behind. No new HTTPS poll. *(in sync it costs 0 extra radio sessions; an
   hourly signed GET would be 24 × ~0.1 mAh ≈ 2.4 mAh/day (estimate: 2.6 s measured at ~120 mA plus
   RRC tail), ~5 % of the 38–50 mAh/day budget, and burns one counter value and one Firestore
   transaction per poll to learn nothing.)* Session-up and airplane gating are inherited.
10. **Firmware fix: cap self-retries.** After 3 failed attempts (≥60 s apart) on one nudge the
    device drops it; the next nudge, online edge or heartbeat re-nudge (≤1 h) brings it back.
    *(today a relay outage costs ~60 TLS attempts/h ≈ 6 mAh/h; capped it is ≤3/h.)* Push+nudge path
    unchanged.
11. **Past 32 entries:** the pager keeps the first 32 in §3.7 order (default, groups, people by
    name); `more:true` is logged; no paging (device storage is 32, `more` stays reserved). The web
    marks each entry beyond 32 "Not on pager" and shows a banner "Your pager shows 32 of N". A
    legacy (non-`bpull`) device keeps its 10.
12. **Web:** `/settings/book` "Address book" for every role (Settings menu); admins/super get a
    member picker (family persons) above the list and a link to People for approvals. New chat
    becomes a select over `GET /api/book` sendable entries: `freeSolo={isFamilyAdmin}`, `groupBy`
    Family / People / Numbers / Groups, `getOptionLabel={o => typeof o === 'string' ? o : o.label}`,
    `filterOptions={createFilterOptions({stringify: o => `${o.label} ${o.displayName} ${o.alias}
    ${o.phone ?? ''}`})}` plus the synthetic "Text +1 …" option only when the user may text any
    number (`policy.out` ∈ {open, any_sms}) or is an admin; `isOptionEqualToValue` by alias.

## Failure modes and recovery
| failure | effect | recovery |
|---|---|---|
| push lost after bump | pager shows old book | next `/status` (≤1 h heartbeat, or Re-sync) re-nudges |
| fetch fails 3× | nudge dropped on device | relay re-publishes on heartbeat/online edge |
| relay crash between primary write and family bump | 5xx to the web | client retry; idempotent |
| >32 entries | tail missing on pager | web marks "Not on pager"; owner trims groups/edges |
| policy blocks a family member | not on pager, greyed on web with reason | admin changes policy |

## What to measure
Relay: `book bump reason=<trigger> owner=<uid> devices=<n>` per bump; `device book` lines per device
per day (expect ≈ bumps, not 24); `status bv behind` re-nudges per day; count of books served with
`more`. Device: `book fetch done` `elapsed_ms` and `applied`; new `book fetch: giving up` count.

## PROTOCOL.md edits (for a later pass; additive, no envelope change, §3.3 budget untouched)

**Applied to PROTOCOL.md §3.7 and §14.7 on 6 Oct 2026.** The §14.7 line follows decision 10 and `bookpull.c` (3 failed attempts per nudge, the 409 retry not counted) rather than the "retries … at most 3 times" wording below.

- §3.7 "Relay-side triggers", append: "…, a change to the owner's nickname for a listed entry, a
  member joining or leaving the owner's family, and a `disabled` or `policy` change of a family
  member. *(the book lists the whole family and per-owner nicknames, `docs/ADDRESS_BOOK_DESIGN.md`.)*"
- §3.7, new last paragraph: "**Periodic check.** The §5.4(d) heartbeat carries `bv`, so a device
  that missed a nudge is re-nudged within an hour; there is no periodic fetch. *(an hourly signed GET
  would cost ~2.4 mAh/day to learn what the heartbeat already reports.)*"
- §14.7 "Device on failure", append: "A device retries one nudge on its own at most
  3 times, ≥60 s apart, then waits for the next nudge or online edge. *(an outage otherwise costs a
  TLS attempt a minute.)*"
