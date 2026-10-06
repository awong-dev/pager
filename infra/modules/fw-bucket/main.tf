# Public firmware bucket for OTA (docs/OTA_DESIGN.md D2/D3). Layout:
#   fw/index.json, fw/<id16>/{full.z,from-<base16>.dz,manifest.json}, fw/probe-4k.bin
# Cost: a release is <= ~0.56 MB, so storage stays far inside the GCS
# always-free 5 GiB (us-* region); egress is ~$0.00004 per full OTA.
resource "google_storage_bucket" "fw" {
  project  = var.project_id
  name     = var.bucket_name
  location = var.region
  labels   = var.labels

  uniform_bucket_level_access = true
  public_access_prevention    = "inherited" # must allow allUsers below
  force_destroy               = false
  # No versioning: objects are content-addressed (same id, same bytes);
  # only fw/index.json is rewritten.
}

# World-readable on purpose. The bucket holds only firmware images; the pager
# does not trust the transport (TLS validation is off on the modem) but checks
# SHA-256 hashes carried in the HMAC-signed cfg.ota (docs/OTA_DESIGN.md D3, D7,
# D9). Public read exposes nothing that is not already in the open firmware.
resource "google_storage_bucket_iam_member" "public_read" {
  bucket = google_storage_bucket.fw.name
  role   = "roles/storage.objectViewer"
  member = "allUsers"
}

# Upload rights for the human running tools/fwpub.py (needs create + overwrite
# for index.json, hence objectAdmin rather than objectCreator).
resource "google_storage_bucket_iam_member" "publisher" {
  for_each = toset(var.publishers)

  bucket = google_storage_bucket.fw.name
  role   = "roles/storage.objectAdmin"
  member = each.value
}
