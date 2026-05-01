#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "$repo_root"

events="${1:-5000}"
run_id="ingest_stress_$(date +%Y%m%d_%H%M%S)"

echo "Starting local ingest stress with ${events} events..."
"${script_dir}/local_up.sh"

# Seed a manifest row so FK constraints are satisfied.
docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" <<SQL
INSERT INTO manifest_table (run_id, benchmark_commit_sha, environment_name)
VALUES ('${run_id}', 'local-ingest-stress', 'local')
ON CONFLICT (run_id) DO NOTHING;
SQL

# Stress insert path as a placeholder until async queue + COPY writer is implemented.
docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" <<SQL
INSERT INTO raw_metrics_table (
    run_id, arm, stage, start_ts, end_ts, duration_us, peak_rss_mb, rss_delta_mb
)
SELECT
    '${run_id}',
    'fastfhir',
    'stage3_query',
    NOW(),
    NOW(),
    1000 + (g % 100),
    32.0,
    1.0
FROM generate_series(1, ${events}) AS g;
SQL

rows=$(docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" -Atc "SELECT COUNT(*) FROM raw_metrics_table WHERE run_id='${run_id}';")

echo "Inserted ${rows} rows for ${run_id}."
echo "Note: This script currently stresses DB volume only. Replace with queue + background batch-writer conformance once implemented."
