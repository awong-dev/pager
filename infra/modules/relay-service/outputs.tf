output "artifact_registry_repository" {
  description = "e.g. us-central1-docker.pkg.dev/<project>/pager -- prefix a /relay:<tag> for the actual image URI CI pushes."
  value       = "${google_artifact_registry_repository.relay.location}-docker.pkg.dev/${var.project_id}/${google_artifact_registry_repository.relay.repository_id}"
}

output "service_name" {
  value = google_cloud_run_v2_service.relay.name
}

output "service_url" {
  value = google_cloud_run_v2_service.relay.uri
}

output "service_account_email" {
  value = google_service_account.relay.email
}

output "bootstrap_job_name" {
  value = google_cloud_run_v2_job.bootstrap.name
}
