$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..')).Path
Set-Location $repoRoot

& (Join-Path $scriptDir 'local_up.ps1') -NoBuild

$runId = 'smoke_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
$pgUser = if ($env:POSTGRES_USER) { $env:POSTGRES_USER } else { 'bench' }
$pgDb = if ($env:POSTGRES_DB) { $env:POSTGRES_DB } else { 'benchmark' }

$sql = @"
INSERT INTO manifest_table (run_id, benchmark_commit_sha, environment_name)
VALUES ('$runId', 'local-smoke', 'local');

INSERT INTO raw_metrics_table (
  run_id, arm, stage, start_ts, end_ts, duration_us, peak_rss_mb, rss_delta_mb
)
VALUES (
  '$runId', 'fastfhir', 'stage3_query', NOW(), NOW(), 2500, 42.0, 2.5
);

INSERT INTO aggregate_metrics_table (
  run_id, arm, stage, n_samples, p50_us, p95_us, p99_us, p50_ms, p95_ms, p99_ms, peak_rss_mb
)
VALUES (
  '$runId', 'fastfhir', 'stage3_query', 1, 2500, 2500, 2500, 2.5, 2.5, 2.5, 42.0
);
"@

$sql | docker compose exec -T db psql -U $pgUser -d $pgDb

$artifactRoot = Join-Path 'artifacts' $runId
New-Item -ItemType Directory -Path (Join-Path $artifactRoot 'logs') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $artifactRoot 'metrics') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $artifactRoot 'manifest') -Force | Out-Null
"smoke run $runId" | Out-File -Encoding utf8 (Join-Path $artifactRoot 'logs/smoke.log')

Write-Host "Smoke run completed: $runId"
