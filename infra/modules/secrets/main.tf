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
# Names below match exactly what relay/app actually reads
# (relay/app/config.py, relay/app/notify/sms.py).

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
# All three names are read by relay/app/notify/sms.py. TWILIO_AUTH_TOKEN is
# both the HTTP Basic auth password for outbound Messages API calls and the
# HMAC key relay/app/backends/sms_twilio.py validates inbound
# `X-Twilio-Signature` headers against.
resource "google_secret_manager_secret" "twilio_account_sid" {
  project   = var.project_id
  secret_id = "TWILIO_ACCOUNT_SID" # relay/app/notify/sms.py account_sid()
  labels    = var.labels
  replication {
    auto {}
  }
}

resource "google_secret_manager_secret" "twilio_auth_token" {
  project = var.project_id
  # relay/app/notify/sms.py auth_token(); also the HMAC key for
  # relay/app/backends/sms_twilio.py's inbound signature check.
  secret_id = "TWILIO_AUTH_TOKEN"
  labels    = var.labels
  replication {
    auto {}
  }
}

resource "google_secret_manager_secret" "twilio_from_number" {
  project   = var.project_id
  secret_id = "TWILIO_FROM_NUMBER" # relay/app/notify/sms.py from_number()
  labels    = var.labels
  replication {
    auto {}
  }
}

# --- Google Chat (gchat backend, docs/SERVER_PLAN.md §6.5) -------------
# Deliberately NOT created: relay/app/backends/gchat.py needs no secret
# material. Outbound uses the relay Cloud Run service account itself via ADC
# (see the relay-service module's IAM roles), and the inbound webhook is
# authenticated by verifying Google's own signed JWT against Google's public
# certs. GCHAT_AUDIENCE is a project number, not a secret, and is set as a
# plain env var. If a future Chat feature needs real secret material, add a
# `google_secret_manager_secret` block here then.

resource "google_secret_manager_secret" "additional" {
  for_each = toset(var.additional_secret_ids)

  project   = var.project_id
  secret_id = each.value
  labels    = var.labels
  replication {
    auto {}
  }
}
