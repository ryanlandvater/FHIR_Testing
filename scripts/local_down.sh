#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "$repo_root"

if [[ "${1:-}" == "--clean-volumes" ]]; then
  docker compose down -v --remove-orphans
else
  docker compose down --remove-orphans
fi
