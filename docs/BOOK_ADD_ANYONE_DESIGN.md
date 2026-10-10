# Book: add anyone, delivery is policy (10 Oct 2026)

Owner, 10 Oct 2026: "firmware has no way to text an unapproved number. It cannot add an address
book entry without approval. Please allow that. Address book can add anyone. Whether or not the
message delivers is the policy." Tasks: `docs/BOOK_ADD_ANYONE_TASKS.md`.
Supersedes CONTACT_REQ_DESIGN decisions 1 (pending rows, the pending link) and 2 (request
approval). It also amends ADDRESS_BOOK_DESIGN decisions 1-3 (the `added` marker) and resolves
FAMILIES_DESIGN deviation 2: a pager reaches a new number by adding it, not by sending to it.

## Today (verified)
`contact_req` → `ingest._classify_contact_req` → pending `contactRequests` row + `contact_request`
alert; the pager sees only `p[] {n,s}`. Approve (`family._approve_contact_request`) writes the
owner→peer edge, so **approval is membership and permission at once**. `entries_for` lists an
external only via `sms_contacts_for` (an explicit edge, or policy `external == any`), and
`devcfg._approved_contacts` drops non-`sendable` entries. A refused `/up` gets `unknown recipient`.

## Decisions
| # | decision | why |
|---|---|---|
| 1 | **Add creates the entry.** Phone → `externals_store.get_or_create(fid, e164, name)` plus a marker. If the number is already a family contact under another name, it is reused and the typed name becomes the owner's `nick`. If the *name* belongs to a different number, the contact is named `<name≤11cp> <last4>`, then `<name> <last6>`, then the E.164, and `nick` = typed name. No 409 ever reaches a pager. | Family names stay unique, because `sms_find_by_name` and `contactNames` depend on that. The kid still sees the name they typed (relay path; a modem owner's `cfg.sms` carries the family name). |
| 2 | **Alias scope does not change**: a same-family person, or a non-disabled person with a `message` edge **to** the owner, gets a marker (no alert). Any other alias (unknown, external, group, disabled, foreign without an inbound edge) gets the one shared `<name>: no contact @<alias>`. | Creating an entry on a successful add would confirm that an alias exists, and `n` would leak a stranger's name. Kids' real contacts are phone numbers, and a cross-family person opts in through their own edge. The rule that every alias failure looks the same is kept. |
| 3 | **`in_book` only means "already on this pager"**: the entry is in this owner's `c[]` (relay path) or `cfg.sms` (modem path). An entry hidden by policy gets a marker. | Today a policy-hidden sibling returns "already in your book" while the pager does not show them. |
| 4 | **The marker is `users/{owner}/book/{peer}.added = true`**, plus `addedAt` and `addedBy` (device id), next to `nick`/`familyId`. `routing`, `policy` and `sms_contacts_for` **never read it** (pinned by a test). | ADDRESS_BOOK decision 3 objects to a second *allow-list*. This is the owner's own list of entries to show. Permission stays in edges and policy, so this is not a second gate. |
| 5 | **Pager listing**: `entries_for` adds marked peers that are not already listed (persons, plus externals of the owner's family) and carries `added`. `c[]` = sendable entries ∪ `added` entries. Policy-implied entries that are not sendable stay off, as ADDRESS_BOOK decision 2 says. | The kid typed it in, so the kid should see it. An implied entry the kid never asked for and cannot message is noise. |
| 6 | **Limits**: at most 10 adds per device per hour (`rate_limits.check_and_increment("book_add:{d}")`, checked after the dedup check, reply `too many adds; try later`). At most **32** `added` markers per owner (reply `<name>: address book full`). The 5-pending cap goes away. | 32 is the pager's storage (decision 11). 100 adds produce at most 32 entries and 32 family contacts. Nothing is texted unless policy allows, and each peer raises at most one open alert. Under `open`, adding a number is a real permission to text it, which is what `open` means. |
| 7 | **Delivery is policy.** `routing.send` does not change. If the recipient carries an `added` marker for the sender and the refusal is `not_allowed`/`policy_out`/`policy_in`, ingest replaces `unknown recipient` with a named reply (§4.2 text below). Every other refusal keeps today's body. | The pager already knows the entry exists, so naming it leaks nothing, and the kid needs to know to resend. |
| 8 | **Approval alert on a refused send, only when it is fixable**: `fixable = policy.check(owner, peer, True, has_in) is None`, meaning the owner's own edge would let it through. The `contact_request` alert has deterministic id `cr_{owner}_{peer}`, written in one transaction: absent → create; `open` → no-op; decided more than 24 h ago → reopen. FCM push only on create or reopen. Unfixable (the policy refuses that kind, or the peer's side refuses) → reply only, no alert. | Dedup is one doc, so a race cannot double-alert. Asking a parent for something one click cannot grant is noise; they set the policy on purpose. |
| 9 | **The refused message is not held.** The kid resends after approval. | §4.2 drops and never stores a refused `/up`. A held outbound would need a second delivery path with its own wireId dedup, and could deliver hours late on top of a kid's resend. Inbound `heldSms` exists because the sender cannot be told; the kid can. |
| 10 | **Approve** (`contact_request` alert without `contactRequestKey`) = `set_edge(owner, peer, message=True, locate=<existing or False>)`, then `rederive_family_sms_contacts` for an external, then `book.bump_and_push({owner})`, then the alert is marked handled. Block (phone) is unchanged. A legacy alert with `contactRequestKey` → 409 `superseded; add again from the pager`. | The entry already exists, so approve only grants permission. No migration while prod is test data. |
| 11 | **`open` / `any_sms`**: the add creates the contact. `sms_contacts_for` already implies it and the send delivers. `open` fires `new_conversation` as today, `any_sms` raises no alert, as today. | No change in meaning. |
| 12 | **Modem-path owner** (no `smsNumber`, decision 1 phone): `cfg.sms` is the delivery gate and the relay never sees the send. The marker is still written, but the contact enters `cfg.sms` only through `sms_contacts_for` (an edge or `any`). If policy refuses at add time: fixable → raise the decision 8 alert **now**, reply `<name>: added; needs a parent's OK to text`. Unfixable → `<name>: added; settings don't allow texting`. Over the 8-entry cap: the web marks the entry "Not on pager". | Putting a refused number on the modem allow-list would bypass policy. Raising the alert at add time is the only point where a parent can hear about it. |
| 13 | **`p[]` retired**: the relay stops sending `p` and stops creating `pending` rows. A full book always carries `c` (possibly `[]`), so `book.c` never reads it as a nudge (`is_nudge = !have_c && !have_p`); an absent `p` already parses as 0 requests. Rejections (bad/blocked/no_contact) keep their system replies and their `rejected` rows, but no longer bump the book. | Pending no longer exists, and the reply already tells the kid. |
| 14 | **Dedup + transaction for an add**: (a) `get_by_device_and_req` → redelivery no-op. (b) rate check. (c) `get_or_create` (idempotent through `create()`). (d) **one transaction**: `create()` `contactRequests/{d}_{id}` `{status:"added", peerUid}`, count `added` markers, `set(merge)` the marker (+`nick`), and `bookVersion += 1` on each of the owner's devices. (e) push the book (outside the transaction). (f) modem path: `rederive_sms_contacts`, alert. | A lost push is repaired by the `bv` heartbeat, but a lost bump never would be (ADDRESS_BOOK decision 7). The row's `create()` is the transactional dedup on the wire id. |
| 15 | **Web** (minimal): the AlertCard `contact_request` reads "@kid wants to text Grandma (+1 206 555 0100) · added from the pager", with Approve / Block (phone) / Dismiss. Contacts page: an "added by @kid" chip from the markers. `/settings/book`: an "Added from pager" chip, the reason when not sendable, and **Remove** (`DELETE /api/book/{owner}/added/{peer}` clears `added` only, then bump+push). ApprovedEditor is unchanged. | Nothing new to learn. Remove is how a parent undoes 32 junk adds. |
| 16 | **Firmware**: the Add toast changes from "sent for approval" to "added", and the hint from "enter send for approval" to "enter add". `scr_book.c`/`scr_pick.c` request-row rendering is deleted. `BK_P` parsing stays (an older relay may still send it, and it costs nothing). The `book.h` `book_request()` comment is corrected: digits are a phone, there is no 5-pending cap, and the add creates the entry. No other behaviour changes. The entry appears after the bump → nudge → pull (seconds). | Only the copy is wrong today. `msg.h`'s `msg_queue_reply` comment stays accurate: added entries are in `c[]`. |

## Invariants
The relay is the only writer: the `book` writes are relay-only in `firestore.rules`, and the read rule is unchanged. The allow-list is still edges plus policy in `routing` and in the rules; the marker grants nothing. The wireId dedup is unchanged and a refused send stores nothing. Delivery state is untouched. No long-lived process is involved.

## Failure modes
| failure | effect | recovery |
|---|---|---|
| crash after `get_or_create`, before the transaction | orphan family contact, no marker | redelivery re-runs (row absent) and finishes; or Contacts → Delete |
| push lost after the bump | entry missing on the pager | heartbeat `bv` re-nudge (≤1 h) |
| alert transaction contention | the second sender reads `open` | none needed; one alert |
| parent dismisses, kid keeps sending | `<n>: not approved` for 24 h, then the alert reopens | approve via People → Approved |
| modem owner over 8 | entry marked, not in `cfg.sms` | web "Not on pager"; parent trims |

## Measure
`contact_req outcome=<added|added_held|in_book|bad_number|blocked|no_contact|full|rate>`; `send_refused_added reason=<..> fixable=<0|1> alert=<created|open|reopened|declined>`; alerts per family per day; markers per owner (p99 ≤ 32); count of `contactRequests` rows with `status:"pending"` created after the deploy = 0.

## Proposed PROTOCOL.md text (additive; server-architect applies after review)
§3.1 table, `p` row, append: *(10 Oct 2026: no longer sent, §3.2; an absent `p` is empty.)*

§3.2, appended to the `kind:"contact_req"` paragraph:
> *(10 Oct 2026, owner decision.)* A request is an **add**, not a request for approval: the relay
> adds the entry at once (a phone becomes the owner's family SMS contact; an alias resolves only to
> a same-family person or a person with a `message` edge to the owner, and any other alias gets
> the one `no contact` reply), and it appears in the next book's `c[]` whether or not the owner may
> message it. Whether a message to it delivers is §4.2's decision. The 5-pending limit is replaced
> by at most 10 adds per device per hour (`too many adds; try later`) and 32 added entries per
> owner (`<name>: address book full`). An owner without an SMS number whose policy does not allow
> the number yet gets `<name>: added; needs a parent's OK to text` or `<name>: added; settings
> don't allow texting`, and the number stays off `cfg.sms` until allowed. *(a pager could not add
> an entry without a parent first; membership and permission are now separate, docs/BOOK_ADD_ANYONE_DESIGN.md.)*

§3.2 `kind:"book"`, appended to the payload bullet:
> *(10 Oct 2026: the relay no longer sends `p`; nothing is pending. A full book always carries `c`,
> possibly empty, so it is never read as a §3.7 nudge; a receiver MUST treat an absent `p` as
> empty.)* *(an add creates the entry, so there is nothing to list as pending.)*

§4.2, after the 9 Oct note:
> *(10 Oct 2026: when `to` names an entry the owner added (§3.2) and the policy refuses it, the
> body names the entry instead: `<n>: needs a parent's OK; resend once approved` when the owner's
> own approval would let it through (a parent is alerted, one open alert per owner and entry),
> `<n>: not approved` when a parent declined within 24 h, else `<n>: not allowed`.
> The message is still dropped, never held; `<n>` is the entry's book name.)* *(the device already
> lists the entry, so naming it leaks nothing, and the user needs to know to resend.)*

§3.3 budget: the longest new body is a 48-byte name plus 43 bytes = 91 bytes, under the 92 bytes
§3.3 already carries for `contact_req` replies, so §3.3 does not change and the envelope stays
≈230 bytes, under 640.
