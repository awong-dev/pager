# infra/modules/schedule -- docs/SERVER_PLAN.md §9.1/§9.2:
# "Cloud Scheduler jobs (tick, sweep) with OIDC to the relay; Cloud Tasks queue"
#
# IMPORTANT, read before wiring this up:
# relay/app/routers/internal.py DOES verify OIDC tokens -- it checks
# signature, `aud` == the relay's `OIDC_AUDIENCE` env var, and the caller's
# `email` against `OIDC_ALLOWED_EMAILS`. A missing/invalid token is a 401
# (no longer the pre-Phase-8 404).
#
# REMAINING GAP (Terraform side, not app side): infra/modules/relay-service
# does not set either env var on the Cloud Run service, so /internal/tick and
# /internal/sweep fail closed with 401 on every Scheduler invocation until
# they are. Wiring them needs BOTH of:
#
#   1. `OIDC_ALLOWED_EMAILS` = this module's scheduler SA email. There is no
#      real module cycle here: `account_id` below is the literal
#      "pager-scheduler", so the email is deterministic and can be computed
#      in infra/envs/prod as
#      "pager-scheduler@${var.project_id}.iam.gserviceaccount.com" and passed
#      into relay-service without referencing this module at all (cleanest:
#      move google_service_account.scheduler up into envs/prod and pass the
#      email down into both modules).
#   2. `OIDC_AUDIENCE`. Referencing the service's own computed `.uri` from
#      inside its own `env` block IS a genuine self-reference, but the
#      audience does not have to be the run.app URL: set a
#      `custom_audiences` value on google_cloud_run_v2_service (e.g.
#      "https://pager-relay") and use that same string for both the
#      service's OIDC_AUDIENCE env var and `oidc_token.audience` below,
#      sourced from one shared variable. That resolves in a single apply --
#      no two-phase bootstrap needed.
#
# Do not assume the scheduled jobs "work" end to end until both are done.

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
