output "secret_ids" {
  description = "Map of logical name -> Secret Manager secret_id, for relay-service to wire into Cloud Run env vars."
  value = {
    broker_api_key     = google_secret_manager_secret.broker_api_key.secret_id
    broker_api_secret  = google_secret_manager_secret.broker_api_secret.secret_id
    webhook_key        = google_secret_manager_secret.webhook_key.secret_id
    twilio_account_sid = google_secret_manager_secret.twilio_account_sid.secret_id
    twilio_auth_token  = google_secret_manager_secret.twilio_auth_token.secret_id
    twilio_from_number = google_secret_manager_secret.twilio_from_number.secret_id
    cell_geo_api_key   = google_secret_manager_secret.cell_geo_api_key.secret_id
  }
}

output "secret_names" {
  description = "Fully-qualified `projects/.../secrets/...` names, keyed the same as secret_ids."
  value = {
    broker_api_key     = google_secret_manager_secret.broker_api_key.name
    broker_api_secret  = google_secret_manager_secret.broker_api_secret.name
    webhook_key        = google_secret_manager_secret.webhook_key.name
    twilio_account_sid = google_secret_manager_secret.twilio_account_sid.name
    twilio_auth_token  = google_secret_manager_secret.twilio_auth_token.name
    twilio_from_number = google_secret_manager_secret.twilio_from_number.name
    cell_geo_api_key   = google_secret_manager_secret.cell_geo_api_key.name
  }
}

output "additional_secret_ids" {
  value = { for k, s in google_secret_manager_secret.additional : k => s.secret_id }
}
