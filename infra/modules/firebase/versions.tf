terraform {
  required_version = ">= 1.6.0"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 8.2.0"
    }
    google-beta = {
      source  = "hashicorp/google-beta"
      version = "~> 8.2.0" # Firebase-specific resources (google_firebase_*, google_identity_platform_config) are google-beta-only
    }
  }
}
