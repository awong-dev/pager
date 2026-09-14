variable "project_id" {
  type        = string
  description = "GCP project id."
}

variable "additional_secret_ids" {
  description = "Extra Secret Manager secret containers beyond the fixed list in main.tf, for anything a future backend needs that the fixed list didn't anticipate. Empty by default."
  type        = list(string)
  default     = []
}

variable "labels" {
  description = "Labels applied to every secret this module creates."
  type        = map(string)
  default     = { app = "pager", managed-by = "terraform" }
}
