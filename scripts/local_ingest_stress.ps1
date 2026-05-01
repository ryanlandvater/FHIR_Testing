$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

param(
    [int]$Events = 5000
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..')).Path
Set-Location $repoRoot

$runId = 'ingest_stress_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
$pgUser = if ($env:POSTGRES_USER) { $env:POSTGRES_USER } else { 'bench' }
$pgDb = if ($env:POSTGRES_DB) { $env:POSTGRES_DB } else { 'benchmark' }

Write-Host "Starting local ingest stress with $Events events..."
& (Join-Path $scriptDir 'local_up.ps1') -NoBuild

$seedSql = @"
INSERT INTO manifest_table (run_id, benchmark_commit_sha, environment_name)
VALUES ('$runId', 'local-ingest-stress', 'local')
ON CONFLICT (run_id) DO NOTHING;
"@
$seedSql | docker compose exec -T db psql -U $pgUser -d $pgDb

$stressSql = @"
INSERT INTO raw_metrics_table (
    run_id, arm, stage, start_ts, end_ts, duration_us, peak_rss_mb, rss_delta_mb
)
SELECT
    '$runId',
    'fastfhir',
    'stage3_query',
    NOW(),
    NOW(),
    1000 + (g % 100),
    32.0,
    1.0
FROM generate_series(1, $Events) AS g;
"@
$stressSql | docker compose exec -T db psql -U $pgUser -d $pgDb

$query = "SELECT COUNT(*) FROM raw_metrics_table WHERE run_id='$runId';"
$rows = docker compose exec -T db psql -U $pgUser -d $pgDb -Atc $query
Write-Host "Inserted $rows rows for $runId."
Write-Host 'Note: This script currently stresses DB volume only. Replace with queue + background batch-writer conformance once implemented.'
