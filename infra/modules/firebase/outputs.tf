output "web_app_id" {
  value = google_firebase_web_app.default.app_id
}

# Consumed by web/'s build (docs/SERVER_PLAN.md §9.1). Not secret -- Firebase
# web SDK config is meant to ship inside the client bundle.
output "web_sdk_config" {
  description = "Firebase JS SDK config object for web/ (apiKey, authDomain, projectId, storageBucket, messagingSenderId, appId)."
  value = {
    api_key             = data.google_firebase_web_app_config.default.api_key
    auth_domain         = data.google_firebase_web_app_config.default.auth_domain
    project_id          = var.project_id
    storage_bucket      = data.google_firebase_web_app_config.default.storage_bucket
    messaging_sender_id = data.google_firebase_web_app_config.default.messaging_sender_id
    app_id              = google_firebase_web_app.default.app_id
  }
}

output "firestore_database_name" {
  value = google_firestore_database.default.name
}

output "hosting_site_id" {
  value = google_firebase_hosting_site.default.site_id
}

output "hosting_default_url" {
  value = "https://${google_firebase_hosting_site.default.site_id}.web.app"
}
