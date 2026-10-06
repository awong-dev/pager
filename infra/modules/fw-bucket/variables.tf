variable "project_id" {
  type = string
}

variable "region" {
  description = "Bucket location (a us-* region keeps it inside the GCS always-free 5 GiB tier)."
  type        = string
}

variable "bucket_name" {
  description = "Globally unique bucket name; the env defaults it to \"<project_id>-pager-fw\"."
  type        = string
}

variable "publishers" {
  description = "IAM members (e.g. [\"user:you@example.com\"]) granted roles/storage.objectAdmin so tools/fwpub.py can upload. Set in terraform.tfvars, never hard-coded."
  type        = list(string)
  default     = []
}

variable "labels" {
  type    = map(string)
  default = {}
}
