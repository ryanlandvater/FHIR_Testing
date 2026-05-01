#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "$repo_root"

docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" -c "TRUNCATE TABLE raw_metrics_table, aggregate_metrics_table, manifest_table CASCADE;"

echo "Benchmark tables truncated."
