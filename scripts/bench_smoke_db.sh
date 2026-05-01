#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

TARGETS_MB="${TARGETS_MB:-1,2,4,8}"
ITERATIONS="${ITERATIONS:-1}"
DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-benchmark}"
DB_USER="${DB_USER:-bench}"
DB_PASSWORD="${DB_PASSWORD:-bench}"
DB_CONTAINER="${DB_CONTAINER:-fhir_bench_db}"

DB_CONN="host=${DB_HOST} port=${DB_PORT} dbname=${DB_NAME} user=${DB_USER} password=${DB_PASSWORD}"

# Current stage coverage expectation for one iteration at one target.
# Keep this in sync with benchmark arm implementations.
EXPECTED_STAGE_KEYS=(
  "fastfhir:stage1_serialize"
  "fastfhir:stage3_query"
  "json_fhir:stage1_serialize"
  "json_fhir:stage3_query"
  "google_fhir:stage1_serialize"
  "google_fhir:stage2_transport"
  "google_fhir:stage3_query"
  "hl7v2:stage1_serialize"
  "hl7v2:stage2_transport"
  "hl7v2:stage3_query"
)

if [[ ! -x ./build/bench/bench/bench_harness ]]; then
  echo "bench_harness missing; build first with cmake --build build/bench --target bench_harness" >&2
  exit 1
fi

if ! docker ps --format '{{.Names}}' | grep -qx "$DB_CONTAINER"; then
  echo "Starting DB container via docker compose..."
  docker compose up -d db >/dev/null
fi

# Ensure core schema exists for an already-initialized volume.
docker exec -i "$DB_CONTAINER" psql -U "$DB_USER" -d "$DB_NAME" -f /docker-entrypoint-initdb.d/migrations/001_init.sql >/dev/null

LOG_FILE="$(mktemp /tmp/bench_smoke_db.XXXXXX)"
echo "Running bench_harness (targets=${TARGETS_MB}, iterations=${ITERATIONS})..."
DYLD_LIBRARY_PATH=local/lib ./build/bench/bench/bench_harness \
  --iterations "$ITERATIONS" \
  --bundle-targets-mb "$TARGETS_MB" \
  --bundle-max-mb "${TARGETS_MB##*,}" \
  --db "$DB_CONN" 2>&1 | tee "$LOG_FILE" >/dev/null

if grep -q "Failed to insert run record" "$LOG_FILE"; then
  echo "DB insert failed: benchmark_runs insert error" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

RUN_ID="$(grep -Eo 'Database run_id: [0-9]+' "$LOG_FILE" | awk '{print $3}' | tail -n1)"
if [[ -z "$RUN_ID" ]]; then
  echo "Unable to determine run_id from benchmark output" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

IFS=',' read -r -a TARGET_ARRAY <<< "$TARGETS_MB"
TARGET_COUNT="${#TARGET_ARRAY[@]}"
EXPECTED_TOTAL_ROWS=$(( TARGET_COUNT * ITERATIONS * ${#EXPECTED_STAGE_KEYS[@]} ))

ACTUAL_TOTAL_ROWS="$(docker exec -i "$DB_CONTAINER" psql -U "$DB_USER" -d "$DB_NAME" -At -c "SELECT COUNT(*) FROM benchmark_results WHERE run_id=${RUN_ID};")"
if [[ "$ACTUAL_TOTAL_ROWS" != "$EXPECTED_TOTAL_ROWS" ]]; then
  echo "Row count mismatch for run_id=${RUN_ID}: expected=${EXPECTED_TOTAL_ROWS}, actual=${ACTUAL_TOTAL_ROWS}" >&2
  exit 1
fi

for key in "${EXPECTED_STAGE_KEYS[@]}"; do
  arm="${key%%:*}"
  stage="${key##*:}"
  rows="$(docker exec -i "$DB_CONTAINER" psql -U "$DB_USER" -d "$DB_NAME" -At -c "SELECT COUNT(*) FROM benchmark_results WHERE run_id=${RUN_ID} AND arm='${arm}' AND stage='${stage}';")"
  expected_rows=$(( TARGET_COUNT * ITERATIONS ))
  if [[ "$rows" != "$expected_rows" ]]; then
    echo "Stage coverage mismatch for ${arm}/${stage}: expected=${expected_rows}, actual=${rows}" >&2
    exit 1
  fi
done

echo "Smoke DB test passed. run_id=${RUN_ID}, rows=${ACTUAL_TOTAL_ROWS}."
echo "Log: ${LOG_FILE}"
