# Secret Manager *containers* only. Terraform never puts a secret value in
# this repo — every resource below is `google_secret_manager_secret` (the
# container), never `google_secret_manager_secret_version` with a literal
# `secret_data`. Real values are added out-of-band, per instance, with:
#
#   gcloud secrets versions add <SECRET_ID> --data-file=- --project <PROJECT>
#
# (interactively, or from a local file that is never committed — see
# infra/README.md's "secret values" runbook step). If you are ever tempted
# to add a `secret_data = "..."` argument here: don't. That would put a real
# secret in Terraform state (which is bad enough on its own) and in this
# git history (worse).
#
# Names below are chosen to match exactly what relay/app actually reads
# (relay/app/config.py, relay/app/backends/sms_stub.py) wherever that code
# already exists. Where Phase 7 (the real Twilio/gchat adapters) has not
# landed yet, the name is SERVER_PLAN.md §9.2's placeholder and is called
# out below — it may need a follow-up `terraform apply` once Phase 7 lands
# and the real env var name is known for certain.

resource "google_secret_manager_secret" "broker_api_key" {
  project   = var.project_id
  secret_id = "BROKER_API_KEY" # relay/app/config.py Settings.broker_api_key
  labels    = var.labels
  replication {
    auto {}
  }
}

resource "google_secret_manager_secret" "broker_api_secret" {
  project   = var.project_id
  secret_id = "BROKER_API_SECRET" # relay/app/config.py Settings.broker_api_secret
  labels    = var.labels
  replication {
    auto {}
  }
}

resource "google_secret_manager_secret" "webhook_key" {
  project   = var.project_id
  secret_id = "WEBHOOK_KEY" # relay/app/config.py Settings.webhook_key; broker rule engine sends this back as X-Relay-Webhook-Key
  labels    = var.labels
  replication {
    auto {}
  }
}

# --- Twilio (SMS backend, docs/SERVER_PLAN.md §6.4) --------------------
# TWILIO_ACCOUNT_SID and TWILIO_FROM_NUMBER already match the exact names
# relay/app/backends/sms_stub.py reads today (the Phase 5 stand-in). Phase 7
# lands the real adapter, which additionally needs an auth token to build
# Basic-auth-authenticated Twilio API calls and to validate inbound
# `X-Twilio-Signature` headers — that code does not exist yet, so
# TWILIO_AUTH_TOKEN's name is SERVER_PLAN.md §9.2's placeholder
# ("TWILIO_*"), not a name read anywhere in relay/app yet. Confirm it
# against Phase 7's actual config.py addition before relying on it.
resource "google_secret_manager_secret" "twilio_account_sid" {
  project   = var.project_id
  secret_id = "TWILIO_ACCOUNT_SID" # matches relay/app/backends/sms_stub.py today
  labels    = var.labels
  replication {
    auto {}
  }
}

resource "google_secret_manager_secret" "twilio_auth_token" {
  project = var.project_id
  # (unverified name) no code reads this yet — Phase 5's sms_stub.py mock
  # ignores auth entirely. Placeholder per SERVER_PLAN.md §9.2/§6.4; verify
  # against Phase 7's real relay/app/backends/sms_twilio.py before use.
  secret_id = "TWILIO_AUTH_TOKEN"
  labels    = var.labels
  replication {
    auto {}
  }
}

resource "google_secret_manager_secret" "twilio_from_number" {
  project   = var.project_id
  secret_id = "TWILIO_FROM_NUMBER" # matches relay/app/backends/sms_stub.py today
  labels    = var.labels
  replication {
    auto {}
  }
}

# --- Google Chat (gchat backend, docs/SERVER_PLAN.md §6.5) -------------
# Deliberately NOT created: no GCHAT_* secret exists in relay/app anywhere
# (no gchat.py yet — Phase 7), and per SERVER_PLAN.md §6.5 neither planned
# direction needs one — outbound uses the relay Cloud Run service account
# itself via ADC (no key material, see relay-service module's IAM roles),
# and the inbound webhook is authenticated by verifying Google's own signed
# JWT (Google's public certs, not a shared secret). If Phase 7's real
# adapter turns out to need something (e.g. a Chat app "verification
# token"), add a `google_secret_manager_secret` block here then — this
# comment is the record of why it was not pre-guessed.

resource "google_secret_manager_secret" "additional" {
  for_each = toset(var.additional_secret_ids)

  project   = var.project_id
  secret_id = each.value
  labels    = var.labels
  replication {
    auto {}
  }
}
