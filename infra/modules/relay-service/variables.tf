variable "project_id" {
  type = string
}

variable "region" {
  type    = string
  default = "us-central1"
}

variable "service_name" {
  type    = string
  default = "pager-relay"
}

variable "image" {
  description = "Full Artifact Registry image URI (e.g. us-central1-docker.pkg.dev/<project>/pager/relay:<tag>). CI (.github/workflows/deploy.yml) sets this per deploy; terraform.tfvars.example pins a placeholder so a bare `terraform validate` doesn't need a real image to exist."
  type        = string
}

variable "artifact_registry_repository_id" {
  description = "Docker-format Artifact Registry repository this module creates (relay images live here; .github/workflows/deploy.yml pushes CI-built images here too)."
  type        = string
  default     = "pager"
}

variable "artifact_registry_keep_count" {
  description = "docs/SERVER_PLAN.md §9.2: 'prune to the last 3 tags -- the no-cost allowance is 0.5 GB'. Implemented as a KEEP cleanup policy (most recent N versions), not a CI-side delete step, so pruning happens regardless of whether a given deploy run pushes an image."
  type        = number
  default     = 3
}

variable "broker_api_url" {
  description = "docs/SERVER_PLAN.md §9.4: EMQX Cloud Serverless REST API base URL (or the broker-gce module's, if that fallback is used). Not secret by itself -- matches relay/app/config.py's BROKER_API_URL default shape."
  type        = string
}

# --- Secret Manager wiring ------------------------------------------
# These come from infra/modules/secrets' outputs. Cloud Run v2 refuses to
# create a revision that references a secret with zero versions, so
# infra/README.md's runbook adds real values with `gcloud secrets versions
# add` *before* the first `terraform apply` of this module (or the first
# apply targets everything except this service -- see that file).
variable "broker_api_key_secret_id" {
  type = string
}

variable "broker_api_secret_secret_id" {
  type = string
}

variable "webhook_key_secret_id" {
  type = string
}

# Optional: only wired into the service's env if non-null. A deployment
# with no Twilio account still runs -- relay/app/notify/sms.py treats "not
# configured" as a soft no-op (the delivery stays queued), not a crash.
variable "twilio_account_sid_secret_id" {
  type    = string
  default = null
}

variable "twilio_auth_token_secret_id" {
  type    = string
  default = null
}

variable "twilio_from_number_secret_id" {
  type    = string
  default = null
}

variable "twilio_base_url" {
  description = "relay/app/notify/sms.py's TWILIO_BASE_URL override -- ONLY meaningful for pointing at the local Twilio mock (tools/mocks/twilio_mock.py). Leave empty in prod so the adapter talks to Twilio's own api.twilio.com; kept as a variable only so a staging deployment against the mock is possible without editing this module."
  type        = string
  default     = ""
}

variable "oidc_audience" {
  description = "app/routers/internal.py's OIDC_AUDIENCE -- must match infra/modules/schedule's oidc_token.audience exactly (both fed the same value from envs/prod, per that file's module docstring 'Known gap' section). Also set as this service's custom_audiences so a fixed, non-self-referential string (e.g. \"https://pager-relay\") can be used instead of the service's own computed .uri."
  type        = string
}

variable "oidc_allowed_emails" {
  description = "app/routers/internal.py's OIDC_ALLOWED_EMAILS -- comma-separated caller service-account emails allowed to call /internal/*. Today just infra/modules/schedule's pager-scheduler SA; computed deterministically in envs/prod (account_id is the literal \"pager-scheduler\") to avoid a module cycle."
  type        = string
}

variable "tasks_mode" {
  description = "relay/app/tasks.py's TASKS_MODE. Only \"inline\" is usable end-to-end today; \"cloud_tasks\" builds tasks but has nowhere to dispatch them yet (see that module's docstring's known-gap section). The variable exists so the eventual flip is a tfvars change, not a module edit."
  type        = string
  default     = "inline"
  validation {
    condition     = var.tasks_mode == "inline"
    error_message = "relay/app/tasks.py only supports TASKS_MODE=inline end-to-end; see that module's docstring."
  }
}

variable "sweep_batch" {
  description = "Override for app/jobs.py's SWEEP_BATCH (default 500 if left null -- see relay/.env.example)."
  type        = number
  default     = null
}

variable "loc_req_ttl_s" {
  description = "Override for app/location.py's LOC_REQ_TTL_S (default 900 if left null). Production should leave this null -- it exists only so tests/tools/e2e_v2.py can shorten it."
  type        = number
  default     = null
}

variable "min_instance_count" {
  description = "docs/SERVER_PLAN.md §9.2/D10 and the hard rule in this repo's infra brief: must stay 0. The always-on-instance trade-off is D10's call, not infra's -- do not raise this without a SERVER_PLAN.md update."
  type        = number
  default     = 0
  validation {
    condition     = var.min_instance_count == 0
    error_message = "min_instance_count must stay 0 -- see docs/SERVER_PLAN.md D10 and this module's comment. That decision is out of scope for infra to change unilaterally."
  }
}

variable "max_instance_count" {
  type    = number
  default = 2
}

variable "labels" {
  type    = map(string)
  default = { app = "pager", managed-by = "terraform" }
}
