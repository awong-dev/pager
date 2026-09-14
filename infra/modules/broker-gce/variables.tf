# infra/modules/broker-gce -- docs/SERVER_PLAN.md §9.4/§9.5(a) OPTIONAL
# fallback. The default plan is EMQX Cloud Serverless (D2), which is NOT
# Terraform-managed at all (no provider exists for it -- it's configured by
# hand, see infra/README.md). This module only exists for the day D2's
# "UNVERIFIED" assumptions turn out false. infra/envs/prod's
# `use_broker_gce` boolean (default false) gates whether this module is
# instantiated at all.

variable "project_id" {
  type = string
}

# e2-micro's always-free-tier eligibility is region-restricted (as of this
# writing: us-west1, us-central1, us-east1 only). Keep this in one of those
# three or the "≈ $0-4/mo" cost line in SERVER_PLAN.md §9.5(a) stops being
# accurate and becomes a real e2-micro bill on top of the IP charge below.
variable "region" {
  type    = string
  default = "us-central1"
}

variable "zone" {
  type    = string
  default = "us-central1-a"
}

variable "instance_name" {
  type    = string
  default = "pager-broker"
}

variable "broker_domain" {
  description = "DNS name that resolves to this instance's external IP, used for the Let's Encrypt cert (MQTT TLS on 8883, docs/PROTOCOL.md §6.1 -- firmware pins ISRG Root X1 for any self-hosted broker, SERVER_PLAN.md §9.5's chapeau). Must already have an A record pointing here before first boot, or certbot fails and the startup script leaves EMQX on plain MQTT only (logged, not fatal, so the instance still comes up)."
  type        = string
}

variable "letsencrypt_email" {
  description = "Contact email for Let's Encrypt certificate expiry notices."
  type        = string
}

variable "relay_service_url" {
  description = "The relay Cloud Run service URL (infra/modules/relay-service's service_url output) -- where the EMQX rule engine's HTTP action forwards pager/+/{up,status,loc} (mirrors tools/emqx_setup.py's --relay-url, pointed at the real deployment instead of the compose network)."
  type        = string
}

variable "broker_api_key_secret_id" {
  description = "Secret Manager secret_id holding the BROKER_API_KEY value the relay's REST-publish credential must match (infra/modules/secrets). Fetched at boot; never written into this Terraform config."
  type        = string
}

variable "broker_api_secret_secret_id" {
  type = string
}

variable "webhook_key_secret_id" {
  description = "Secret Manager secret_id holding WEBHOOK_KEY -- the value the rule engine's HTTP action sends as X-Relay-Webhook-Key (relay/app/config.py Settings.webhook_key)."
  type        = string
}

variable "emqx_image" {
  description = "Pinned to match relay/docker-compose.yml's local dev image exactly, so the rule/connector/action shape this module's startup script provisions is the one already proven by tools/emqx_setup.py against the identical image."
  type        = string
  default     = "emqx/emqx:5.8.0"
}

variable "admin_cidr_ranges" {
  description = "CIDR ranges allowed to reach the EMQX dashboard/management API (18083). Empty by default -- the dashboard is NOT exposed publicly until an operator explicitly lists their own IP here (or an SSH-tunnel/IAP approach is used instead, see infra/README.md). MQTT itself (8883) is always public -- that's the whole point of the broker."
  type        = list(string)
  default     = []
}

variable "labels" {
  type    = map(string)
  default = { app = "pager", managed-by = "terraform" }
}
