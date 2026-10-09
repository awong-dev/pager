# infra/ — deployment runbook

This is the **only** place real cloud resources get touched for this project, and only by a
human running these steps by hand (or by CI, once step 9 below has been done). This tree has been
applied to the real project: the owner ran `terraform apply` by hand (7 Oct 2026, for the OTA bucket
and relay variables), and the Deploy workflow runs `terraform apply` on every push to `main` with the
sha-tagged relay image (§8). Read `docs/SERVER_PLAN.md` §9 first; this file is the "how", that section
is the "what and why".

**Never commit**: `terraform.tfvars` (only `terraform.tfvars.example` is tracked), any
`.tfstate` file, any real secret value, or a service-account JSON key (none should ever be
created — this whole layout is Workload Identity Federation, on purpose).

---

## 0. Prerequisites

- `gcloud` CLI, authenticated as a human with Owner (or Project Creator + Billing Account User)
  on whatever GCP org/folder the project will live in.
- `terraform` >= 1.14 (this tree was built and validated against 1.14.9), `docker`, `node` 20+
  (for `web/`), `firebase-tools` (`npx firebase-tools`, no global install
  needed).
- A GitHub repo (this one) with Actions enabled.

## 1. Create the GCP project and enable billing (by hand, not Terraform)

`infra/bootstrap/variables.tf` assumes the project already exists — project *creation* and
*billing account linkage* are one-time, human, security-sensitive actions kept outside Terraform
on purpose (nothing here should be able to provision a billing-linked project on its own).

```
gcloud projects create <PROJECT_ID> --name="Pager"
gcloud billing projects link <PROJECT_ID> --billing-account=<BILLING_ACCOUNT_ID>
```

Then, in the [Firebase console](https://console.firebase.google.com/), add Firebase to this
project and upgrade it to the **Blaze** plan (`docs/SERVER_PLAN.md` §9.2: Cloud Run and
Scheduler both require it; Firestore/Auth/FCM/Hosting keep their no-cost allowances on Blaze).
This upgrade has no Terraform resource — Google does not expose it as one.

## 2. Bootstrap: APIs + Terraform state bucket (local state, applied once)

```
cd infra/bootstrap
terraform init
terraform apply -var project_id=<PROJECT_ID> -var state_bucket_name=<PROJECT_ID>-tfstate
```

Note the `state_bucket_name` output — step 4 needs it. This directory's state stays **local**
forever (see its `versions.tf` comment on why it cannot depend on its own output bucket); do not
try to migrate it to a remote backend.

## 3. Point `infra/envs/prod` at the real backend

Edit `infra/envs/prod/versions.tf`'s `backend "gcs" { bucket = "..." }` to the bucket name from
step 2 (or leave the placeholder and pass `-backend-config="bucket=<name>"` to `terraform init`
instead, if you'd rather not edit a tracked file with an environment-specific value — either
works; editing the file is simpler for a single-environment project like this one).

## 4. Fill in `terraform.tfvars`

```
cd infra/envs/prod
cp terraform.tfvars.example terraform.tfvars
# edit terraform.tfvars — see that file's own comments for each value.
# `broker_api_url` needs step 10 (EMQX Cloud Serverless) done first, or a placeholder now and a
# real `terraform apply` again later once you have it.
terraform init
```

## 5. First apply, part 1: everything that doesn't need a real container image yet

Cloud Run refuses to create a service/job revision unless its image already exists and is
pullable — so the relay Cloud Run service can't be created in the same apply as its own first
image push. Split it:

```
terraform apply \
  -target=module.secrets \
  -target=module.firebase \
  -target=module.ci_deploy \
  -target=module.relay_service.google_artifact_registry_repository.relay
```

This creates: the Secret Manager containers, the Firebase project/Firestore/Auth/Hosting
resources, the GitHub Actions WIF pool+provider+deploy SA, and (just) the Artifact Registry
repository the next step pushes into.

## 6. Add secret values (never through Terraform)

```
gcloud secrets versions add BROKER_API_KEY    --project <PROJECT_ID> --data-file=-
gcloud secrets versions add BROKER_API_SECRET --project <PROJECT_ID> --data-file=-
gcloud secrets versions add WEBHOOK_KEY       --project <PROJECT_ID> --data-file=-
```

(each command reads the secret value from stdin — type/paste it and press Ctrl-D, or pipe it
from a password manager's CLI; never leave it in shell history or a file that gets committed).
**Do not let a trailing newline into the value**: pipe with `printf '%s' "$VALUE"`, never `echo`.
Secret Manager stores exactly the bytes it is given, an HTTP header can never carry a newline, and
a `WEBHOOK_KEY` stored with one made every broker webhook fail with 401 on the first real
deployment. The relay now strips whitespace from these three values, but fix it at the source.
`BROKER_API_KEY`/`BROKER_API_SECRET` must be the **exact same pair** you configure as EMQX
Cloud's REST API key in step 10 below — do steps 6 and 10 together, in whichever order is
convenient, but make sure the values end up identical on both sides. Generate a `WEBHOOK_KEY`
yourself (e.g. `openssl rand -hex 32`) — it just needs to match what you configure into the EMQX
rule engine's HTTP action header in step 10.

**Relay SMS (per-user Twilio numbers, `docs/RELAY_SMS_DESIGN.md` decisions 9 and 10).** Terraform
creates empty `TWILIO_ACCOUNT_SID` and `TWILIO_AUTH_TOKEN` containers (there is no
`TWILIO_FROM_NUMBER`: each member's own number is the sender) and sets the plain env
`TWILIO_BASE_URL=https://api.twilio.com`. Order: deploy the relay first, then `terraform apply`
(the apply on its own only adds empty containers, so the order is not critical). Then:

a. Add the secret versions (values from the Twilio console, Account Info; no trailing newline):

   ```
   printf '%s' "$TWILIO_ACCOUNT_SID" | gcloud secrets versions add TWILIO_ACCOUNT_SID --data-file=- --project <PROJECT>
   printf '%s' "$TWILIO_AUTH_TOKEN"  | gcloud secrets versions add TWILIO_AUTH_TOKEN  --data-file=- --project <PROJECT>
   ```

b. Keep `PUBLIC_BASE_URL` (`public_base_url`) as the Cloud Run `run.app` origin
   (`terraform output -raw relay_service_url`), no trailing slash, unchanged. Twilio is pointed
   directly at Cloud Run, not at the Firebase Hosting origin, so the URL Twilio signs
   (`<run.app origin>/webhooks/twilio/sms`) equals what the relay verifies, and the device CA
   pointer `/ca/{sha}.pem` keeps working. No `firebase.json` change and no second setting.
c. Set `enable_sms_secrets = true` (repo variable `ENABLE_SMS_SECRETS=true` for CI) and
   apply/deploy again. Do this only after (a): Cloud Run refuses a revision that references a
   secret with zero versions.
d. Twilio console: create one Messaging Service "pager" with the 10DLC campaign attached; add each
   member's number to its sender pool; set the service's inbound request URL to
   `<PUBLIC_BASE_URL>/webhooks/twilio/sms` (the run.app URL) (HTTP POST). Leave Advanced Opt-Out on. Then assign
   each number to a member in the web app (People, member drawer, "SMS number").

**`CELL_GEO_API_KEY`** (docs/PROTOCOL.md §13.2 — cell-tower location fallback) is only needed if
you are enabling `CELL_GEO_PROVIDER=google` (or `opencellid`); leave `enable_cell_geo_secret =
false` and `cell_geo_provider = "none"` in `terraform.tfvars` until you have a real key — the
relay works exactly as it does today with `none` (no third-party lookup, nothing stored beyond
`devices/{d}.status.lastCell`). To create a Google Geolocation API key:

1. Google Cloud Console → this project → **APIs & Services → Library** → enable the **Geolocation
   API**.
2. **APIs & Services → Credentials → Create credentials → API key.**
3. **Restrict the key**: "Restrict key" → **API restrictions** → select only **Geolocation API**
   (do *not* leave it unrestricted — a key that can also call, say, the Maps or Places APIs is a
   much more valuable thing to leak). There is no IP/referrer restriction that makes sense here
   (the caller is a Cloud Run service, not a browser or a fixed-IP host), so API restriction is the
   only meaningful one available.
4. `gcloud secrets versions add CELL_GEO_API_KEY --project <PROJECT_ID> --data-file=-` (same
   no-trailing-newline caution as the other secrets above), then set `cell_geo_provider = "google"`
   and `enable_cell_geo_secret = true` in `terraform.tfvars` (or as the `CELL_GEO_PROVIDER` /
   `enable_cell_geo_secret` values in step 9's CI wiring) and `terraform apply` again.

**Cost at this project's volume**: the Geolocation API bills per request past a monthly free
allowance; a school pager answering a handful of `no_fix` `loc_req`s a day, almost all of which hit
`app/cellgeo.py`'s 30-day cache after the first lookup per cell (a handful of distinct cells at
one school + home), stays a tiny fraction of that free allowance — realistically $0/month, but
**verify current Google Geolocation API pricing before relying on that** (pricing pages change; this
repo does not track it). OpenCelliD's own pricing/free-tier terms are unverified here too — see
`app/cellgeo.py`'s docstring on that provider's `UNVERIFIED` API shape.

## 7. Build and push the first relay image by hand

```
gcloud auth configure-docker <REGION>-docker.pkg.dev
docker build -t <REGION>-docker.pkg.dev/<PROJECT_ID>/pager/relay:bootstrap relay/
docker push <REGION>-docker.pkg.dev/<PROJECT_ID>/pager/relay:bootstrap
```

## 8. First apply, part 2: everything else

```
terraform apply -var relay_image=<REGION>-docker.pkg.dev/<PROJECT_ID>/pager/relay:bootstrap
```

(The explicit `-var relay_image=...` is required only here, because the service does not exist
yet; with it empty, `envs/prod/main.tf` looks the running image up from the existing service via
`data.google_cloud_run_v2_service`, which fails if the service is absent.)

**Local apply never changes the relay image; CI owns it.** Leave `relay_image` out of
`terraform.tfvars`. Without it, a local `terraform apply` keeps whatever image Cloud Run is
currently serving; only the Deploy workflow's `-var relay_image=<repo>:<sha>` rolls it forward.
Incident, 7 Oct 2026: a local `terraform.tfvars` line `relay_image = ".../relay:latest"` pointed
at a weeks-old image (nobody retags `latest`), the apply rolled Cloud Run back and sign-in 500ed
until the Deploy workflow was rerun.

This creates the Cloud Run service + the `bootstrap` job + their service
account and IAM roles, the Cloud Scheduler `tick`/`sweep` jobs + their OIDC caller identity, and
the Cloud Tasks queue. Check the `relay_service_url` output — `GET <that URL>/healthz` should
now return 200 (once a working `BROKER_API_URL` is in `terraform.tfvars`; a placeholder URL
there just means `/healthz`'s broker-reachability check fails, not that the service is down).

**Known gap, expected at this point**: `/internal/tick` and `/internal/sweep` will **401**.
`relay/app/routers/internal.py` does real OIDC verification (signature + `aud` ==
`OIDC_AUDIENCE` + caller `email` in `OIDC_ALLOWED_EMAILS`), and it fails closed when those two
env vars are unset — which they are, because `infra/modules/relay-service` does not set them.
Until it does, the Scheduler `tick`/`sweep` jobs will call a route that 401s, so **no tick
retries and no retention sweep run in this deployment**. See
`infra/modules/schedule/variables.tf`'s module docstring for the exact one-apply fix (a shared
`custom_audiences` string for `OIDC_AUDIENCE`, and the deterministic
`pager-scheduler@<project>.iam.gserviceaccount.com` email for `OIDC_ALLOWED_EMAILS`) — track it
as a real follow-up before relying on scheduled jobs.

**Second known gap, same root cause (cyclic self-reference): `public_base_url`.** Once a CA is
pinned (`broker_ca_pem_file` above), devices need `PUBLIC_BASE_URL` set to something real so their
bootstrap bundle's CA pointer (`GET /ca/{sha256hex}.pem`, docs/V02_DESIGN.md §4.4) resolves — Cloud
Run v2 cannot reference a service's own `.uri` from inside the same `apply` that creates it, so
`infra/envs/prod/variables.tf`'s `public_base_url` has no computed default. After this first apply,
run `terraform output -raw relay_service_url`, set `public_base_url` in `terraform.tfvars` to that
value (production today: `https://pager-relay-2ix4jtetvq-uw.a.run.app`), and `terraform apply`
again. Until that second apply, a device create/rotate against a deployment with a CA pinned fails
with a 500 (`app/devsetup.py` refuses to issue an unpinned bundle silently) rather than a device
that can never validate its CA pointer.

## 9. Wire up GitHub Actions (turns `.github/workflows/deploy.yml` from a no-op into a real pipeline)

Status (8 Oct 2026): done. The Deploy workflow runs for real: it pushes the sha-tagged relay image,
applies Terraform with that image, and deploys Hosting and Firestore. It is no longer a no-op.

Read that workflow's own top-of-file comment for the exact gating mechanism first. Then:

- Repo **secrets** (Settings → Secrets and variables → Actions → Secrets):
  - `GCP_WORKLOAD_IDENTITY_PROVIDER` = `terraform output -raw ci_deploy_workload_identity_provider` (from `infra/envs/prod`)
  - `GCP_DEPLOY_SERVICE_ACCOUNT` = `terraform output -raw ci_deploy_service_account_email`
  - `NEXT_PUBLIC_FIREBASE_VAPID_KEY` (docs/V03_PLAN.md §3a, task 3a.3 -- real push notifications).
    A repo secret rather than a repo variable, per that task's explicit call, even though a Web
    Push certificate's public key is not sensitive the way the two secrets above are. **Manual
    step, Firebase console:** Project settings → Cloud Messaging → "Web Push certificates" →
    Generate key pair → copy the public key string → paste it in as this secret's value. Do this
    *before* the first deploy that includes 3a.2's `firebase-messaging-sw.js` generator: that
    prebuild script fails the build if any `NEXT_PUBLIC_FIREBASE_*` value -- including this one --
    is empty, so an unset secret breaks `firebase-deploy` entirely once 3a.2 lands, not just push
    notifications specifically. `infra/envs/prod/main.tf` separately sets the relay Cloud Run
    service's `PUSH_BACKEND=fcm` (no repo variable/secret needed for that half -- it's a fixed
    value in Terraform); the two are independent (this secret lets the *browser* subscribe to
    push, `PUSH_BACKEND` lets the *relay* send it) and both are needed for 3a to work end to end.
    **IAM, already covered:** the relay service account already has `roles/
    firebasecloudmessaging.admin` (`infra/modules/relay-service/main.tf`'s `relay_roles` list,
    granted for a different reason originally -- "webapp backend's FCM sends") which includes the
    `firebasecloudmessaging.messages.create` permission `firebase_admin.messaging.send_each_for_
    multicast()` needs; no new binding was required for this task. `infra/bootstrap/main.tf` does
    now also enable `fcm.googleapis.com` (added by this task) -- rerun bootstrap's `terraform
    apply` (step 2) if this deployment predates that line, or the relay's first real FCM send
    will 403.
- Repo **variables** (same page, "Variables" tab):
  - `GCP_PROJECT_ID`, `GCP_REGION`, `BROKER_API_URL`, `BROKER_HOST` (same values as `terraform.tfvars`)
  - `BROKER_CA_PEM_FILE` (optional; same value as `terraform.tfvars`' `broker_ca_pem_file`, e.g.
    `certs/digicert-global-root-g2.pem`). Leave it unset to pin no CA. **If `terraform.tfvars` sets it
    and this repo variable does not, the next CI deploy silently un-pins:** setup codes issued after
    that carry no CA. Devices already set up keep whatever CA they were given.
  - `PUBLIC_BASE_URL` (same value as `terraform.tfvars`' `public_base_url`, §8's second known gap
    above -- the deployed Cloud Run URL, once known). Only load-bearing once a CA is pinned; if this
    repo variable is unset while `BROKER_CA_PEM_FILE` is set, the next CI deploy makes device
    create/rotate start failing with a 500 instead of silently un-pinning (`app/devsetup.py`).
  - `CELL_GEO_PROVIDER` (optional; docs/PROTOCOL.md §13.2 -- cell-tower location fallback). Unset
    or empty deploys with `"none"` (`.github/workflows/deploy.yml`'s own fallback), the same
    no-lookup default as everywhere else. Set to `google` (or `opencellid`) only once
    `CELL_GEO_API_KEY` has a real value (step 6 above) **and** `enable_cell_geo_secret = true` in
    `terraform.tfvars` -- this repo variable alone does not wire the secret in.
  - `FIREBASE_PROJECT_ID`, `FIREBASE_API_KEY`, `FIREBASE_AUTH_DOMAIN`, `FIREBASE_STORAGE_BUCKET`,
    `FIREBASE_MESSAGING_SENDER_ID`, `FIREBASE_APP_ID` -- the web app's Firebase config (same values as
    `web/.env.local`'s own `NEXT_PUBLIC_FIREBASE_*`, from the Firebase console's Project Settings ->
    General -> "Your apps" -> the web app's config snippet). Not secret -- a Firebase web app's config
    is meant to be public, the project is protected by Firestore/Auth rules -- but they must be set as
    repo variables regardless, or `firebase-deploy`'s build silently ships the demo-project fallback
    baked into `web/lib/firebase.ts`, and every real sign-in fails with `auth/api-key-not-valid`.

Once both secrets exist, a push to `main` (or a manual `workflow_dispatch` run) builds the relay
image, pushes it, runs `terraform apply` via WIF, and runs `firebase deploy --only
hosting,firestore`. The `firebase-deploy` job additionally needs `web/` to have a working
`npm run build`. If that build breaks, the `firebase-deploy` job fails loudly rather than
swallowing the error.

On where `firebase.json` lives: the root-level `firebase.json`/`.firebaserc` exist purely for this
deploy step. Local dev uses `relay/firebase.json` (emulator config +
`firestore.rules`/`firestore.indexes.json` paths) and `web/firebase.json` (a local Hosting preview)
independently; both point at the same `relay/firestore.rules` / `relay/firestore.indexes.json`
rather than duplicating them.

## 10. EMQX Cloud Serverless console setup (needs a real account)

No Terraform provider exists for EMQX Cloud (`docs/SERVER_PLAN.md` §9.4) — this is all
console/API work, by hand:

1. Sign up at [emqx.com/cloud](https://www.emqx.com/en/cloud) (Serverless tier, free, and no card
   was required last time this was checked — reverify).
2. Create a Serverless deployment. Verify the three assumptions `docs/SERVER_PLAN.md` §10 D2 marks
   **OPEN**, in order, and **stop and use `infra/modules/broker-gce` instead if any fail**:
   - Rule engine supports an HTTP action on this tier.
   - The REST publish API (`/api/v5/publish`) is available on this tier.
   - The HTTP action's timeout can be set **≥ 15 s** (a cold Cloud Run relay can take 2-4 s to
     even start responding — `relay/emqx/`'s local dev config uses a 10 s `request_ttl`, which is
     too short for production).
3. Replicate the exact rule/connector/action shape `tools/emqx_setup.py` creates locally
   (`CONNECTOR_NAME = relay_webhook`, `ACTION_NAME = relay_webhook_action`, `RULE_ID =
   pager_to_relay`, topics `pager/+/up`, `pager/+/status`, `pager/+/loc` **and**
   `pager/boot/+/up`), pointed at this
   deployment's real `<relay_service_url>/webhooks/mqtt` instead of the compose network address,
   with the `X-Relay-Webhook-Key` header set to the same value you put in `WEBHOOK_KEY` (step 6).
   The rule SQL **must** carry a base64 copy of the payload, because devices speak CBOR and EMQX's
   JSON encoder destroys non-text bytes:
   `SELECT topic, payload, base64_encode(payload) as payload_b64, qos, clientid FROM "pager/+/up",
   "pager/+/status", "pager/+/loc", "pager/boot/+/up"`. Action: `POST`, header `content-type:
   application/json`, body `${.}`. This was done by hand in the console for the production
   deployment on 2026-09-20 and **exists nowhere else**: a rebuilt deployment has to redo it.
   To check it, restart a pager and look for a `POST /webhooks/mqtt` `200` in the relay's logs
   (`401` = the key header does not match the secret).
   `infra/modules/broker-gce/startup-script.sh.tpl` is a worked example of this same shape
   re-implemented in curl, in case the EMQX Cloud console makes it easier to look at a script
   than reconstruct it purely from the dashboard.
4. Generate a REST API key/secret pair for the relay's own publishes; put that exact pair into
   `BROKER_API_KEY`/`BROKER_API_SECRET` (step 6) and put the deployment's REST API base URL into
   `terraform.tfvars`'s `broker_api_url`, then `terraform apply` again to update the Cloud Run
   service's env.
5. Record whatever you found in step 2 back into `docs/SERVER_PLAN.md` §9.4 and §10 D2 — this
   file doesn't do that for you.
6. **Device credentials and ACLs are not provisioned anywhere.** `POST
   /api/admin/devices` mints and returns an MQTT credential, but nothing pushes it (or
   `PROTOCOL.md` §2's three ACL rules) into the broker. If EMQX Cloud Serverless's console/API
   exposes credential+ACL management, either use it by hand per device, or wire the admin API to
   push automatically as a backend follow-up (`SERVER_PLAN.md` §5.5 describes the intended shape: "the relay pushes the device credential ... otherwise
   the admin UI shows 'add these to the broker' copy"). Until one of those exists, a device
   created via the admin API cannot actually authenticate to the broker.

## 11. Create the first admin

```
gcloud run jobs execute pager-relay-bootstrap \
  --region <REGION> --project <PROJECT_ID> \
  --args="--admin-email=<the admin's real email>"
```

(`docs/SERVER_PLAN.md` §5.3 — this is `python -m app.bootstrap --admin-email ...`, idempotent,
safe to rerun. The email is passed at execution time, not baked into Terraform, so it never ends
up in state or this repo — see `infra/modules/relay-service/main.tf`'s comment on the job.)

## 12. Multi-family cutover: wipe and re-bootstrap (one-time, no migration)

`docs/FAMILIES_DESIGN.md` §7: this project has no migration path from the old single-household
schema to the families schema (`relay/app/bootstrap.py`'s `bootstrap_admin`, which now also
creates `families/default` and sets `{role, fam}` claims). The owner's 30 Sep call: production
holds only disposable test data, so the cutover is wipe-everything-and-re-bootstrap, not a
backfill job. **Do this exactly once**, right before deploying the families-schema build; running
it again later destroys real data with no way back.

1. **Wipe every Firestore document.** `firebase-tools` has this built in
   (verified via `npx firebase-tools firestore:delete --help`, this repo's installed version
   `15.32.0`):

   ```
   npx firebase-tools firestore:delete --all-collections --project <PROJECT_ID> --force
   ```

2. **Wipe every Firebase Auth user.** There is no delete-all here: `firebase auth:export` only
   reads accounts out to a file, `firebase auth:import` only loads accounts in (verified via
   `npx firebase-tools auth --help`, `auth:export --help`, `auth:import --help` — neither
   subcommand takes a delete/wipe flag), and this gcloud install has no `gcloud
   identity-platform` command group at all (`gcloud identity-platform --help` on this machine's
   gcloud 577.0.0 returns "Invalid choice: 'identity-platform'"; `gcloud beta identity-platform`
   fails the same way; `gcloud alpha identity-platform` exists only after installing the `alpha`
   component, which was not done here and is not verified to have the command either — don't
   guess with a real project). The command that does work is the Admin SDK's own
   `list_users`/`delete_users` (verified present and matching this signature in `relay/.venv`'s
   installed `firebase-admin==7.5.0`: `auth.list_users(page_token=None, max_results=1000,
   app=None)`, `auth.delete_users(uids, app=None)`). Run it from a machine authenticated as a
   principal with Firebase Authentication Admin on the project (`gcloud auth
   application-default login`, or the relay's own deploy service account):

   ```
   cd relay && .venv/bin/python3 -c "
   import firebase_admin
   from firebase_admin import auth
   firebase_admin.initialize_app(options={'projectId': '<PROJECT_ID>'})
   uids = [u.uid for u in auth.list_users().iterate_all()]
   for i in range(0, len(uids), 1000):
       auth.delete_users(uids[i:i + 1000])
   print(f'deleted {len(uids)} users')
   "
   ```

3. **Re-run bootstrap** (step 11's command, idempotent — safe even though the wipe already made
   it start from nothing):

   ```
   gcloud run jobs execute pager-relay-bootstrap \
     --region <REGION> --project <PROJECT_ID> \
     --args="--admin-email=<the admin's real email>"
   ```

   This creates `families/default` (name "Home", override with a second `--args="--family-name=
   ..."`), the bootstrap user as `role: super` in that family, `{role: 'super', fam: 'default'}`
   custom claims, and `settings/meta.schemaVersion = 2`.

4. **Deploy order after the wipe**: rules and indexes first (`firebase deploy --only
   firestore` — step 9's `firebase-deploy` job covers this, or run it by hand), then the relay
   (steps 7-8), then web (`firebase deploy --only hosting`) — rules must be in place before the
   relay or web can write anything into the freshly emptied database.

## 13. (Optional) Custom domain

Set `custom_domain` in `terraform.tfvars` (`docs/SERVER_PLAN.md` §10 D6), `terraform apply`,
then follow the Firebase Hosting console's DNS verification instructions (a TXT record, then an
A/AAAA or CNAME record) — Terraform cannot prove domain ownership on its own. `wait_dns_verification
= true` on the `google_firebase_hosting_custom_domain` resource means the `apply` that creates it
blocks until verification completes, so do the DNS record changes in another terminal/tab while
it's running, not after.

## 14. If EMQX Cloud Serverless doesn't pan out: the `broker-gce` fallback

Only if step 10.2's checks failed. In `terraform.tfvars`:

```
use_broker_gce    = true
broker_domain     = "broker.<your domain>"   # must already have an A record -- see below
letsencrypt_email = "you@example.com"
```

This has a genuine chicken-and-egg with DNS: the instance doesn't have an IP until after the
first `apply`, but the startup script's certbot step needs `broker_domain` to already resolve to
that IP to succeed. Sequence: `terraform apply` once (certbot fails, logged as a `WARNING` in
`/var/log/pager-broker-startup.log` on the instance, EMQX still comes up on plain MQTT) → read
the `broker_gce_external_ip` output → create the DNS A record → SSH in and rerun the certbot
line from `infra/modules/broker-gce/startup-script.sh.tpl` by hand (or just reboot the instance,
which reruns the whole startup script). See that module's `variables.tf`/`main.tf` comments for
the free-tier region constraint (`e2-micro` is only free in `us-west1`/`us-central1`/`us-east1`)
and the one genuinely non-zero cost line (the external IPv4 address, `docs/SERVER_PLAN.md`
§9.5(a): "≈ $0-4/mo").

Device credentials/ACLs have the same gap noted in step 10.6 — this module provisions the broker
process, not per-device auth.

## 15. Cold-start measurement (`docs/SERVER_PLAN.md` §9.3, §10 D10) — procedure, not yet performed

A real deployment exists now (production, §8 and §9), but no cold-start measurement is recorded in the
repo or the handoff, so this procedure has not been run.

**What to measure**: wall-clock latency of the parent→pager path (`docs/SERVER_PLAN.md`'s
`POST /api/conversations/{alias}/messages` → pager `deliver()` → broker REST publish → device
receives on `/down`) for the *first* request after the Cloud Run service has scaled to zero —
this is the number `docs/SERVER_PLAN.md` §9.3 estimates at "2-4 s" and D10 says to "measure ...
and, if the active-mode 5 s target is being missed," escalate.

**How**:
1. Confirm zero active Cloud Run instances immediately before the test — either
   `gcloud run services describe pager-relay --region <REGION> --format='value(status.traffic)'`
   right after a long idle gap, or watch the `run.googleapis.com/container/instance_count` metric
   in Cloud Monitoring hit 0. (The Scheduler `tick` job runs every 5 minutes and tends to keep an
   instance warm per `docs/SERVER_PLAN.md` §5.8's side-effect note — you may need to pause the
   `pager-tick` Scheduler job for a few minutes to reliably observe a true cold start.)
2. With `tools/pager_client.py` connected as the device (`--device-id ... connect`) and watching
   (`inbox`, or `autoack on`), send one message from the parent side
   (`tools/pager_client.py --api <relay_service_url> --as <parent alias> say <pager alias> "test"`,
   or the equivalent web app action) and record the wall-clock time from just
   before the `send`/`say` call to the device's `inbox` showing the down message.
3. Cross-check against Cloud Run's own request log for that request (Cloud Logging, filtered to
   the relay service): the log's `startTime` vs. the timestamp of the *instance* starting (a
   separate "Container instance ... started" log line) separates "cold start" (container boot +
   `firebase-admin` import, `docs/SERVER_PLAN.md` §9.3's own suggested first remedy is "a lazier
   `firebase-admin` import") from "in-flight latency" (webhook parse → Firestore transaction →
   broker publish, which happens on every request, warm or cold).
4. Repeat at least 5 times, spaced far enough apart to each be a genuine cold start (not
   back-to-back — a request right after another keeps the instance warm), and record min/median/max,
   since Cloud Run cold starts are variable.

**Where to record it**: `docs/PROTOCOL.md` §6.5 ("Latency budget check"). That section's existing
table is the **device-side** modem/eDRX budget only (sleep-mode ≈27.3s / active-mode ≈4.3s
worst case) — the relay cold start happens *before* that budget even starts (it's server-side,
upstream of the device even being paged), so add a new short paragraph or row after the existing
table stating the measured relay cold-start figure and the *combined* worst case (relay cold
start + that table's existing worst case), rather than editing the table's own numbers, which
remain correct on their own terms. If the combined figure blows D10's "accept 2-4 s" budget,
update `docs/SERVER_PLAN.md` D10's decision with the real number, per its own "measure ... revisit
only with a number" language, before spending any money on `min_instance_count = 1`.

---

## 16. Firmware OTA bucket (`docs/OTA_DESIGN.md`)

Status (7 Oct 2026): done. The bucket `kid-pager-pager-fw` is live with its index at `fw/index.json`,
and the first builds were published with `tools/fwpub.py`. The steps below are how it was set up.

1. In `envs/prod/terraform.tfvars` set `fw_publishers = ["user:<your google account>"]` (optionally `fw_bucket_name`; default `<project_id>-pager-fw`).
2. `terraform apply` (human step): creates the public-read bucket, grants you `objectAdmin`, and sets `FW_BUCKET_BASE`/`FW_INDEX_URL` on the relay. Apply after the relay code that reads them is deployed; an unrecognised env var is harmless.
3. `gcloud auth login`, then `python tools/fwpub.py probe --bucket <name>`: uploads `fw/probe-4k.bin` and prints its sha256, for bench experiment 1 (OTA_DESIGN.md §7).
4. Publish each release build: `python tools/fwpub.py publish build/images/<x>-release-app.bin --bucket <name> [--base <older.bin>]...` (needs `pip install detools`; `--dry-run --out DIR` previews without gcloud).
5. Check `terraform output fw_index_url` serves the index with `curl`.
6. Cost: about 0.56 MB per release, inside the always-free 5 GiB; no resource here is billable at this scale.

---

## Cost summary (recap of `docs/SERVER_PLAN.md` §9.3 — verify against current pricing)

Everything in this tree is designed to be **$0/month** except: (a) Secret Manager/Artifact
Registry/logging past a few active versions/pruned tags (`$0-$1`), (b) Twilio relay SMS, only when numbers are
bought (usage-based, not Terraform-managed), and (c) `broker-gce`'s external IPv4 address if that fallback
is ever turned on (`≈ $0-4/mo`, called out in that module's own comments) — everything else
(Cloud Run at `min_instance_count = 0`, Firestore/Auth/FCM/Hosting within their no-cost
allowances, Cloud Scheduler's 2 free jobs, Cloud Tasks) is designed to stay at exactly $0 at this
project's scale. Any resource in this tree that is *not* $0 has a comment on it explaining why —
search for `$` in `infra/modules/*/main.tf` if you want the full list at a glance.
