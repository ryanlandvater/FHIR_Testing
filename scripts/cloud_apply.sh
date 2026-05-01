#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <aws|gcp> <env-dir>"
  exit 1
fi

provider="$1"
env_dir="$2"

terraform -chdir="infra/${provider}/${env_dir}" init
terraform -chdir="infra/${provider}/${env_dir}" apply -auto-approve
