# Dedicated identity for Cloud Scheduler's OIDC-authenticated calls into the
# relay's /internal/* routes. See variables.tf's module docstring: Phase 8
# is what makes the relay actually check this identity; this module only
# provisions it.
resource "google_service_account" "scheduler" {
  project      = var.project_id
  account_id   = "pager-scheduler"
  display_name = "Cloud Scheduler caller identity for pager relay /internal/*"
}

# Lets the scheduler SA invoke the (already-public) relay service. Not
# strictly required for the call to succeed today (the service also grants
# allUsers roles/run.invoker in relay-service/main.tf, since Hosting
# rewrites and the broker webhook need unauthenticated access at the Cloud
# Run layer), but this is the binding Phase 8's app-level OIDC check will
# rely on having a real, auditable identity behind the token it verifies --
# keep it even though it's currently redundant with the public binding.
resource "google_cloud_run_v2_service_iam_member" "scheduler_invoker" {
  project  = var.project_id
  location = var.region
  name     = var.relay_service_name
  role     = "roles/run.invoker"
  member   = "serviceAccount:${google_service_account.scheduler.email}"
}

# docs/SERVER_PLAN.md §5.8, §9.2: every 5 minutes -> /internal/tick.
resource "google_cloud_scheduler_job" "tick" {
  project   = var.project_id
  region    = var.region
  name      = "pager-tick"
  schedule  = var.tick_schedule
  time_zone = var.time_zone

  # jobs.tick()'s own retry (per-delivery, inside the request) is separate
  # from Scheduler's own retry-on-failed-invocation, which stays at
  # Scheduler's defaults -- a missed tick is caught by the next one 5
  # minutes later, so no special retry_config is needed here.

  http_target {
    uri         = "${var.relay_service_url}/internal/tick"
    http_method = "POST"

    oidc_token {
      service_account_email = google_service_account.scheduler.email
      audience              = var.relay_service_url
    }
  }

  depends_on = [google_cloud_run_v2_service_iam_member.scheduler_invoker]
}

# docs/SERVER_PLAN.md §5.7/§9.2/§11 D7: weekly (Sunday 03:00 in var.time_zone) -> /internal/sweep.
resource "google_cloud_scheduler_job" "sweep" {
  project   = var.project_id
  region    = var.region
  name      = "pager-sweep"
  schedule  = var.sweep_schedule
  time_zone = var.time_zone

  http_target {
    uri         = "${var.relay_service_url}/internal/sweep"
    http_method = "POST"

    oidc_token {
      service_account_email = google_service_account.scheduler.email
      audience              = var.relay_service_url
    }
  }

  # The sweep is resumable (§5.7) and Cloud Run's own request timeout
  # (300s, relay-service/main.tf) is what actually bounds one HTTP call;
  # Scheduler's attempt_deadline just needs to be at least that long so a
  # slow-but-succeeding sweep isn't reported as a failed invocation.
  attempt_deadline = "300s"

  depends_on = [google_cloud_run_v2_service_iam_member.scheduler_invoker]
}

# docs/SERVER_PLAN.md §5.2/§9.2: "backoff 30 s ... 15 min, max 5" for
# delivery retries. Not yet enqueued into by relay/app/tasks.py (TASKS_MODE
# stays "inline" until Phase 8 -- see relay-service/variables.tf) but
# provisioned now so Phase 8's CloudTasksQueue has a real queue to target
# without another Terraform module needing to be written then.
resource "google_cloud_tasks_queue" "delivery_retries" {
  project  = var.project_id
  location = var.region
  name     = var.task_queue_name

  rate_limits {
    max_concurrent_dispatches = 10 # matches app/jobs.py's own "cap dispatches at 10 per tick" precedent
    max_dispatches_per_second = 10
  }

  retry_config {
    max_attempts  = 5      # docs/SERVER_PLAN.md §5.2 "max 5"
    min_backoff   = "30s"  # §5.2 "backoff 30 s"
    max_backoff   = "900s" # §5.2 "... 15 min"
    max_doublings = 5
  }
}
