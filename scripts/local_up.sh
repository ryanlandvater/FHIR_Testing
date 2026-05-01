#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "$repo_root"

if [[ ! -f .env && -f .env.example ]]; then
  cp .env.example .env
fi

mkdir -p artifacts datasets
[[ -f artifacts/.keep ]] || touch artifacts/.keep

if [[ "${1:-}" != "--no-build" ]]; then
  docker compose build
fi

docker compose up -d

echo "Waiting for database readiness..."
for i in {1..60}; do
  if docker compose exec -T db pg_isready -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" >/dev/null 2>&1; then
    echo "DB is ready."
    exit 0
  fi
  sleep 1
done

echo "Database did not become ready in time."
exit 1
