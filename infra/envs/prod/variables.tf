variable "project_id" {
  description = "GCP project id (already created by hand, Blaze billing enabled -- infra/README.md step 0)."
  type        = string
}

variable "region" {
  type    = string
  default = "us-central1"
}

variable "firestore_location" {
  type    = string
  default = "nam5"
}

variable "web_app_display_name" {
  type    = string
  default = "pager-web"
}

variable "hosting_site_id" {
  type    = string
  default = ""
}

variable "custom_domain" {
  description = "docs/SERVER_PLAN.md §11 D6. Empty = none (the *.web.app URL is used everywhere)."
  type        = string
  default     = ""
}

variable "enable_phone_auth" {
  type    = bool
  default = true
}

variable "enable_email_link_auth" {
  type    = bool
  default = true
}

variable "relay_service_name" {
  type    = string
  default = "pager-relay"
}

variable "relay_image" {
  description = "Full Artifact Registry image URI. CI (.github/workflows/deploy.yml) overrides this per deploy with `-var relay_image=...`."
  type        = string
}

variable "broker_api_url" {
  description = "docs/SERVER_PLAN.md §9.4: EMQX Cloud Serverless REST API base URL for this deployment (from the EMQX Cloud console, infra/README.md), or the broker-gce module's own REST endpoint if use_broker_gce=true."
  type        = string
}

variable "broker_host" {
  description = "The broker's MQTT(S) hostname (not the REST API URL) -- baked into every device's setup-code bootstrap bundle. For EMQX Cloud Serverless this is normally the same base hostname as broker_api_url, just used for MQTT(S) instead of the REST management API."
  type        = string
}

variable "twilio_base_url" {
  description = "Leave empty in a real deployment -- see infra/modules/relay-service/variables.tf's comment. Only meaningful pointed at a Twilio-mock-shaped staging endpoint."
  type        = string
  default     = ""
}

variable "enable_sms_secrets" {
  description = "Wire TWILIO_* secret-sourced env vars into the relay service. Leave false until infra/README.md's secret-value step has been done for the three Twilio secrets -- Cloud Run refuses to create a revision that references a secret with zero versions (see relay-service/main.tf's comment)."
  type        = bool
  default     = false
}

variable "tick_schedule" {
  type    = string
  default = "*/5 * * * *"
}

variable "sweep_schedule" {
  type    = string
  default = "0 3 * * 0"
}

variable "time_zone" {
  description = "docs/SERVER_PLAN.md §5.7/D7: the sweep's \"Sunday 03:00\" is local to this timezone."
  type        = string
  default     = "America/Los_Angeles"
}

variable "github_org" {
  type = string
}

variable "github_repo" {
  type    = string
  default = "pager"
}

# --- broker-gce fallback (docs/SERVER_PLAN.md §9.4/§9.5(a)) --------------
# Default false: EMQX Cloud Serverless is the primary plan (D2). Only flip
# this once D2's assumptions are verified false for the real account.
variable "use_broker_gce" {
  type    = bool
  default = false
}

variable "broker_domain" {
  description = "Required only when use_broker_gce=true."
  type        = string
  default     = ""
}

variable "letsencrypt_email" {
  description = "Required only when use_broker_gce=true."
  type        = string
  default     = ""
}

variable "broker_admin_cidr_ranges" {
  description = "CIDR ranges allowed to reach the broker-gce EMQX dashboard. Empty = dashboard not exposed. Only relevant when use_broker_gce=true."
  type        = list(string)
  default     = []
}

variable "labels" {
  type    = map(string)
  default = { app = "pager", managed-by = "terraform" }
}
