resource "google_service_account" "broker" {
  project      = var.project_id
  account_id   = "pager-broker-gce"
  display_name = "pager broker-gce instance identity (Secret Manager read-only)"
}

# Read-only access to exactly the three secrets the startup script fetches
# -- not a project-wide secretAccessor grant.
resource "google_secret_manager_secret_iam_member" "broker_api_key" {
  project   = var.project_id
  secret_id = var.broker_api_key_secret_id
  role      = "roles/secretmanager.secretAccessor"
  member    = "serviceAccount:${google_service_account.broker.email}"
}

resource "google_secret_manager_secret_iam_member" "broker_api_secret" {
  project   = var.project_id
  secret_id = var.broker_api_secret_secret_id
  role      = "roles/secretmanager.secretAccessor"
  member    = "serviceAccount:${google_service_account.broker.email}"
}

resource "google_secret_manager_secret_iam_member" "webhook_key" {
  project   = var.project_id
  secret_id = var.webhook_key_secret_id
  role      = "roles/secretmanager.secretAccessor"
  member    = "serviceAccount:${google_service_account.broker.email}"
}

# MQTT TLS, public -- this is the broker; devices must reach it.
resource "google_compute_firewall" "mqtt" {
  project = var.project_id
  name    = "${var.instance_name}-allow-mqtt"
  network = "default" # assumes the project's default auto-mode VPC exists (true for a fresh project unless deliberately deleted)

  allow {
    protocol = "tcp"
    ports    = ["8883", "1883"] # 1883 kept for the same reason relay/docker-compose.yml keeps it: local debugging/inspection, not for firmware (which always uses TLS per PROTOCOL.md §6.1)
  }

  source_ranges = ["0.0.0.0/0"]
  target_tags   = ["pager-broker"]
}

# Port 80, public but transient: only needed for certbot's HTTP-01
# challenge (initial cert issuance + renewal). Left open rather than
# toggled per-renewal -- simpler, and serving nothing but the ACME
# challenge path costs nothing extra.
resource "google_compute_firewall" "acme_http" {
  project = var.project_id
  name    = "${var.instance_name}-allow-acme-http"
  network = "default"

  allow {
    protocol = "tcp"
    ports    = ["80"]
  }

  source_ranges = ["0.0.0.0/0"]
  target_tags   = ["pager-broker"]
}

# Dashboard/management API (18083) -- NOT public by default (var.admin_cidr_ranges
# is empty by default, so this rule is not created at all). See variables.tf.
resource "google_compute_firewall" "dashboard" {
  count   = length(var.admin_cidr_ranges) > 0 ? 1 : 0
  project = var.project_id
  name    = "${var.instance_name}-allow-dashboard"
  network = "default"

  allow {
    protocol = "tcp"
    ports    = ["18083"]
  }

  source_ranges = var.admin_cidr_ranges
  target_tags   = ["pager-broker"]
}

resource "google_compute_instance" "broker" {
  project      = var.project_id
  name         = var.instance_name
  zone         = var.zone
  machine_type = "e2-micro" # always-free-tier eligible in var.region's three allowed regions -- see variables.tf comment

  tags   = ["pager-broker"]
  labels = var.labels

  boot_disk {
    initialize_params {
      image = "debian-cloud/debian-12"
      size  = 10            # GiB -- well within the 30 GiB-month standard-disk free-tier allowance
      type  = "pd-standard" # the free-tier disk type; pd-ssd/pd-balanced are not free
    }
  }

  network_interface {
    network = "default"
    access_config {
      # Ephemeral external IPv4. docs/SERVER_PLAN.md §9.5(a): "external
      # IPv4 ≈ $4 unless waived" -- as of Google's 2024 external-IP pricing
      # change this applies whether the address is static or ephemeral, and
      # whether or not the free-tier waiver still applies depends on the
      # account. This is the one line item in this module that is NOT
      # guaranteed $0 -- see infra/README.md's cost callout.
    }
  }

  service_account {
    email = google_service_account.broker.email
    scopes = [
      "https://www.googleapis.com/auth/cloud-platform", # broad, but this SA only *has* the three secretAccessor grants above -- scope is a ceiling, IAM is the actual limit
    ]
  }

  metadata_startup_script = templatefile("${path.module}/startup-script.sh.tpl", {
    project_id                  = var.project_id
    broker_domain               = var.broker_domain
    letsencrypt_email           = var.letsencrypt_email
    relay_service_url           = var.relay_service_url
    broker_api_key_secret_id    = var.broker_api_key_secret_id
    broker_api_secret_secret_id = var.broker_api_secret_secret_id
    webhook_key_secret_id       = var.webhook_key_secret_id
    emqx_image                  = var.emqx_image
  })

  allow_stopping_for_update = true
}
