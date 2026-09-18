# Workload Identity Federation for GitHub Actions -- lets
# .github/workflows/deploy.yml authenticate as a real GCP service account
# without a long-lived JSON key ever existing (none is created by this
# module, and none should ever be added to GitHub secrets either).

resource "google_iam_workload_identity_pool" "github" {
  project                   = var.project_id
  workload_identity_pool_id = var.pool_id
  display_name              = "GitHub Actions (pager)"
  description               = "Federates GitHub Actions OIDC tokens for ${var.github_org}/${var.github_repo} only -- see attribute_condition below."
}

resource "google_iam_workload_identity_pool_provider" "github" {
  project                            = var.project_id
  workload_identity_pool_id          = google_iam_workload_identity_pool.github.workload_identity_pool_id
  workload_identity_pool_provider_id = var.provider_id
  display_name                       = "GitHub OIDC"

  attribute_mapping = {
    "google.subject"       = "assertion.sub"
    "attribute.repository" = "assertion.repository"
    "attribute.ref"        = "assertion.ref"
  }

  # Scoped to this repo only, per the brief ("scoped to this repo only").
  # Belt-and-suspenders with the IAM binding below, which also scopes to
  # the same repository via the principalSet condition.
  attribute_condition = "assertion.repository == \"${var.github_org}/${var.github_repo}\""

  oidc {
    issuer_uri = "https://token.actions.githubusercontent.com"
  }
}

resource "google_service_account" "deploy" {
  project      = var.project_id
  account_id   = var.deploy_service_account_id
  display_name = "GitHub Actions deploy identity (.github/workflows/deploy.yml)"
}

# Only principals whose GitHub `repository` claim matches this exact repo
# may impersonate the deploy SA -- not "any workload in this WIF pool".
resource "google_service_account_iam_member" "wif_binding" {
  service_account_id = google_service_account.deploy.name
  role               = "roles/iam.workloadIdentityUser"
  member             = "principalSet://iam.googleapis.com/${google_iam_workload_identity_pool.github.name}/attribute.repository/${var.github_org}/${var.github_repo}"
}

# --- Deploy SA project roles ---------------------------------------------
# Broader than the relay runtime SA (infra/modules/relay-service) on
# purpose -- this identity runs `terraform apply` for infra/envs/prod
# (Cloud Run, Scheduler, Tasks, Secret Manager *containers*, Firestore/
# Firebase config, IAM bindings) and `firebase deploy --only
# hosting,firestore`. Still enumerated by named admin roles rather than
# `roles/editor`/`roles/owner`, so it cannot touch billing, unrelated
# projects, or things like Compute (unless var.enable_broker_gce_api's
# bootstrap API was also enabled, at which point add roles/compute.admin
# separately -- not included by default here).
locals {
  deploy_roles = [
    "roles/run.admin",                  # deploy Cloud Run services/jobs
    "roles/iam.serviceAccountUser",     # deploy actions need to act as the relay runtime SA (infra/modules/relay-service)
    "roles/artifactregistry.writer",    # push + prune image tags (SERVER_PLAN.md §9.2)
    "roles/artifactregistry.repoAdmin", # manage the repository resource itself (retention/pruning policy) via Terraform
    "roles/secretmanager.admin",        # create/manage secret *containers* (never versions with real values from CI)
    "roles/datastore.owner",            # google_firestore_database + `firebase deploy --only firestore` (rules/indexes)
    "roles/firebase.admin",             # google_firebase_* resources (project, web app, hosting, custom domain) + `firebase deploy --only hosting`
    "roles/cloudscheduler.admin",       # infra/modules/schedule
    "roles/cloudtasks.admin",           # infra/modules/schedule
    "roles/iam.securityAdmin",          # grant the relay runtime SA + scheduler SA their project IAM roles via Terraform
    "roles/serviceusage.serviceUsageConsumer",
    # Self-referential bootstrap gap, found live: this module's own
    # `terraform apply` (run by this exact SA in CI) manages the workload
    # identity pool/provider this SA authenticates through -- without this
    # role, CI's own apply 403s reading its own pool
    # (iam.workloadIdentityPools.get). roles/iam.securityAdmin does not
    # cover Workload Identity Federation resources.
    "roles/iam.workloadIdentityPoolAdmin",
  ]
}

resource "google_project_iam_member" "deploy" {
  for_each = toset(local.deploy_roles)

  project = var.project_id
  role    = each.value
  member  = "serviceAccount:${google_service_account.deploy.email}"
}
