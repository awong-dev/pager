# google-beta only: every Firebase-specific resource in this module lives
# under the google-beta provider (some, like google_firestore_database, also
# exist under plain `google`, but this module standardizes on google-beta
# throughout per docs/SERVER_PLAN.md §9.1's module table, so one provider
# alias covers everything here).

# --- Firebase project -----------------------------------------------
# "Adds" Firebase to an existing GCP project. No cost by itself.
resource "google_firebase_project" "default" {
  provider = google-beta
  project  = var.project_id
}

# --- Firestore, native mode -------------------------------------------
# docs/SERVER_PLAN.md §3/§9.2. Free-tier quota (50k reads/20k writes/20k
# deletes per day, 1 GiB stored) comfortably covers §9.3's projected usage;
# see that section for the full sizing table. $0 at this scale.
resource "google_firestore_database" "default" {
  provider    = google-beta
  project     = var.project_id
  name        = "(default)" # Firebase Auth + most client SDKs assume the default database
  location_id = var.firestore_location
  type        = "FIRESTORE_NATIVE"

  # Deletion protection: a `terraform destroy` must not be able to take out
  # the only copy of message/location history. Delete via the console (or
  # relax this on purpose) if a real teardown is ever intended.
  delete_protection_state = "DELETE_PROTECTION_ENABLED"

  depends_on = [google_firebase_project.default]
}

# --- Firebase web app --------------------------------------------------
# The `web/` Next.js static export's Firebase SDK config comes from this
# resource + the data source below, output as a Terraform value so the web
# build can consume it (docs/SERVER_PLAN.md §9.1: "+ SDK config as a
# Terraform output the web app's build can consume").
resource "google_firebase_web_app" "default" {
  provider     = google-beta
  project      = var.project_id
  display_name = var.web_app_display_name

  depends_on = [google_firebase_project.default]
}

data "google_firebase_web_app_config" "default" {
  provider   = google-beta
  project    = var.project_id
  web_app_id = google_firebase_web_app.default.app_id
}

# --- Identity Platform (Firebase Auth) ----------------------------------
# docs/SERVER_PLAN.md §5.3/§11 D5: email-link (passwordless) and phone
# sign-in, both enabled by default. Free tier covers email-link entirely and
# phone within its no-cost allowance.
resource "google_identity_platform_config" "default" {
  provider = google-beta
  project  = var.project_id

  sign_in {
    email {
      enabled           = var.enable_email_link_auth
      password_required = false # passwordless email-link only, per §5.3
    }

    phone_number {
      enabled = var.enable_phone_auth
      # No test phone numbers configured here on purpose: those are a
      # per-developer console/emulator convenience (relay/docker-compose.yml
      # already covers local dev via the Auth emulator), not something this
      # production config should carry.
    }
  }

  depends_on = [google_firebase_project.default]
}

# --- Firebase Hosting ----------------------------------------------------
# Serves web/out (docs/SERVER_PLAN.md §7.1) and fronts the Cloud Run relay
# via firebase.json rewrites (web/firebase.json — not this Terraform; see
# infra/README.md). Hosting itself is free (CDN + free TLS).
resource "google_firebase_hosting_site" "default" {
  provider = google-beta
  project  = var.project_id
  site_id  = var.hosting_site_id != "" ? var.hosting_site_id : var.project_id

  depends_on = [google_firebase_project.default]
}

# Optional custom domain (docs/SERVER_PLAN.md §11 D6, default: none). Only
# created when var.custom_domain is non-empty; DNS verification and the TXT
# record are a manual runbook step either way (infra/README.md) since
# Terraform cannot prove ownership of a domain it does not control DNS for.
resource "google_firebase_hosting_custom_domain" "default" {
  provider      = google-beta
  count         = var.custom_domain != "" ? 1 : 0
  project       = var.project_id
  site_id       = google_firebase_hosting_site.default.site_id
  custom_domain = var.custom_domain

  # WAIT_FOR_CERT_UPDATE keeps `apply` from finishing before verification
  # only when there's something to wait for; REDIRECT/etc. not used.
  wait_dns_verification = true
}
