# Relay SMS: one Twilio number per user (8 Oct 2026)

*(9 Oct 2026: a member whose `smsNumber` is a bridge phone's SIM or Google Voice number sends and receives through that phone instead of Twilio — docs/BRIDGE_PHONE_DESIGN.md decisions 3–4; the number-belongs-to-a-person model, the hold-until-approved inbound flow and the pager's `t:"sms"` book entries below are unchanged. Twilio remains the transport for every other number.)*

*(owner, 8 Oct 2026: "Examine the entire flow to ensure we can associate a twilio number with a
*user*. Then the pager sending/receiving to a SMS will go through the relay. Outbound policy should
follow what we've implemented as rules. Inbound policy should store any unknown SMS but it should
NOT deliver to the pager. Instead, it should notify the parents in the family with the message
contents forwarded. Admins can then authorize. Prior messages can be delivered.")*

This reverses the 7 Oct 2026 "no relay SMS" decision (`build/bench-logs/DESIGN_no_relay_sms.md`,
commit 123efa4) for members who hold a relay SMS number. The modem path (`cfg.sms`,
`firmware/main/sms.c`) is unchanged and still serves members without one. Twilio account: the
owner's, low-volume standard 10DLC brand with an EIN; numbers are bought and attached to the
campaign by hand in the Twilio console (see relay/README.md "Twilio"). Tasks:
`build/bench-logs/TASK_relaysms_{backend,web,infra,docs}.md`.

**Firmware does not change.** `scr_pick.c` lists the book's `c[]` (any `t`, including `"sms"`,
sent as a normal `/up to:<alias>`) and then `sms.c`'s own `cfg.sms` list (modem). A member with a
relay number gets their SMS contacts in `c[]` tagged `t:"sms"` and an **empty** `cfg.sms`, so
each contact appears once and every text goes through the relay.

## What exists (from the code and git)
- Commit 123efa4^ had a Twilio adapter (`app/backends/sms_twilio.py`: `deliver`, HMAC-SHA1
  `verify_twilio_signature`, `twilio_webhook_url` from `PUBLIC_BASE_URL`), a client
  (`app/notify/sms.py`: `send_sms(to, body, from_number)` against `TWILIO_BASE_URL`), the webhook
  (`routers/webhooks.py` `POST /webhooks/twilio/sms`: per-IP limiter, signature, hold-or-deliver),
  a mock (`tools/mocks/twilio_mock.py`, compose `twilio-mock`), 583 lines of tests
  (`tests/test_sms_twilio.py`), Terraform secrets, and e2e scenarios. Its model was a **family**
  number (`families.smsNumber`) plus a person's own phone as an `sms` backend. Both go.
- An SMS contact is `users/{x_…}` `kind:"external"` with `ownerFamilyId`, `phone`, unique
  `displayName` per family (`app/store/externals.py`); it has no backend row. Routing refuses any
  DM to or from one (`routing._sms_contact_reject`, reason `sms_contact`).
- `app/policy.py` `check()` already evaluates the numbers column of both pickers against an
  external peer (`any` / `approved` + edge / `none`). Member default is `people`/`people`
  (numbers: none); admin default `open`/`any`.
- Alerts (`app/alerts.py`, `families/{fid}/alerts`) push FCM to every family admin with
  `notify.alerts != false` (`backends/webapp.py` `push_alert`). `sms_unknown` exists for the
  modem path (open, nothing held); approve creates the contact + edge + `rederive_family_sms_contacts`.
- The book (`app/book.py` `entries_for`, `devcfg._approved_contacts`) lists externals from
  `sms_contacts_for(owner)` (explicit edge, or implied when `rule(out,"external")=="any"`, minus
  explicit denies) but never puts them in the pager's `c[]`.

## Decisions
1. **The number belongs to a person.** `users/{uid}.smsNumber: E.164 | null`, persons only.
   Reverse index `smsNumbers/{e164}` = `{uid}` written with `create()` (server-only: no rules
   block, pinned in `test_rules.py`), freed on clear. Set or cleared by a family admin
   (`PATCH /api/family/members/{uid} {smsNumber}`) or super (`PATCH /api/admin/users/{uid}`):
   `normalize_phone`, 400 on garbage, 409 `number already assigned to @alias`. Setting or clearing
   it bumps and pushes the owner's book and re-derives their `cfg.sms` (decision 5). The relay
   never buys numbers or edits Twilio configuration; the console steps are documented.
   *(a user may own several pagers; the number is the user's, and a text fans out to all of their
   enabled backends exactly like a web page does today.)*
2. **An external is a relay peer only through a numbered person.** `_sms_contact_reject` is
   replaced by `_sms_route_reject(sender, recipient)`: for a DM where exactly one side is an
   external, the person side must have `smsNumber`, else `RejectedRecipient(reason=
   "no_sms_number")`; external↔external is refused as before (`sms_contact`). Then `policy.check`
   runs unchanged (numbers column + edges). Groups, broadcast (no `to`) and a device default
   recipient still exclude externals (unchanged, out of scope). The pager's §4.2 case-3 reply for
   `no_sms_number` is `sms not set up; ask your admin`; the web 403 message is "You have no SMS
   number; ask your family admin."
3. **Delivery to an external is an `sms` backend row on the external.** `externals.get_or_create`
   creates `users/{x}/backends/{bid}` `{kind:"sms", enabled:true, verifiedAt:now, config:{phone}}`;
   `externals.ensure_sms_backend(x)` backfills one for a contact that predates this. `"sms"`
   returns to `BackendKind` and the registry; `LINK_FLOW_KINDS` stays `{"gchat"}` and
   `POST /api/me/backends {kind:"sms"}` stays 422 (no link flow: the admin asserts the number).
   **An `sms` row is valid only on an external:** `backends_store.list_backends`/`get_backend`
   skip `kind:"sms"` rows whose owner is a person *(123efa4 left stale person-phone `sms` rows
   in prod that `_live_kind` hides today; re-adding the kind must not revive them)*. `SmsTwilioBackend.deliver`: `From` = the **sender's**
   `users.smsNumber`, `To` = `config.phone`. No sender number → `failed` with error
   `no_sms_number` (never retried). Twilio 4xx (21211 invalid, 21610 opted out, 21614 not
   mobile, 30xxx filtered) → `failed`; 5xx or transport error → `queued`. The tick regains a
   bounded retry for `sms` deliveries left `queued` (restore `Routing.redeliver` and
   `messages_store.list_recent_queued_by_kind` from 123efa4^; `record_delivery_attempt`'s
   MAX_DELIVERY_ATTEMPTS still ends it). `render_state`: sent → "sent by SMS".
4. **Inbound: known and approved delivers, everything else is held.** `POST /webhooks/twilio/sms`
   (restored shape: per-IP limiter, `X-Twilio-Signature` over `PUBLIC_BASE_URL + path`, fail
   closed without `TWILIO_AUTH_TOKEN`). Steps, first match wins; every outcome is one INFO line
   `sms in to=@alias from=...1234 sid=SM… outcome=<…>` with the number redacted:
   | step | outcome |
   |---|---|
   | `To` not in `smsNumbers`, or its user is not a non-disabled person with a family | `dropped_unknown_to`, 200 |
   | `From` not a plausible number | `dropped_bad_from` |
   | `From` in `families/{fid}.blockedNumbers` | `blocked` (nothing stored) |
   | `MessageSid` already in `heldSms` or `wireIds` | `duplicate` (Twilio retries on non-2xx) |
   | family contact X for `From` exists **and** `policy.check(X→U, has_edge_in=allow U→X.message)` is `None` | `delivered`: body > 160 cp → `too_long` (reply hint to the sender, nothing stored); else `routing.send(sender_uid=X, recipient_alias=U.alias, kind="text", body, origin_backend_kind="sms", origin_backend_id=<X's sms bid>, wire_id=MessageSid)` |
   | otherwise (no contact, no edge, or U's numbers rule is `none`) | `held` (below) |
   An empty `Body` with `NumMedia > 0` becomes `[photo]`; media is never fetched. **`any` on the
   inbound picker no longer delivers an unknown number** (FAMILIES_DESIGN §2's "delivered and
   alerted" is superseded): it means the family's *contacts* reach this member without an edge.
   No auto-reply is sent to an unknown number *(a reply confirms a live target to a spammer and
   costs a segment; flag: owner may want "your message is waiting for approval")*.
5. **Held texts.** `heldSms/{MessageSid}` = `{familyId, toUid, fromPhone, body (≤1600 cp),
   receivedAt, status: "held"|"delivered"|"too_long"|"blocked"|"dismissed", decidedAt, alertId}`,
   server-only. One **open** `sms_unknown` alert per `(familyId, subjectUid=U, peerPhone)`: the
   first held text creates it (`preview`=body[:120], `heldCount:1`, `heldBody:null`); each later
   one updates `preview` to the newest body, `heldCount += 1`, `updatedAt`, and **pushes FCM
   again** (every text reaches the parents' phones with its contents: the push `body` is
   `"+1 206 555 0100 → @kid: <text>"`, title from `_ALERT_TITLES`). Cap 25 held per
   `(familyId, fromPhone, toUid)`: beyond it, `held_cap` (dropped, logged, no push).
   `GET /api/family/alerts/{id}/held` → `{held:[{id, body, receivedAt, status}]}` oldest first.
6. **Approve delivers the backlog.** `POST /api/family/alerts/{id}/approve {name}` for a held
   `sms_unknown` (subject U, phone P), in this order, each step idempotent so a retry after a
   crash finishes the job: (a) `rule(U.policy.in_, "external") == "none"` → 409 `@kid's inbound
   policy does not allow numbers; change it under People` and nothing is written; (b)
   `externals.get_or_create(fid, P, name)` (409 `ContactNameTaken` keeps the alert open, as
   today); (c) `allow U→X message:true`; (d) `rederive_family_sms_contacts` and
   `book.bump_and_push({U})`; (e) every `heldSms` row `held` for `(fid, P, U)` in `receivedAt`
   order: > 160 cp → `too_long`; else `routing.send(... wire_id=row.id, ts=row.receivedAt)` →
   `delivered` (a `rejected` result → the row stays `held` and the response carries
   `undelivered: n`); (f) `alerts_store.decide(handled)`. **Block** adds P to `blockedNumbers`,
   marks the rows `blocked`, decides the alert. **Dismiss** marks them `dismissed`. Rows are kept
   for the message retention window (`jobs.sweep` deletes `heldSms` by `receivedAt` with the same
   TTL as messages).
7. **The modem list and the book.** `book.sms_contacts_for(owner)` is unchanged as the *set* of
   the owner's SMS peers. `book.entries_for`: an external entry's `sendable` is now `owner.smsNumber
   is not None and policy.check(...) is None`, `reason` `no_sms_number` otherwise; `onPager`
   follows `c[]`. `devcfg._approved_contacts`: when the owner has `smsNumber`, externals are
   listed in `c[]` with `t:"sms"` (name = nick or displayName, 16 cp), counted toward the 32;
   otherwise excluded, as today. `rederive_sms_contacts(owner)`: when the owner has `smsNumber`,
   `devices.smsContacts = []` (pushed as an empty `cfg.sms`); otherwise unchanged. Triggers: all
   of today's plus `smsNumber` set/cleared.
8. **Web.** People → member drawer gains "SMS number" (admin; helper: "A Twilio number from the
   family's account. Texts to it reach @kid's pagers; @kid's pager texts contacts from it.") with
   409 shown inline; Admin → Users gets the same field for super. The alert card for a held
   `sms_unknown` shows "to @kid · +1 206 555 0100", the newest text, "N messages waiting", an
   expander listing them (`GET …/held`), and Approve (name) / Block / Dismiss with the approve
   text "@kid can text this number from their pager, and the N waiting messages are delivered
   now." `DeliveryChips`: `sms` → waiting to send SMS / sent by SMS / SMS failed. New chat lists
   sendable externals (phone shown) again, only when the signed-in user has an `smsNumber`.
   Contacts page: a "via relay for @a, @b" hint is optional (skip if time is short).
9. **Infra and config.** Secrets `TWILIO_ACCOUNT_SID`, `TWILIO_AUTH_TOKEN`; env `TWILIO_BASE_URL`
   (`https://api.twilio.com`) and `PUBLIC_BASE_URL` on the Cloud Run service; **no**
   `TWILIO_FROM_NUMBER`. `PUBLIC_BASE_URL` stays the Cloud Run `run.app` origin it already is (it
   is also the base of the device CA pointer, which Firebase Hosting does not rewrite): Twilio is
   pointed **directly at Cloud Run**, `https://<service>.run.app/webhooks/twilio/sms`, not at the
   Hosting origin, so the signed URL and `twilio_webhook_url()` agree without a second setting. Validate only; the owner applies
   after the relay deploy (secret versions are added by hand). Dev: compose `twilio-mock` and
   `TWILIO_BASE_URL=http://twilio-mock:8010`; CI unit tests need no mock (httpx is stubbed); the
   e2e suite gets one `relay_sms` scenario against the mock (inbound known → pager; inbound
   unknown → held → approve → delivered; pager → contact → mock `/_sent`).
10. **Twilio console (owner, documented in relay/README.md).** One Messaging Service "pager" with
    the 10DLC campaign; every member number is added to its sender pool; the service's inbound
    request URL is `PUBLIC_BASE_URL/webhooks/twilio/sms`, i.e. the Cloud Run origin (one place, not
    per number). Outbound
    passes `From=<member number>` explicitly, so no `MessagingServiceSid` is needed. Advanced
    Opt-Out is **off** (decision 11: the relay answers the keywords itself). The relay stores bodies in Firestore; Twilio keeps its
    own copy in its logs — note for the owner.
11. **Consent is owned by the relay (owner, 8 Oct 2026; tasks in docs/RELAY_SMS_CONSENT_TASKS.md).**
    `smsConsent/{e164}` = `{status: opted_in|opted_out, optedInAt, optedOutAt, source:
    keyword|admin, lastDisclosureDate, updatedAt}`, server-only. Inbound keywords (trimmed,
    case-insensitive, whole body, checked before the blocked-number test, never stored or routed):
    START/OPTIN/IN -> opt in + welcome (`outcome=keyword_start`); STOP/UNSUBSCRIBE/END/QUIT -> opt
    out + confirmation (`keyword_stop`); HELP/INFO/SUPPORT -> help text (`keyword_help`). Replies
    are sent from the member's number straight through `send_sms`, outside the opt-in gate.
    Outbound `deliver()` only goes to an `opted_in` number (else `failed`, `not_opted_in` /
    `opted_out`, never retried). Body = `<Name> says: "<defanged text>" - Pager (<operator>)`, plus
    `. Reply STOP to opt out, HELP for help.` on the first relayed message to a number per UTC day
    (claimed in a transaction before the send; a failed send keeps the claim). URLs and phone numbers
    in the text get human-undoable spaces (`app/sms_compliance.defang`). An admin adding a contact
    or approving a held alert / contact request is the opt-in: first time only, the relay sends
    the welcome from the member's number. Operator name and support email come from
    `SMS_OPERATOR_NAME` / `SMS_SUPPORT_EMAIL`.

## Rejected
- Twilio Advanced Opt-Out: it intercepts STOP/START/HELP before the webhook (the relay never sees
  the keywords) and its carrier reply text cannot name the operator, which the program requires.
- A per-family number with `@alias` routing (the 7 Oct model): the owner asked for a number per
  user; aliases typed by grandparents were the failure mode CONTACT_REQ fixed.
- A person's sign-in `users.phone` as the SMS route: CONTACT_REQ decision 5 stands.
- Delivering unknown numbers under `any_sms`: owner, 8 Oct 2026: unknown is never delivered.
- Buying numbers or setting webhooks through Twilio's API: the 10DLC campaign attachment is a
  console step anyway, and a wrong automated purchase costs money.
- Twilio status callbacks (delivery receipts), MMS media download, externals in groups, a web
  auto-reply to held senders: later.

## Transaction boundaries
- `heldSms/{sid}` is written with `create()`; `AlreadyExists` → `duplicate`, no second alert push.
- Alert upsert after the held row is non-transactional; a crash between them leaves a held row
  without an alert, repaired by the next text from that number (the upsert finds no open alert
  and creates one counting all `held` rows) and by approve, which lists rows by `(fid, P, U)`,
  not by `alertId`.
- Approve: contact (name reservation `create()` + `create_user`), edge, rederive/book, deliveries
  (each `routing.send` is dedup'd by `wire_id` = sid inside `create_message`'s transaction), row
  status, then `decide`. A crash anywhere leaves the alert open; the retry repeats idempotent
  steps and finishes.
- `smsNumbers/{e164}` reservation `create()` before the `users` write; clear deletes the index
  only if it names this uid.

## Failure modes
| failure | effect | recovery |
|---|---|---|
| Twilio 5xx / timeout on send | delivery `queued`, chip "waiting to send SMS" | tick retry, ≤5 attempts, then `failed` |
| sender has no number | `failed` at once; pager gets `sms not set up; ask your admin` | admin assigns a number |
| recipient opted out (21610) | `failed`, chip "SMS failed" | the recipient texts START |
| number never opted in / opted out in `smsConsent` | `failed` at once, no Twilio call, log `code=not_opted_in` / `opted_out`, never retried | admin adds/approves the contact, or the recipient texts START |
| webhook signature fails | 401, Twilio retries, then gives up | fix `PUBLIC_BASE_URL`/token; Twilio console shows the error |
| flood from one unknown number | 25 held, rest dropped and logged | Block |
| held text > 160 cp approved | row `too_long`, not on the pager | parents read it in the alert |
| number reassigned to another member | 409 until cleared on the first | clear, then set |

## Measure
- `sms in … outcome=` counts per outcome per day; `held` versus `delivered` ratio.
- `sms out to=...1234 from=...5678 sid= status=sent|failed code=` per send.
- Open `sms_unknown` alerts older than 7 days (parents not responding).
- Twilio console: segments per month against the §cost estimate (~1.2 ¢/segment all-in).

## PROTOCOL.md edits (additive, no envelope change)
- §3.2 `c[]` row: replace the 7 Oct parenthetical with: *(8 Oct 2026: a member with a relay SMS
  number (`users.smsNumber`, docs/RELAY_SMS_DESIGN.md) gets their SMS contacts in `c[]` with
  `t:"sms"` and an empty `cfg.sms`; a text to such an entry is an ordinary `/up to:<alias>` the
  relay forwards by SMS. Members without a number keep the 7 Oct behaviour.)*
- §3.6 `cfg.sms`: append *(8 Oct 2026: pushed empty for a member with a relay SMS number.)*
- §4.2 case 3: add the `sms not set up; ask your admin` body to the list of system replies.
