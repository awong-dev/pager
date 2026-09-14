output "instance_name" {
  value = google_compute_instance.broker.name
}

output "external_ip" {
  description = "Point var.broker_domain's DNS A record here."
  value       = google_compute_instance.broker.network_interface[0].access_config[0].nat_ip
}

output "service_account_email" {
  value = google_service_account.broker.email
}
