provider "google" {
  project = var.project_id
  region  = var.region
}

# --- APIs -------------------------------------------------------------
# Enabling an API itself is free; the resources created against it are what
# cost money (or don't — see each module's cost comments). Kept as one
# for_each so infra/README.md's bootstrap step is a single `terraform apply`.
locals {
  base_apis = [
    "run.googleapis.com",             # Cloud Run v2 (relay service + jobs)
    "cloudscheduler.googleapis.com",  # tick + sweep
    "cloudtasks.googleapis.com",      # delivery retry queue
    "firestore.googleapis.com",       # the data store
    "firebase.googleapis.com",        # Firebase project/web-app/hosting resources
    "identitytoolkit.googleapis.com", # Identity Platform / Firebase Auth config
    # Identity Platform's own console settings page 400s without this
    # enabled -- found live debugging default sendOobCode (email link /
    # password reset) delivery silently failing on a fresh project.
    "cloudfunctions.googleapis.com",
    "firebaseextensions.googleapis.com", # required by some google_firebase_* resources
    "fcm.googleapis.com",                # Firebase Cloud Messaging: relay's firebase_admin.messaging sends (docs/V03_PLAN.md §3a, task 3a.3) call this API directly, separate from firebase.googleapis.com's project/hosting/web-app management surface
    "secretmanager.googleapis.com",      # secret containers
    "artifactregistry.googleapis.com",   # relay container images
    "iam.googleapis.com",
    "iamcredentials.googleapis.com", # WIF token exchange (ci-deploy module)
    "sts.googleapis.com",            # WIF token exchange
    "cloudresourcemanager.googleapis.com",
    "serviceusage.googleapis.com",
    "cloudbuild.googleapis.com", # optional: CI image builds via Cloud Build instead of local docker
    "chat.googleapis.com",       # §6.5 gchat backend — API only, no Terraform resource for the Chat app itself
  ]

  broker_gce_apis = var.enable_broker_gce_api ? ["compute.googleapis.com"] : []

  apis = toset(concat(local.base_apis, local.broker_gce_apis))
}

resource "google_project_service" "this" {
  for_each = local.apis

  project                    = var.project_id
  service                    = each.value
  disable_dependent_services = false
  disable_on_destroy         = false # never let a `destroy` here silently break the running deployment
}

# --- Terraform state bucket --------------------------------------------
# Free-tier note: GCS's always-free tier is 5 GiB regional storage in
# `us-*` multi/single regions; a Terraform state file for this project is a
# few hundred KiB even with the default 5 historical noncurrent versions
# below, so this stays at $0.
resource "google_storage_bucket" "tfstate" {
  project  = var.project_id
  name     = var.state_bucket_name
  location = var.region

  uniform_bucket_level_access = true
  public_access_prevention    = "enforced"
  force_destroy               = false # state buckets must never be casually destroyable

  versioning {
    enabled = true # state history / recovery from a bad apply, at near-zero storage cost
  }

  lifecycle_rule {
    condition {
      # 5 old versions per year is generous headroom for this project's
      # infrequent applies while still bounding storage.
      num_newer_versions = 5
    }
    action {
      type = "Delete"
    }
  }
}
