# infra/bootstrap — one-off, applied by hand per infra/README.md.
#
# This module does two things and nothing else:
#   1. Enables the GCP APIs every other module needs.
#   2. Creates the GCS bucket that infra/envs/prod's `backend "gcs"` block
#      then points at.
# Both are prerequisites for envs/prod to `terraform init` against a real
# backend, which is exactly why they cannot live in envs/prod themselves
# (chicken-and-egg: the backend bucket can't be provisioned by the
# configuration that depends on the backend already existing).

variable "project_id" {
  description = "The GCP project id. Must already exist (create it in the console or `gcloud projects create` by hand first — infra/README.md step 0)."
  type        = string
}

variable "region" {
  description = "Default region for the state bucket and any regional APIs."
  type        = string
  default     = "us-central1"
}

variable "state_bucket_name" {
  description = "Globally-unique GCS bucket name for Terraform remote state (e.g. \"<project_id>-tfstate\")."
  type        = string
}

variable "enable_broker_gce_api" {
  description = "Also enable compute.googleapis.com. Only needed if infra/modules/broker-gce (the EMQX-on-e2-micro fallback) will ever be used. Leave false for the default EMQX Cloud Serverless path (docs/SERVER_PLAN.md §9.4/§9.5)."
  type        = bool
  default     = false
}
