#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "$repo_root"

iterations=25
run_id="run_$(date +%Y%m%d_%H%M%S)"
build_flag=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--iterations)
			iterations="${2:-25}"
			shift 2
			;;
		--run-id)
			run_id="${2:-$run_id}"
			shift 2
			;;
		--no-build)
			build_flag="--no-build"
			shift
			;;
		*)
			echo "Unknown argument: $1"
			exit 1
			;;
	esac
done

"${script_dir}/local_up.sh" $build_flag

cmake -S "$repo_root" -B "$repo_root/build"
cmake --build "$repo_root/build" --target bench_harness

artifact_root="$repo_root/artifacts/${run_id}"
metrics_dir="$artifact_root/metrics"
logs_dir="$artifact_root/logs"
manifest_dir="$artifact_root/manifest"
mkdir -p "$metrics_dir" "$logs_dir" "$manifest_dir"

metrics_csv="$metrics_dir/raw_metrics.csv"
"$repo_root/build/bench/bench_harness" --smoke --iterations "$iterations" | tee "$metrics_csv" > "$logs_dir/harness.log"

docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" <<SQL
INSERT INTO manifest_table (run_id, benchmark_commit_sha, environment_name)
VALUES ('${run_id}', 'local-benchmark', 'local')
ON CONFLICT (run_id) DO NOTHING;
SQL

values_file="$metrics_dir/raw_metrics_values.sql"
: > "$values_file"

while IFS=',' read -r arm stage duration_us; do
	[[ -z "${arm}" || -z "${stage}" || -z "${duration_us}" ]] && continue
	if [[ "$duration_us" =~ ^[0-9]+$ ]]; then
		printf "('%s','%s','%s', NOW() - INTERVAL '%s microseconds', NOW(), %s, NULL, NULL),\n" \
			"$run_id" "$arm" "$stage" "$duration_us" "$duration_us" >> "$values_file"
	fi
done < "$metrics_csv"

if [[ ! -s "$values_file" ]]; then
	echo "No metrics parsed from harness output; aborting." >&2
	exit 1
fi

sed -i '' '$ s/,$//' "$values_file"

docker compose exec -T db psql -U "${POSTGRES_USER:-bench}" -d "${POSTGRES_DB:-benchmark}" <<SQL
INSERT INTO raw_metrics_table (
	run_id, arm, stage, start_ts, end_ts, duration_us, peak_rss_mb, rss_delta_mb
)
VALUES
$(cat "$values_file");

INSERT INTO aggregate_metrics_table (
	run_id, arm, stage, n_samples, p50_us, p95_us, p99_us, median_us, p50_ms, p95_ms, p99_ms, median_ms, peak_rss_mb, rss_delta_mb
)
SELECT
	run_id,
	arm,
	stage,
	COUNT(*) AS n_samples,
	percentile_cont(0.50) WITHIN GROUP (ORDER BY duration_us) AS p50_us,
	percentile_cont(0.95) WITHIN GROUP (ORDER BY duration_us) AS p95_us,
	percentile_cont(0.99) WITHIN GROUP (ORDER BY duration_us) AS p99_us,
	percentile_cont(0.50) WITHIN GROUP (ORDER BY duration_us) AS median_us,
	percentile_cont(0.50) WITHIN GROUP (ORDER BY duration_us) / 1000.0 AS p50_ms,
	percentile_cont(0.95) WITHIN GROUP (ORDER BY duration_us) / 1000.0 AS p95_ms,
	percentile_cont(0.99) WITHIN GROUP (ORDER BY duration_us) / 1000.0 AS p99_ms,
	percentile_cont(0.50) WITHIN GROUP (ORDER BY duration_us) / 1000.0 AS median_ms,
	MAX(peak_rss_mb) AS peak_rss_mb,
	AVG(rss_delta_mb) AS rss_delta_mb
FROM raw_metrics_table
WHERE run_id = '${run_id}'
GROUP BY run_id, arm, stage;
SQL

echo "$run_id" > "$manifest_dir/run_id.txt"
echo "Completed local benchmark run: $run_id"
