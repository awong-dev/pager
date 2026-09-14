output "relay_service_url" {
  value = module.relay_service.service_url
}

output "artifact_registry_repository" {
  value = module.relay_service.artifact_registry_repository
}

output "hosting_default_url" {
  value = module.firebase.hosting_default_url
}

output "firebase_web_sdk_config" {
  description = "Feed into web/'s build (docs/SERVER_PLAN.md §9.1)."
  value       = module.firebase.web_sdk_config
}

output "ci_deploy_workload_identity_provider" {
  value = module.ci_deploy.workload_identity_provider
}

output "ci_deploy_service_account_email" {
  value = module.ci_deploy.deploy_service_account_email
}

output "scheduler_service_account_email" {
  value = module.schedule.scheduler_service_account_email
}

output "broker_gce_external_ip" {
  value = var.use_broker_gce ? module.broker_gce[0].external_ip : null
}
