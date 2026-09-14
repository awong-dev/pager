terraform {
  required_version = ">= 1.6.0"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 8.2.0" # pin: see infra/README.md for the upgrade procedure
    }
  }

  # No backend block on purpose: this module creates the GCS bucket that
  # every other module's state will eventually live in, so it cannot itself
  # depend on that bucket existing yet. It is applied once, by hand, with
  # local state (infra/README.md runbook step 1) and never touched again
  # except to add APIs. Do not add a `backend "gcs"` block here.
}
