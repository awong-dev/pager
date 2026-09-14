provider "google" {
  project = var.project_id
  region  = var.region
}

provider "google-beta" {
  project = var.project_id
  region  = var.region
}

# --- Firebase project + Firestore + Auth + Hosting ------------------------
module "firebase" {
  source = "../../modules/firebase"
  providers = {
    google      = google
    google-beta = google-beta
  }

  project_id             = var.project_id
  firestore_location     = var.firestore_location
  web_app_display_name   = var.web_app_display_name
  hosting_site_id        = var.hosting_site_id
  custom_domain          = var.custom_domain
  enable_phone_auth      = var.enable_phone_auth
  enable_email_link_auth = var.enable_email_link_auth
}

# --- Secret Manager containers --------------------------------------------
module "secrets" {
  source = "../../modules/secrets"

  project_id = var.project_id
}

# --- Cloud Run relay service + one-off jobs --------------------------------
module "relay_service" {
  source = "../../modules/relay-service"

  project_id   = var.project_id
  region       = var.region
  service_name = var.relay_service_name
  image        = var.relay_image

  broker_api_url = var.broker_api_url

  broker_api_key_secret_id    = module.secrets.secret_ids.broker_api_key
  broker_api_secret_secret_id = module.secrets.secret_ids.broker_api_secret
  webhook_key_secret_id       = module.secrets.secret_ids.webhook_key

  twilio_account_sid_secret_id = var.enable_sms_secrets ? module.secrets.secret_ids.twilio_account_sid : null
  twilio_auth_token_secret_id  = var.enable_sms_secrets ? module.secrets.secret_ids.twilio_auth_token : null
  twilio_from_number_secret_id = var.enable_sms_secrets ? module.secrets.secret_ids.twilio_from_number : null
  twilio_base_url              = var.twilio_base_url

  labels = var.labels

  depends_on = [module.firebase]
}

# --- Cloud Scheduler (tick, sweep) + Cloud Tasks queue ---------------------
module "schedule" {
  source = "../../modules/schedule"

  project_id = var.project_id
  region     = var.region

  relay_service_url  = module.relay_service.service_url
  relay_service_name = module.relay_service.service_name

  tick_schedule  = var.tick_schedule
  sweep_schedule = var.sweep_schedule
  time_zone      = var.time_zone

  labels = var.labels
}

# --- GitHub Actions deploy identity (WIF) ----------------------------------
module "ci_deploy" {
  source = "../../modules/ci-deploy"

  project_id  = var.project_id
  region      = var.region
  github_org  = var.github_org
  github_repo = var.github_repo
}

# --- Optional broker fallback (docs/SERVER_PLAN.md §9.4/§9.5(a)) ----------
# Not instantiated at all unless use_broker_gce=true (default false: EMQX
# Cloud Serverless is the primary plan, configured by hand, no Terraform
# resource for it -- see infra/README.md).
module "broker_gce" {
  count  = var.use_broker_gce ? 1 : 0
  source = "../../modules/broker-gce"

  project_id = var.project_id
  region     = var.region

  broker_domain     = var.broker_domain
  letsencrypt_email = var.letsencrypt_email
  relay_service_url = module.relay_service.service_url

  broker_api_key_secret_id    = module.secrets.secret_ids.broker_api_key
  broker_api_secret_secret_id = module.secrets.secret_ids.broker_api_secret
  webhook_key_secret_id       = module.secrets.secret_ids.webhook_key

  admin_cidr_ranges = var.broker_admin_cidr_ranges

  labels = var.labels
}
