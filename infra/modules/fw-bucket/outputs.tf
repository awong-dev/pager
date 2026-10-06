output "bucket_name" {
  value = google_storage_bucket.fw.name
}

output "public_base" {
  description = "Base of every object URL; cfg.ota url = public_base + the index path."
  value       = "https://storage.googleapis.com/${google_storage_bucket.fw.name}/"
}

output "index_url" {
  value = "https://storage.googleapis.com/${google_storage_bucket.fw.name}/fw/index.json"
}
