output "scheduler_service_account_email" {
  value = google_service_account.scheduler.email
}

output "tick_job_name" {
  value = google_cloud_scheduler_job.tick.name
}

output "sweep_job_name" {
  value = google_cloud_scheduler_job.sweep.name
}

output "task_queue_id" {
  value = google_cloud_tasks_queue.delivery_retries.id
}
