# Relay SMS across many Twilio accounts (proposal, 8 Oct 2026)

*(owner, 8 Oct 2026: "we might eventually have to handle multiple twilio EIN style accounts.
Possibly a custom endpoint per family, or one account per number. Propose a design to handle a
constellation of twilio accounts -- potentially one personal account per number.")*

Status: proposal, not reviewed, not built. Extends `docs/RELAY_SMS_DESIGN.md`; decisions 1-8
there stand (a number belongs to a person, unknown inbound is held, approve delivers the backlog).
Only decisions 9 and 10 (one account's credentials in the environment, one Messaging Service)
change.

## 1. Why

- Twilio's Sole Proprietor 10DLC program is the only registration that clears in minutes, and it
  allows **one number and one campaign per registration**. A family whose parent registers that
  way is, by construction, one Twilio account per number.
- The owner's EIN entity (SPS By The Numbers) will register on its own primary account later.
  Brands do not transfer between primary accounts and numbers lose their registration when moved,
  so the relay must speak to several accounts at once rather than wait for one to absorb the rest.
- Subaccounts (the ISV shape) have their own SID and auth token too. One model covers personal
  accounts, the EIN account and any subaccount: **a Twilio account is a row with a SID and a
  token; a number points at the account it lives in.**

## 2. Decisions

1. **Account registry.** Firestore `twilioAccounts/{accountSid}`:
   `{label, kind: "sole_prop" | "standard" | "subaccount", ownerFamilyId: string | null,
   status: "active" | "disabled" | "auth_failed", tokenSecret: string, createdAt, createdBy}`.
   `ownerFamilyId = null` means platform-owned (super admin's accounts); a family id means the
   family brought its own account. The auth token is never in Firestore: `tokenSecret` is the
   Secret Manager secret name `twilio-token-<AccountSid>`.
2. **Number to account.** `smsNumbers/{e164}` (the existing index behind `users.smsNumber`)
   gains `accountSid`. Assigning a number to a person (People -> member -> SMS number) now also
   picks the account from a list the admin may use: super sees all, a family admin sees the
   platform accounts plus their own family's. A number can belong to one account only; the
   relay refuses a second assignment of the same E.164 (already the case).
3. **Outbound.** `sms_twilio.deliver()` resolves `From` -> `smsNumbers[From].accountSid` ->
   account -> token, and `send_sms()` takes `(account_sid, token)` instead of reading the
   environment. The URL is `/2010-04-01/Accounts/{account_sid}/Messages.json` on
   `TWILIO_BASE_URL` (the mock keeps working: it ignores the SID path segment).
4. **Inbound: one URL for every account.** Twilio posts `AccountSid` with every message
   webhook. `POST /webhooks/twilio/sms` reads it, loads that account (unknown or disabled ->
   401, nothing verified), verifies `X-Twilio-Signature` with **that account's** token, then
   additionally requires `smsNumbers[To].accountSid == AccountSid` (a token leaked from account
   A cannot inject texts for account B's numbers). Everything after that is decision 4 of
   RELAY_SMS_DESIGN unchanged. The signed URL is still `PUBLIC_BASE_URL/webhooks/twilio/sms`
   byte for byte, in every account's console.
5. **Token storage and access.** One Secret Manager secret per account, created by the relay
   when a super admin (phase 1) or family admin (phase 2) pastes SID + token into the web app
   (`POST /api/admin/twilio-accounts {sid, token, label, kind}`; the token is write-only, never
   echoed). The relay service account gets `roles/secretmanager.admin` **with an IAM condition**
   `resource.name.startsWith("projects/<project>/secrets/twilio-token-")`, so it can create,
   add a version to, and read only those secrets (Terraform, `infra/modules/relay-service`).
   Tokens are cached in process memory for 10 min; `disabled` and a token rotation (re-paste)
   invalidate the cache entry.
6. **Verify on add, read-only.** When an account is added the relay calls
   `GET /2010-04-01/Accounts/{sid}.json` with the pasted token; a 401 rejects the paste with a
   clear message. When a number is assigned to an account the relay calls
   `GET /Accounts/{sid}/IncomingPhoneNumbers.json?PhoneNumber=<e164>` and refuses a number the
   account does not own. Both are reads; RELAY_SMS_DESIGN's rejection of buying numbers or
   editing console config through the API stands.
7. **Who may do what.** Super: add, disable, delete (only with zero numbers) any account; assign
   any number. Family admin: assign numbers from platform accounts or their own family's
   accounts to their own members; phase 2 lets a family admin add an account with
   `ownerFamilyId = their family`. A disabled account's numbers keep routing inbound as held
   (so nothing is lost) and fail outbound with `account_disabled`.
8. **Failure handling.** Outbound 401 from Twilio -> account `status: auth_failed`, delivery
   stays `queued` (transient), one `twilio_auth` alert to the super admins and to the owning
   family's admins; a successful re-paste clears it. Inbound signature failure is logged with the
   account label and counted; ten in a minute raises the same alert. Twilio 20003/20005 (account
   suspended/closed) -> `auth_failed` too. Sole-prop daily limits (about 1,000 segments to
   T-Mobile) are not enforced by the relay; the delivery's Twilio error code is stored as today.
9. **Migration.** `enable_sms_secrets` is still false in prod and the two Terraform secrets
   `twilio_account_sid` / `twilio_auth_token` have no versions, so there is nothing to migrate:
   those two secrets and the `TWILIO_ACCOUNT_SID` / `TWILIO_AUTH_TOKEN` env wiring are removed.
   `TWILIO_BASE_URL` and `PUBLIC_BASE_URL` stay. Dev/compose: a seed `twilioAccounts` row whose
   secret resolves through a `TWILIO_DEV_TOKENS` env map (`sid=token,...`) instead of Secret
   Manager, selected by `SECRETS_BACKEND=env`; the mock accepts any Basic auth.
10. **Console steps per account (owner or family admin, by hand).** Standard/EIN account: one
    Messaging Service with the campaign, numbers in its sender pool, inbound URL as above.
    Sole-prop account: the single number's own inbound URL is set directly on the number (a
    Messaging Service is optional there), Advanced Opt-Out on. Both documented in
    `relay/README.md` "Twilio".

## 3. Rejected

- **A custom endpoint per family** (`/webhooks/twilio/sms/{familyId}`): the `AccountSid` in the
  POST body plus the per-account signature already identifies and authenticates the sender; a
  path segment would duplicate that, put family ids into Twilio console config, and multiply
  the byte-exact URL trap in RELAY_SMS_DESIGN decision 9.
- **Tokens in Firestore** (plain or KMS-wrapped): Secret Manager is the project's one place for
  secrets, and a scoped IAM condition keeps the relay's grant narrow.
- **Credentials in the environment per account**: a redeploy per family is not acceptable.
- **Using the Twilio API to buy numbers or set webhooks**: unchanged rejection (cost of a wrong
  purchase, and the campaign attachment is a console step anyway).
- **Parent-account token for subaccounts**: a subaccount is just another row; the parent's
  token never enters the relay.

## 4. Transaction boundaries

- Add account: Secret Manager create + version first, then the Firestore row; a Firestore
  failure deletes the secret. Re-paste: new version, then cache invalidate.
- Assign number: one Firestore transaction over `users/{uid}`, `smsNumbers/{e164}` (now with
  `accountSid`), exactly as today plus the field.
- Delete account: refused unless `smsNumbers` has no row with that `accountSid`; then Firestore
  row, then secret (a leftover secret is harmless and listed by label for cleanup).

## 5. Tests

- Unit: account lookup by SID; signature verified with the right token and rejected with a
  sibling account's; `To` belonging to another account -> 401; outbound picks the account from
  `From`; `auth_failed` on 401 with alert; disabled account inbound -> held, outbound -> failed.
- E2E (compose, mock with two SIDs): two families on two accounts, inbound to each, cross-account
  forgery rejected, outbound From each.

## 6. Open questions for the owner

1. Phase 2 (family admins paste their own account) now or later? Phase 1 alone covers "one
   personal account per number" as long as the super admin does the paste.
2. Should a number assignment require the read-only ownership check (decision 6), or is an
   admin's choice trusted? The check costs one Twilio call per assignment.
3. Alerts for `auth_failed`: super only, or also the owning family's admins?
