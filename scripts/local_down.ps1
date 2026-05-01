$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

param(
    [switch]$CleanVolumes
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..')).Path
Set-Location $repoRoot

if ($CleanVolumes) {
    docker compose down -v --remove-orphans
} else {
    docker compose down --remove-orphans
}
