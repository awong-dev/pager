variable "project_id" {
  type        = string
  description = "GCP project id."
}

variable "additional_secret_ids" {
  description = "Extra Secret Manager secret containers beyond the fixed list in main.tf, e.g. once Phase 7's real gchat/sms adapters land and need something the fixed list didn't anticipate. Empty by default."
  type        = list(string)
  default     = []
}

variable "labels" {
  description = "Labels applied to every secret this module creates."
  type        = map(string)
  default     = { app = "pager", managed-by = "terraform" }
}
