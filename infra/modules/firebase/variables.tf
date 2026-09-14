variable "project_id" {
  type        = string
  description = "GCP project id. Must already have Firebase-compatible billing (Blaze) — docs/SERVER_PLAN.md §9.2's 'Billing plan' note."
}

variable "firestore_location" {
  description = "Firestore native-mode location. docs/SERVER_PLAN.md §9.2: 'nam5 or the region nearest the family'. nam5 (multi-region US) is the default for best free-tier durability; switch to a single region (e.g. us-central1) if the family is not in the US."
  type        = string
  default     = "nam5"
}

variable "web_app_display_name" {
  description = "Display name for the Firebase web app resource (shows in the Firebase console)."
  type        = string
  default     = "pager-web"
}

variable "hosting_site_id" {
  description = "Firebase Hosting site id. Defaults to the project id (Firebase's own default site naming) if left empty."
  type        = string
  default     = ""
}

variable "custom_domain" {
  description = "Optional Hosting custom domain (docs/SERVER_PLAN.md §11 D6: 'None assumed; the *.web.app URL works for everything'). Empty string = no custom domain resource created."
  type        = string
  default     = ""
}

variable "enable_phone_auth" {
  description = "Enable the phone sign-in provider on Identity Platform (docs/SERVER_PLAN.md §5.3/§11 D5: 'Both enabled; the login page accepts either'). Phone sign-in can incur per-SMS cost past Firebase Auth's no-cost allowance -- kept as a variable so it can be turned off without editing this module."
  type        = bool
  default     = true
}

variable "enable_email_link_auth" {
  description = "Enable the email-link (passwordless) sign-in provider on Identity Platform."
  type        = bool
  default     = true
}
