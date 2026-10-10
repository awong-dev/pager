output "secret_ids" {
  description = "Map of logical name -> Secret Manager secret_id, for relay-service to wire into Cloud Run env vars."
  value = {
    broker_api_key      = google_secret_manager_secret.broker_api_key.secret_id
    broker_api_secret   = google_secret_manager_secret.broker_api_secret.secret_id
    webhook_key         = google_secret_manager_secret.webhook_key.secret_id
    cell_geo_api_key    = google_secret_manager_secret.cell_geo_api_key.secret_id
    soracom_auth_key_id = google_secret_manager_secret.soracom_auth_key_id.secret_id
    soracom_auth_key    = google_secret_manager_secret.soracom_auth_key.secret_id
  }
}

output "secret_names" {
  description = "Fully-qualified `projects/.../secrets/...` names, keyed the same as secret_ids."
  value = {
    broker_api_key      = google_secret_manager_secret.broker_api_key.name
    broker_api_secret   = google_secret_manager_secret.broker_api_secret.name
    webhook_key         = google_secret_manager_secret.webhook_key.name
    cell_geo_api_key    = google_secret_manager_secret.cell_geo_api_key.name
    soracom_auth_key_id = google_secret_manager_secret.soracom_auth_key_id.name
    soracom_auth_key    = google_secret_manager_secret.soracom_auth_key.name
  }
}

output "additional_secret_ids" {
  value = { for k, s in google_secret_manager_secret.additional : k => s.secret_id }
}
