terraform {
  required_version = ">= 1.6.0"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 8.2.0"
    }
    google-beta = {
      source  = "hashicorp/google-beta"
      version = "~> 8.2.0"
    }
  }

  # Points at the bucket infra/bootstrap created (its `state_bucket_name`
  # output). Left as a placeholder here on purpose -- filling in a real
  # bucket name is infra/README.md's runbook step 2, done with
  # `terraform init -backend-config=...` or by editing this block by hand
  # once the bucket exists. `terraform validate -backend=false` (this
  # module's CI/dev-loop check) never touches this block at all.
  backend "gcs" {
    bucket = "kid-pager-tfstate"
    prefix = "envs/prod"
  }
}
