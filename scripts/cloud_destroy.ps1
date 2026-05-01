$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

param(
    [Parameter(Mandatory = $true)][ValidateSet('aws', 'gcp')][string]$Provider,
    [Parameter(Mandatory = $true)][string]$EnvDir
)

terraform -chdir="infra/$Provider/$EnvDir" init
terraform -chdir="infra/$Provider/$EnvDir" destroy -auto-approve
