data "google_project" "this" {
  project_id = var.project_id
}

# --- Artifact Registry --------------------------------------------------
# docs/SERVER_PLAN.md §9.2: "pushes to Artifact Registry (prune to the last
# 3 tags -- the no-cost allowance is 0.5 GB)". A KEEP cleanup policy (most
# recent N versions) is enforced by Artifact Registry itself on a schedule,
# not by a CI step, so pruning happens even if a deploy is skipped for a
# while. Storage within the 0.5 GB free allowance is $0; a relay image
# (python:3.12-slim base + a small FastAPI app) is tens of MB, so 3 tags
# stays comfortably inside it.
resource "google_artifact_registry_repository" "relay" {
  project       = var.project_id
  location      = var.region
  repository_id = var.artifact_registry_repository_id
  format        = "DOCKER"
  description   = "Pager relay container images (docs/SERVER_PLAN.md §9.2)"

  cleanup_policies {
    id     = "keep-last-${var.artifact_registry_keep_count}"
    action = "KEEP"
    most_recent_versions {
      keep_count = var.artifact_registry_keep_count
    }
  }
}

# --- Service account (least privilege, not project-editor) --------------
resource "google_service_account" "relay" {
  project      = var.project_id
  account_id   = "${var.service_name}-sa"
  display_name = "Pager relay Cloud Run runtime (Firestore + FCM + Secret accessor)"
}

locals {
  relay_roles = [
    "roles/datastore.user",               # Firestore read/write -- the relay is the only writer (docs/SERVER_PLAN.md §2 decision 3)
    "roles/firebaseauth.admin",           # verify + mint custom tokens, create users, set the `admin` custom claim (§5.3, §5.5)
    "roles/firebasecloudmessaging.admin", # webapp backend's FCM sends (§6.3)
    "roles/secretmanager.secretAccessor", # read the secret *values* this module wires in as env vars below
    "roles/cloudtasks.enqueuer",          # forward-looking: app/tasks.py's CloudTasksQueue (TASKS_MODE=cloud_tasks) needs this; harmless no-op today since TASKS_MODE stays "inline"
    "roles/logging.logWriter",            # a dedicated (non-default) service account needs this explicitly to write Cloud Logging entries
    "roles/monitoring.metricWriter",
  ]
}

resource "google_project_iam_member" "relay" {
  for_each = toset(local.relay_roles)

  project = var.project_id
  role    = each.value
  member  = "serviceAccount:${google_service_account.relay.email}"
}

# --- Secret bindings -----------------------------------------------------
# Terraform does NOT create secret values (infra/modules/secrets does the
# containers; infra/README.md's runbook adds values by hand). This module
# only wires env vars that pull from those containers' *latest* version at
# Cloud Run revision creation time -- which means the referenced secret
# must already have at least one version, or the revision fails to create.
locals {
  # name -> secret_id, only for secrets that are actually configured.
  optional_secret_envs = {
    for k, v in {
      TWILIO_ACCOUNT_SID = var.twilio_account_sid_secret_id
      TWILIO_AUTH_TOKEN  = var.twilio_auth_token_secret_id
      TWILIO_FROM_NUMBER = var.twilio_from_number_secret_id
    } : k => v if v != null
  }
}

# --- Cloud Run v2 service --------------------------------------------
resource "google_cloud_run_v2_service" "relay" {
  project  = var.project_id
  name     = var.service_name
  location = var.region

  # Public: Hosting rewrites (/api/**, /webhooks/**) and the broker's own
  # webhook POST both need to reach this unauthenticated at the Cloud Run
  # layer. /webhooks/mqtt is authenticated by the shared WEBHOOK_KEY header
  # (app-level, not IAM); /internal/* is meant to be OIDC-checked at the
  # app level (see infra/modules/schedule's module docstring).
  ingress = "INGRESS_TRAFFIC_ALL"

  template {
    service_account = google_service_account.relay.email

    # docs/SERVER_PLAN.md §9.2 / this repo's hard rule: min 0, max 2.
    scaling {
      min_instance_count = var.min_instance_count
      max_instance_count = var.max_instance_count
    }

    max_instance_request_concurrency = 20
    timeout                          = "300s" # the sweep is resumable (§5.7); this just bounds one HTTP request

    containers {
      image = var.image

      ports {
        container_port = 8000
      }

      resources {
        # request-based billing: CPU is only allocated while a request is
        # in flight (cpu_idle = true is the default for gen2/CPU-throttled
        # Cloud Run and is what keeps this at $0 -- see §9.3). Explicit here
        # so nobody "fixes" it to always-on CPU by accident.
        cpu_idle          = true
        startup_cpu_boost = true
        limits = {
          cpu    = "1"
          memory = "512Mi"
        }
      }

      env {
        name  = "GOOGLE_CLOUD_PROJECT"
        value = var.project_id
      }
      env {
        name  = "BROKER_API_URL"
        value = var.broker_api_url
      }
      env {
        name  = "TASKS_MODE"
        value = var.tasks_mode
      }
      env {
        name  = "TWILIO_BASE_URL"
        value = var.twilio_base_url
      }

      dynamic "env" {
        for_each = var.sweep_batch == null ? [] : [var.sweep_batch]
        content {
          name  = "SWEEP_BATCH"
          value = tostring(env.value)
        }
      }

      dynamic "env" {
        for_each = var.loc_req_ttl_s == null ? [] : [var.loc_req_ttl_s]
        content {
          name  = "LOC_REQ_TTL_S"
          value = tostring(env.value)
        }
      }

      # DEV_MODE is intentionally never set here (absent -> Settings.dev_mode
      # defaults False, relay/app/config.py). It is not exposed as a
      # variable on purpose -- a real deployment must never be able to
      # accidentally set DEV_MODE=1 via a tfvars typo, since that both opens
      # POST /api/dev/token (a token-minting bypass) and is what currently
      # gates /internal/tick and /internal/sweep open (see
      # infra/modules/schedule's docstring on the OIDC wiring gap).

      env {
        name = "BROKER_API_KEY"
        value_source {
          secret_key_ref {
            secret  = var.broker_api_key_secret_id
            version = "latest"
          }
        }
      }
      env {
        name = "BROKER_API_SECRET"
        value_source {
          secret_key_ref {
            secret  = var.broker_api_secret_secret_id
            version = "latest"
          }
        }
      }
      env {
        name = "WEBHOOK_KEY"
        value_source {
          secret_key_ref {
            secret  = var.webhook_key_secret_id
            version = "latest"
          }
        }
      }

      dynamic "env" {
        for_each = local.optional_secret_envs
        content {
          name = env.key
          value_source {
            secret_key_ref {
              secret  = env.value
              version = "latest"
            }
          }
        }
      }
    }
  }

  labels = var.labels

  lifecycle {
    ignore_changes = [
      # CI (.github/workflows/deploy.yml) deploys new images by
      # `terraform apply -var image=...`; nothing else should fight it, but
      # this guards against the annotations Cloud Run itself stamps onto
      # the resource out of band (traffic percentages / revision names)
      # showing up as spurious diffs.
      template[0].annotations,
    ]
  }
}

# Public, unauthenticated invocation at the Cloud Run IAM layer (separate
# from `ingress` above -- ingress controls network path, this controls
# who's allowed to call it once traffic arrives). Required for Hosting
# rewrites and the broker webhook, both of which call over plain HTTPS with
# no Google identity. Free.
resource "google_cloud_run_v2_service_iam_member" "public_invoker" {
  project  = var.project_id
  location = var.region
  name     = google_cloud_run_v2_service.relay.name
  role     = "roles/run.invoker"
  member   = "allUsers"
}

# --- One-off jobs (run by hand, never on every deploy) -------------------

# `python -m app.bootstrap --admin-email ...` (docs/SERVER_PLAN.md §5.3).
# --admin-email is passed at *execution* time (`gcloud run jobs execute
# ... --args=--admin-email=...`), not baked in here, so no admin's email
# address ends up in Terraform state or this repo. See infra/README.md.
resource "google_cloud_run_v2_job" "bootstrap" {
  project  = var.project_id
  name     = "${var.service_name}-bootstrap"
  location = var.region

  template {
    template {
      service_account = google_service_account.relay.email
      max_retries     = 0
      containers {
        image   = var.image
        command = ["python", "-m", "app.bootstrap"]
        # No default --admin-email argument on purpose -- see comment above.
        # `gcloud run jobs execute pager-relay-bootstrap --args=--admin-email=<addr> --project ...`
        env {
          name  = "GOOGLE_CLOUD_PROJECT"
          value = var.project_id
        }
      }
    }
  }

  labels = var.labels
}
