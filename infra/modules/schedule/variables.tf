# infra/modules/schedule -- docs/SERVER_PLAN.md §9.1/§9.2:
# "Cloud Scheduler jobs (tick, sweep) with OIDC to the relay; Cloud Tasks queue"
#
# IMPORTANT, read before wiring this up: relay/app/routers/internal.py does
# NOT verify OIDC tokens yet. `/internal/tick` and `/internal/sweep` are
# currently gated on `Settings.dev_mode` only (404 when DEV_MODE != "1",
# which infra/modules/relay-service never sets) -- see that router's module
# docstring: "No OIDC verification yet -- that is explicitly Phase 8
# hardening". This module provisions the *caller identity* (a dedicated
# service account, OIDC audience = the relay's URL) that Phase 8's
# verification code will check against once it exists. Until Phase 8 lands,
# these Scheduler jobs will call a route that 404s in a real deployment
# (DEV_MODE unset) -- that is a known, expected gap, not a bug in this
# module. Re-read this comment before assuming the scheduled jobs "work"
# end to end.

variable "project_id" {
  type = string
}

variable "region" {
  type    = string
  default = "us-central1"
}

variable "relay_service_url" {
  description = "infra/modules/relay-service's service_url output."
  type        = string
}

variable "relay_service_name" {
  description = "infra/modules/relay-service's service_name output (used for the run.invoker IAM binding)."
  type        = string
}

variable "tick_schedule" {
  description = "docs/SERVER_PLAN.md §5.8: every 5 minutes."
  type        = string
  default     = "*/5 * * * *"
}

variable "sweep_schedule" {
  description = "docs/SERVER_PLAN.md §5.7/§11 D7: weekly, Sunday 03:00 local."
  type        = string
  default     = "0 3 * * 0"
}

variable "time_zone" {
  description = "docs/SERVER_PLAN.md §5.7's TZ -- the sweep's \"Sunday 03:00 local\" is local to this value, not UTC. Set to the family's actual timezone."
  type        = string
  default     = "America/Los_Angeles"
}

variable "task_queue_name" {
  type    = string
  default = "pager-delivery-retries"
}

variable "labels" {
  type    = map(string)
  default = { app = "pager", managed-by = "terraform" }
}
