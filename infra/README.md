# infra/ — deployment runbook

This is the **only** place real cloud resources get touched for this project, and only by a
human running these steps by hand (or by CI, once step 9 below has been done). Nothing in
`infra/` was ever `terraform apply`'d or `terraform plan`'d against a real backend while this
tree was built — every module only ever saw `terraform init -backend=false` +
`terraform validate`. Read `docs/SERVER_PLAN.md` §9 first; this file is the "how", that section
is the "what and why".

**Never commit**: `terraform.tfvars` (only `terraform.tfvars.example` is tracked), any
`.tfstate` file, any real secret value, or a service-account JSON key (none should ever be
created — this whole layout is Workload Identity Federation, on purpose).

---

## 0. Prerequisites

- `gcloud` CLI, authenticated as a human with Owner (or Project Creator + Billing Account User)
  on whatever GCP org/folder the project will live in.
- `terraform` >= 1.14 (this tree was built and validated against 1.14.9), `docker`, `node` 20+
  (for `web/`, once Phase 6 lands), `firebase-tools` (`npx firebase-tools`, no global install
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
`BROKER_API_KEY`/`BROKER_API_SECRET` must be the **exact same pair** you configure as EMQX
Cloud's REST API key in step 10 below — do steps 6 and 10 together, in whichever order is
convenient, but make sure the values end up identical on both sides. Generate a `WEBHOOK_KEY`
yourself (e.g. `openssl rand -hex 32`) — it just needs to match what you configure into the EMQX
rule engine's HTTP action header in step 10.

`TWILIO_ACCOUNT_SID`/`TWILIO_AUTH_TOKEN`/`TWILIO_FROM_NUMBER` are **not** needed yet — Phase 7
hasn't landed a real Twilio adapter (see this module's own `secrets/main.tf` comment on why
`TWILIO_AUTH_TOKEN`'s name is an unverified placeholder). Leave `enable_sms_secrets = false` in
`terraform.tfvars` until Phase 7 lands and you've added real values for those three.

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

This creates the Cloud Run service + the `bootstrap`/`import_sqlite` jobs + their service
account and IAM roles, the Cloud Scheduler `tick`/`sweep` jobs + their OIDC caller identity, and
the Cloud Tasks queue. Check the `relay_service_url` output — `GET <that URL>/healthz` should
now return 200 (once a working `BROKER_API_URL` is in `terraform.tfvars`; a placeholder URL
there just means `/healthz`'s broker-reachability check fails, not that the service is down).

**Known gap, expected at this point**: `/internal/tick` and `/internal/sweep` will 404. That is
not a misconfiguration — `relay/app/routers/internal.py` gates both routes on `DEV_MODE`, which
this deployment never sets, and OIDC verification for them is explicitly **Phase 8** work not
yet written (see `infra/modules/schedule/variables.tf`'s module docstring). The Scheduler jobs
this module created will therefore "succeed" at calling a route that 404s until Phase 8 adds the
missing verification code to `relay/app/routers/internal.py` — track that as a real follow-up,
not something this Terraform can fix on its own.

## 9. Wire up GitHub Actions (turns `.github/workflows/deploy.yml` from a no-op into a real pipeline)

Read that workflow's own top-of-file comment for the exact gating mechanism first. Then:

- Repo **secrets** (Settings → Secrets and variables → Actions → Secrets):
  - `GCP_WORKLOAD_IDENTITY_PROVIDER` = `terraform output -raw ci_deploy_workload_identity_provider` (from `infra/envs/prod`)
  - `GCP_DEPLOY_SERVICE_ACCOUNT` = `terraform output -raw ci_deploy_service_account_email`
- Repo **variables** (same page, "Variables" tab):
  - `GCP_PROJECT_ID`, `GCP_REGION`, `BROKER_API_URL`, `IMPORT_DATA_BUCKET_NAME` (same values as
    `terraform.tfvars`)

Once both secrets exist, a push to `main` (or a manual `workflow_dispatch` run) builds the relay
image, pushes it, runs `terraform apply` via WIF, and runs `firebase deploy --only
hosting,firestore`. The `firebase-deploy` job additionally needs `web/` to have a working
`npm run build` — **not true yet as of this commit** (Phase 6, `web/`, is running in parallel
with this phase and had not landed when this runbook was written); until it does, the
`firebase-deploy` job will fail loudly (deliberately not swallowed) even once secrets exist. That
is an expected, temporary gap, not a bug in this workflow.

Also decide where `firebase.json` lives for `firebase deploy --only hosting,firestore` to work:
today only `relay/firebase.json` exists (emulator config + `firestore.rules`/`firestore.indexes.json`
paths, used for local dev). Phase 6's `web/firebase.json` (per `docs/SERVER_PLAN.md` §7.1) needs
to add the Hosting `public`/rewrite config and should keep pointing at the same
`relay/firestore.rules`/`relay/firestore.indexes.json` (relative path) rather than duplicating
them — confirm this when Phase 6 lands; not this phase's file to write.

## 10. EMQX Cloud Serverless console setup (the `PENDING_ACCOUNT` from `BUILD_LOG.md` Phase 2a)

No Terraform provider exists for EMQX Cloud (`docs/SERVER_PLAN.md` §9.4) — this is all
console/API work, by hand:

1. Sign up at [emqx.com/cloud](https://www.emqx.com/en/cloud) (Serverless tier, free, no card
   required as of `BUILD_LOG.md` Phase 2a's note — reverify).
2. Create a Serverless deployment. Verify the three things Phase 2a flagged `UNVERIFIED`
   (`SERVER_PLAN.md` D2), in order, and **stop and use `infra/modules/broker-gce` instead if any
   fail**:
   - Rule engine supports an HTTP action on this tier.
   - The REST publish API (`/api/v5/publish`) is available on this tier.
   - The HTTP action's timeout can be set **≥ 15 s** (a cold Cloud Run relay can take 2-4 s to
     even start responding — `relay/emqx/`'s local dev config uses a 10 s `request_ttl`, which
     the BUILD_LOG note already flags as too short for production).
3. Replicate the exact rule/connector/action shape `tools/emqx_setup.py` creates locally
   (`CONNECTOR_NAME = relay_webhook`, `ACTION_NAME = relay_webhook_action`, `RULE_ID =
   pager_to_relay`, topics `pager/+/up`, `pager/+/status`, `pager/+/loc`), pointed at this
   deployment's real `<relay_service_url>/webhooks/mqtt` instead of the compose network address,
   with the `X-Relay-Webhook-Key` header set to the same value you put in `WEBHOOK_KEY` (step 6).
   `infra/modules/broker-gce/startup-script.sh.tpl` is a worked example of this same shape
   re-implemented in curl, in case the EMQX Cloud console makes it easier to look at a script
   than reconstruct it purely from the dashboard.
4. Generate a REST API key/secret pair for the relay's own publishes; put that exact pair into
   `BROKER_API_KEY`/`BROKER_API_SECRET` (step 6) and put the deployment's REST API base URL into
   `terraform.tfvars`'s `broker_api_url`, then `terraform apply` again to update the Cloud Run
   service's env.
5. Record whatever you found in step 2 back into `docs/SERVER_PLAN.md` §9.4 as a
   `(build finding — …)` note, per `BUILD_LOG.md` Phase 2a's instruction — this file doesn't do
   that for you.
6. **Device credentials and ACLs are still not provisioned anywhere** — this is the gap
   `BUILD_LOG.md` Phase 3 flagged as `(deploy blocker, tracked for Phase 9)`: `POST
   /api/admin/devices` mints and returns an MQTT credential, but nothing pushes it (or
   `PROTOCOL.md` §2's three ACL rules) into the broker. If EMQX Cloud Serverless's console/API
   exposes credential+ACL management, either use it by hand per device for now, or treat wiring
   the admin API to push automatically as real Phase 8+ backend work (`SERVER_PLAN.md` §5.5
   already describes the intended shape: "the relay pushes the device credential ... otherwise
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

## 12. (Optional, once) Import the MVP SQLite database

```
gsutil cp relay.db gs://<import_data_bucket_name>/relay.db
gcloud run jobs execute pager-relay-import-sqlite --region <REGION> --project <PROJECT_ID>
```

The bucket auto-deletes objects older than 30 days (`infra/modules/relay-service/main.tf`) — this
is meant to run once, shortly after the bucket is created, not as a standing pipeline.

## 13. (Optional) Custom domain

Set `custom_domain` in `terraform.tfvars` (`docs/SERVER_PLAN.md` §11 D6), `terraform apply`,
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

## 15. Cold-start measurement (`docs/SERVER_PLAN.md` §9.3, D10) — procedure only, not performed here

There is no real deployment yet as this phase was built (`HARD RULE`: no `apply` was run), so
this is written for a human to follow **after** a real deployment exists, not executed now.

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
   or the equivalent web app action once Phase 6 lands) and record the wall-clock time from just
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

## Cost summary (recap of `docs/SERVER_PLAN.md` §9.3 — verify against current pricing)

Everything in this tree is designed to be **$0/month** except: (a) Secret Manager/Artifact
Registry/logging past a few active versions/pruned tags (`$0-$1`), (b) Twilio, only if the SMS
backend is enabled (`~$1 + usage`), and (c) `broker-gce`'s external IPv4 address if that fallback
is ever turned on (`≈ $0-4/mo`, called out in that module's own comments) — everything else
(Cloud Run at `min_instance_count = 0`, Firestore/Auth/FCM/Hosting within their no-cost
allowances, Cloud Scheduler's 2 free jobs, Cloud Tasks) is designed to stay at exactly $0 at this
project's scale. Any resource in this tree that is *not* $0 has a comment on it explaining why —
search for `$` in `infra/modules/*/main.tf` if you want the full list at a glance.
