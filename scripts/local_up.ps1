$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

param(
    [switch]$NoBuild
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..')).Path
Set-Location $repoRoot

if (-not (Test-Path '.env') -and (Test-Path '.env.example')) {
    Copy-Item '.env.example' '.env'
}

New-Item -ItemType Directory -Path 'artifacts' -Force | Out-Null
New-Item -ItemType Directory -Path 'datasets' -Force | Out-Null
if (-not (Test-Path 'artifacts/.keep')) { New-Item -ItemType File -Path 'artifacts/.keep' | Out-Null }
if (-not (Test-Path 'datasets/.keep')) { New-Item -ItemType File -Path 'datasets/.keep' | Out-Null }

if (-not $NoBuild) {
    docker compose build
}

docker compose up -d

Write-Host 'Waiting for database readiness...'
$pgUser = if ($env:POSTGRES_USER) { $env:POSTGRES_USER } else { 'bench' }
$pgDb = if ($env:POSTGRES_DB) { $env:POSTGRES_DB } else { 'benchmark' }

for ($i = 0; $i -lt 60; $i++) {
    docker compose exec -T db pg_isready -U $pgUser -d $pgDb *> $null
    if ($LASTEXITCODE -eq 0) {
        Write-Host 'DB is ready.'
        exit 0
    }
    Start-Sleep -Seconds 1
}

Write-Error 'Database did not become ready in time.'
