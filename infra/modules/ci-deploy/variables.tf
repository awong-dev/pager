variable "project_id" {
  type = string
}

variable "github_org" {
  description = "GitHub org/user that owns the repo, e.g. \"awong-dev\"."
  type        = string
}

variable "github_repo" {
  description = "Repo name only (no org prefix), e.g. \"pager\"."
  type        = string
}

variable "pool_id" {
  type    = string
  default = "pager-github-pool"
}

variable "provider_id" {
  type    = string
  default = "pager-github-provider"
}

variable "deploy_service_account_id" {
  type    = string
  default = "pager-ci-deploy"
}

variable "artifact_registry_repository" {
  description = "Artifact Registry repository name (docker format) this deploy SA may push to."
  type        = string
  default     = "pager"
}

variable "region" {
  type    = string
  default = "us-central1"
}
