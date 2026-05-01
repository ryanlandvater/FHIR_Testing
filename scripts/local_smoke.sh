#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "$repo_root"

# Ensure stack is running.
"${script_dir}/local_up.sh" --no-build

run_id="smoke_$(date +%Y%m%d_%H%M%S)"

docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" <<SQL
INSERT INTO manifest_table (run_id, benchmark_commit_sha, environment_name)
VALUES ('${run_id}', 'local-smoke', 'local');

INSERT INTO raw_metrics_table (
  run_id, arm, stage, start_ts, end_ts, duration_us, peak_rss_mb, rss_delta_mb
)
VALUES (
  '${run_id}', 'fastfhir', 'stage3_query', NOW(), NOW(), 2500, 42.0, 2.5
);

INSERT INTO aggregate_metrics_table (
  run_id, arm, stage, n_samples, p50_us, p95_us, p99_us, p50_ms, p95_ms, p99_ms, peak_rss_mb
)
VALUES (
  '${run_id}', 'fastfhir', 'stage3_query', 1, 2500, 2500, 2500, 2.5, 2.5, 2.5, 42.0
);
SQL

mkdir -p "artifacts/${run_id}/logs" "artifacts/${run_id}/metrics" "artifacts/${run_id}/manifest"
echo "smoke run ${run_id}" > "artifacts/${run_id}/logs/smoke.log"

echo "Smoke run completed: ${run_id}"
