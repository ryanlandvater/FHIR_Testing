#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "$repo_root"

run_id="smoke_$(date +%Y%m%d_%H%M%S)"
"${script_dir}/local_benchmark.sh" --iterations 1 --run-id "$run_id" --no-build

metric_count="$(docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" -Atc "
SELECT COUNT(*)
FROM raw_metrics_table
WHERE run_id = '${run_id}'
  AND arm IN ('fastfhir','json_fhir')
  AND stage IN ('stage1_serialize','stage3_query');")"

if [[ "${metric_count}" -lt 4 ]]; then
  echo "Smoke validation failed: expected at least 4 FFHR/JSON stage metrics, got ${metric_count}." >&2
  exit 1
fi

echo "Smoke run completed: ${run_id}"
