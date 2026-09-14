output "workload_identity_provider" {
  description = "Full resource name for google-github-actions/auth's `workload_identity_provider` input."
  value       = google_iam_workload_identity_pool_provider.github.name
}

output "deploy_service_account_email" {
  description = "For google-github-actions/auth's `service_account` input."
  value       = google_service_account.deploy.email
}
