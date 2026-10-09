# Relay SMS: consent, keywords and disclosure (tasks)

> **Historical:** the Twilio path this describes was removed 9 Oct 2026 (last at 05ec3ed). Kept for reference only; see docs/BRIDGE_PHONE_DESIGN.md.

Owner spec, 8 Oct 2026. Extends docs/RELAY_SMS_DESIGN.md. The relay owns
opt-in/opt-out for the Twilio number instead of Twilio's Advanced Opt-Out
(which intercepts STOP/START/HELP before the webhook, so it must be **off** on
the Messaging Service for any of this to run). One agent, one pass.

## Read first

- relay/app/backends/sms_twilio.py (outbound `deliver()`, inbound helpers)
- relay/app/notify/sms.py (`send_sms`, the one Twilio call)
- relay/app/routers/webhooks.py `_handle_inbound_sms` (lines ~212-325)
- relay/app/routers/family.py `create_contact` (~690), `_approve_sms_unknown`
  (~806), `_approve_contact_request` (~891)
- relay/app/store/externals.py, relay/app/store/held_sms.py (store idiom)
- relay/tests/test_sms_twilio.py (fixtures `outbound`, `world`, `_inbound`,
  `_approved_contact`), relay/tests/test_routing.py `fake_send` fixture
- relay/README.md "Twilio" section (~lines 177-205)

## Files

New: relay/app/store/sms_consent.py, relay/app/sms_compliance.py,
relay/tests/test_sms_consent.py.
Edit: relay/app/backends/sms_twilio.py, relay/app/routers/webhooks.py,
relay/app/routers/family.py, relay/README.md, docs/RELAY_SMS_DESIGN.md (short
new decision 11 + Rejected/Failure-modes rows), existing tests that break.
Do not touch web/, infra/, firmware/.

## Do

### 1. Compliance text in one module: `relay/app/sms_compliance.py`

Constants (operator name and support email read from env
`SMS_OPERATOR_NAME` default `Albert Wong`, `SMS_SUPPORT_EMAIL` default
`awong.dev@gmail.com`, program name fixed `Pager`), and pure functions:

- `relay_body(sender_display_name, text) ->`
  `<Name> says: "<text>" - Pager (<operator>)`. No square brackets around the
  name (the spec's `[Name]` is a placeholder). `text` is the rendered message
  body (`_render_body` today: body, or the `location` preview).
- `DISCLOSURE = ". Reply STOP to opt out, HELP for help."` appended verbatim
  after the suffix on the first relayed message to a number per UTC calendar
  day.
- `WELCOME`, `OPT_OUT_REPLY`, `HELP_REPLY`: the three strings from the spec,
  exactly:
  - `Welcome to Pager, run by Albert Wong. You'll receive two-way coordination messages relayed from a Pager device. Message frequency varies. Msg & data rates may apply. Reply HELP for help, STOP to opt out.`
  - `You have been unsubscribed from Pager (Albert Wong). You will receive no further messages. Email awong.dev@gmail.com with any questions.`
  - `Pager (Albert Wong): two-way coordination messages relayed from a Pager device. For help, email awong.dev@gmail.com. Msg & data rates may apply. Reply STOP to opt out.`
  (operator/email substituted from the env values, so the defaults reproduce
  these byte for byte).
- `keyword(body) -> "start" | "stop" | "help" | None`: `body.strip().upper()`
  exact match. START/OPTIN/IN -> start; STOP/UNSUBSCRIBE/END/QUIT -> stop;
  HELP/INFO/SUPPORT -> help.

### 2. Store: `relay/app/store/sms_consent.py`, collection `smsConsent/{e164}`

Server-only, no rules entry needed (the deploy rules default-deny). Fields:
`status: "opted_in" | "opted_out"`, `optedInAt`, `optedOutAt` (server
timestamps, nullable), `source: "keyword" | "admin"`, `lastDisclosureDate:
"YYYY-MM-DD" | null`, `updatedAt`. Functions:

- `get(e164) -> Consent | None`
- `mark_opted_in(e164, *, source) -> bool`: upsert; returns True iff the
  status **changed to** opted_in (was missing or opted_out). The caller sends
  the welcome only on True.
- `mark_opted_out(e164) -> None`
- `is_opted_in(e164) -> bool` (row exists and status == opted_in)
- `claim_disclosure(e164, today: str) -> bool`: True iff `lastDisclosureDate
  != today`, and sets it. Use a transaction (read, compare, write) so two
  instances don't both append; household scale, no index.

### 3. Outbound in `SmsTwilioBackend.deliver()`

After the `from_number` check and before `send_sms`:

- If `not sms_consent.is_opted_in(phone)`: log
  `sms out to=... from=... sid=- status=failed code=not_opted_in` (or
  `opted_out` when the row says opted_out), `mark_delivery_failed_if_queued`,
  return `DeliverResult(ok=False, state="failed", error=<code>)`. Never
  retried.
- Body = `relay_body(sender.displayName, defang(_render_body(msg)))`. Then
  `if sms_consent.claim_disclosure(phone, today_utc)`: append `DISCLOSURE`.
  If the send then fails (`not result.ok`), the claim stands (simplest; a
  retry the same day goes out without the disclosure). Document that in the
  module docstring.

Compliance replies (welcome, opt-out confirmation, help) and the existing
too-long hint go through `sms_client.send_sms` directly and are **not** subject
to the opt-in gate: the opt-out confirmation must reach a number that just
opted out.

### 3b. Defang URLs and phone numbers in the relayed text (owner item 6)

`defang(text) -> str` in `app/sms_compliance.py`, applied in `deliver()` to the
rendered message text **only**, before `relay_body` wraps it (never to the
prefix, suffix or disclosure). Human-visible, human-undoable spacing:

- **URLs**: anything matching `scheme://host...`, `www.host...`, or a bare
  `host.tld` (labels of `[A-Za-z0-9-]`, final label 2+ letters). Insert one
  space after `://` when present and after each `.` in the **host portion**
  (the host ends at the first `/`, `?`, `#`, `:` or whitespace; the path is
  untouched). `https://example.com/path` -> `https:// example. com/path`;
  `www.foo.co.uk/x` -> `www. foo. co. uk/x`. Run this pass first.
- **Phone numbers**: a span of 7+ digits optionally mixed with `-`, `.`, `(`,
  `)` and spaces. The spec says "after every third digit" but its examples
  count **characters** of the span, so match the examples: split the span on
  spaces; every chunk longer than 3 characters gets a space inserted after
  every third character; chunks of 3 or fewer are left as they are.
  `2065551234` -> `206 555 123 4`; `206-555-1234` -> `206 -55 5-1 234`;
  `(206) 555-1234` -> `(20 6) 555 -12 34`.
- **Already-defanged text is left alone**: `https:// example. com/path` and
  `206 555 123 4` and `206 -55 5-1 234` come back unchanged (no chunk longer
  than 3, no host.tld match). Running `defang` twice equals running it once.

Unit tests (in `tests/test_sms_consent.py` or a `test_sms_defang.py`): each
of the three URL shapes, each phone example above, a sentence with both, a
6-digit number untouched, `e.g.` and `Mr. Smith` untouched, idempotence, and
that the full outbound body keeps the prefix/suffix intact around defanged
text.

### 4. Inbound keywords in `_handle_inbound_sms`

Insert right after `from_number` is normalised and **before** the
`blockedNumbers` check (STOP must work even for a blocked number). Use the raw
`Body` param:

- `start`: `changed = mark_opted_in(from_number, source="keyword")`; always
  reply `WELCOME` from `to_number`; return `"keyword_start"`.
- `stop`: `mark_opted_out(from_number)`; reply `OPT_OUT_REPLY`; return
  `"keyword_stop"`.
- `help`: reply `HELP_REPLY`; return `"keyword_help"`.

Keyword messages are never stored, held, or routed to a pager. Nothing else
in the handler changes (a non-keyword text from an opted-out number is still
held/delivered as today; only outbound is suppressed).

### 5. Admin consent (`family.py`)

Add one helper in `relay/app/routers/family.py` (or `app/book.py` if cleaner),
`_consent_by_admin(family_id, e164, from_number: str | None)`:
`if sms_consent.mark_opted_in(e164, source="admin")`: send `WELCOME` from
`from_number`; if `from_number` is None pick the family's first person (by
alias) with a `smsNumber`; if none, log a warning `sms welcome skipped: no
member number` and leave the row opted_in. Call it from:

- `create_contact` (Contacts page add), `from_number=None`.
- `_approve_sms_unknown` (held alert approve), `from_number = target
  member's smsNumber` (the number the texter wrote to). Call it **before**
  delivering the backlog, so the pager's reply to that backlog is not
  suppressed.
- `_approve_contact_request` where it creates an external, `from_number=None`.

### 6. Docs

- relay/README.md Twilio section: step 6 becomes "Turn **Advanced Opt-Out
  off**: the relay answers STOP/START/HELP itself (keywords, replies, and the
  once-a-day disclosure in `app/sms_compliance.py`; consent rows in
  `smsConsent/{e164}`)"; add the outbound format, the opt-in rule (no outbound
  to a number that has not opted in, by keyword or by an admin adding/
  approving it), the `code=not_opted_in|opted_out` log values and the
  `outcome=keyword_*` values.
- docs/RELAY_SMS_DESIGN.md: decision 11 (consent owned by the relay, the
  collection, the keyword table, the disclosure rule, admin consent = welcome),
  Failure modes row for `not_opted_in`, Rejected: "Twilio Advanced Opt-Out"
  with the reason (it hides the keywords from the webhook, and the carrier
  reply text must name the operator).
- relay/.env.example: `SMS_OPERATOR_NAME`, `SMS_SUPPORT_EMAIL`.

## Tests (`relay/tests/test_sms_consent.py`, reuse the fixtures from
test_sms_twilio.py by import or a small conftest move)

- Each keyword in each spelling/case (`" stop "`, `Quit`, `OPTIN`, `in`,
  `Info`): the reply body is exact, `From` is the member's number, nothing is
  stored (no held row, no message, no alert), outcome logged.
- A non-keyword that merely contains a keyword (`"please stop"`) is not a
  keyword.
- START twice: welcome both times, row stays opted_in.
- Opted-out number: a pager text to it is `failed` with `opted_out`, no Twilio
  call; after START it goes out again.
- Number never opted in: `failed` with `not_opted_in`.
- Outbound format: `Kid says: "hi" - Pager (Albert Wong)` plus the disclosure
  on the first send of the day; second send the same day has no disclosure;
  freeze/monkeypatch the date and show a new day appends it again.
- Admin paths: `POST /api/family/contacts` sends one welcome and marks the
  row; approving a held alert sends the welcome from the member's number
  before the backlog and the backlog delivery succeeds; adding the same
  contact again sends no second welcome.
- Existing tests: update the ones that now see an extra welcome or a
  `not_opted_in` failure (`test_pager_up_to_contact_goes_out...`,
  approve tests, test_routing.py sms cases, test_rules.py if it asserts
  `smsConsent` is unreadable: add that assertion). Keep every other
  assertion.

## Verify

```
cd relay && .venv/bin/python -m pytest -q
```
All green, including the Firestore-rules tests if the suite has them. If
`tools/e2e_v2.py relay_sms` can run against the compose stack in this
environment, run it and update the scenario for the welcome + format;
otherwise say so and list the exact edits the scenario needs.

Report: files changed, test counts before/after, the exact outbound body
produced in the format test, and anything in the spec you had to interpret.
