# Twilio personal account setup (awong.dev@gmail.com)

> **Historical:** the Twilio path this describes was removed 9 Oct 2026 (last at 05ec3ed). Kept for reference only; see docs/BRIDGE_PHONE_DESIGN.md.

Written 8 Oct 2026. A checklist for the owner. Every step is in the Twilio
console on the owner's side: the sign-up, the card, and the brand passcode
cannot be done by an agent.

## Why a personal account first

- US SMS from any carrier-grade provider needs 10DLC registration. Switching
  providers does not avoid it.
- Toll-free verification no longer avoids an EIN either (business registration
  number required since Feb 2026 except for sole proprietors; privacy policy and
  terms URLs required since 15 Sep 2026).
- A new EIN takes 30 to 90 days to propagate. A Sole Proprietor submission is
  flagged (error 30915) when records show an EIN or legal entity behind it.
- So: a **Sole Proprietor brand, no EIN, under awong.dev@gmail.com**, kept
  strictly personal (own name, personal use-case text, nothing naming
  SPS By The Numbers). Later, a **separate primary account** for the EIN entity
  (sps.by.the.numbers@gmail.com, ideally a sps-by-the-numbers.com mailbox).
- Nothing transfers between primary accounts: numbers only via support with
  both owners' approval, and registrations, verifications and opt-outs never
  move. Plan on buying a new number in the business account and repointing
  config. Skip the ISV route (needs the parent's primary profile approved first).
- Sole Proprietor limits: 1 number, 1 campaign, about 1,000 segments/day on
  T-Mobile. Brand approval is minutes (OTP to a personal mobile, not a Twilio
  number). Campaign review is days (unpublished).

## Steps

1. **Sign up** at twilio.com with awong.dev@gmail.com. Verify the email and a
   personal mobile number. That same mobile receives the brand passcode later,
   so use the phone you will have in hand. Onboarding questions: personal or
   hobby project, SMS, no language preference.
2. **Upgrade** under Billing with a card. It asks for an address and whether
   this is for an individual or a business. Choose **individual**. No EIN is
   requested here. A trial account only texts verified numbers and prefixes
   every message, so the upgrade is needed before real testing.
3. **Buy one number.** Phone Numbers, Buy a number, United States, SMS
   capable, **local** type. Not toll-free. About a dollar a month.
4. **Register the Sole Proprietor brand.** Messaging, Regulatory Compliance,
   Sole Proprietor path. Customer profile: your own name, home or PO box
   address, the gmail address, your mobile. Brand registration texts a passcode
   to that mobile; reply within 24 hours. Approval is minutes.
5. **Register the campaign** on the same screen. Use case Sole Proprietor.
   Description along the lines of: "personal family messaging: a parent's texts
   are relayed to a child's pager and the child's replies come back to the
   parent". Two sample messages. Opt-in: recipients are family members the
   parent adds in person. Keep every word personal; nothing about
   SPS By The Numbers. Review time is not published, so plan for days.
   If a form asks for privacy policy or terms URLs: `PRIVACY.md` and `TERMS.md`
   at the repo root (committed 8 Oct 2026, written in the owner's own name)
   are the text, but they need a public URL before they can be pasted (a page
   on kid-pager.web.app, or a public raw GitHub link).
6. **Messaging Service** named `pager`. Add the number to its sender pool, let
   the campaign attach to it, and turn **Advanced Opt-Out off**: the relay
   answers STOP/START/HELP itself (RELAY_SMS_CONSENT_TASKS.md), and with
   it on Twilio swallows those keywords before the webhook. Inbound request URL,
   HTTP POST, exactly this string, no trailing slash:
   ```
   https://pager-relay-2ix4jtetvq-uw.a.run.app/webhooks/twilio/sms
   ```
7. **Hand over the credentials through Secret Manager, not chat.** From the
   console home copy the Account SID and Auth Token into the two secrets
   Terraform already created:
   ```
   gcloud secrets versions add TWILIO_ACCOUNT_SID --project kid-pager --data-file=-
   gcloud secrets versions add TWILIO_AUTH_TOKEN  --project kid-pager --data-file=-
   ```
   Then say so. The agent flips `enable_sms_secrets` in
   `infra/envs/prod/terraform.tfvars` (currently commented out as false;
   `public_base_url` is already set), pushes, and the Deploy workflow wires the
   secrets into the relay.
8. **Assign the number** in the web app: People, your member row, SMS number.
9. **Test**: text the number from a phone the family has not approved. It
   should land as a held alert; approving it delivers it to the pager. Then run
   the relay SMS tests.

## Interim alternative if registration stalls

An Android handset SMS gateway (capcom6 android-sms-gateway or httpSMS) needs
no registration. Not pursued yet.

## What comes after

- Second account (the EIN entity) is where TWILIO_ACCOUNTS_DESIGN.md
  starts to matter. The single-account path works today without any of it.
