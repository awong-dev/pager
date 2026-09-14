output "state_bucket_name" {
  description = "Pass this into infra/envs/prod's backend config (backend.hcl / -backend-config)."
  value       = google_storage_bucket.tfstate.name
}

output "enabled_apis" {
  value = [for s in google_project_service.this : s.service]
}
