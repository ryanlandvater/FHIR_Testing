$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..')).Path
Set-Location $repoRoot

$pgUser = if ($env:POSTGRES_USER) { $env:POSTGRES_USER } else { 'bench' }
$pgDb = if ($env:POSTGRES_DB) { $env:POSTGRES_DB } else { 'benchmark' }

$cmd = 'TRUNCATE TABLE raw_metrics_table, aggregate_metrics_table, manifest_table CASCADE;'
docker compose exec -T db psql -U $pgUser -d $pgDb -c $cmd

Write-Host 'Benchmark tables truncated.'
