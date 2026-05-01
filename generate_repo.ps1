# FastFHIR Benchmark Repository Setup (PowerShell)
# This script initializes the repository by cloning FastFHIR and configuring the build.

param(
    [string]$FastFHIRRepo = $env:FASTFHIR_REPO
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$EnvFile = Join-Path $RepoRoot ".env"

# Load environment variables from .env if it exists
if (Test-Path $EnvFile) {
    Get-Content $EnvFile | ForEach-Object {
        if ($_ -match '^\s*([^#][^=]*?)=(.*)$') {
            $key = $matches[1].Trim()
            $value = $matches[2].Trim()
            [Environment]::SetEnvironmentVariable($key, $value, "Process")
            # Update local parameter if FASTFHIR_REPO was loaded from .env
            if ($key -eq "FASTFHIR_REPO" -and -not $FastFHIRRepo) {
                $FastFHIRRepo = $value
            }
        }
    }
}

$ExternalDir = Join-Path $RepoRoot ".external"
$FastFHIRDir = Join-Path $ExternalDir "FastFHIR"

# Color codes (using Write-Host for colored output)
function Write-Success {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Green
}

function Write-Warning {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Yellow
}

function Write-Error {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Red
}

Write-Warning "=== FastFHIR Benchmark Repository Setup ==="
Write-Host "Repository root: $RepoRoot"

# Validate that we're in the correct directory
$CMakePath = Join-Path $RepoRoot "CMakeLists.txt"
if (-not (Test-Path $CMakePath)) {
    Write-Error "Error: CMakeLists.txt not found. Are you in the repository root?"
    exit 1
}

# Create external directory
if (-not (Test-Path $ExternalDir)) {
    Write-Warning "Creating .external directory..."
    New-Item -ItemType Directory -Path $ExternalDir -Force | Out-Null
}

# Check if FastFHIR is already cloned
$SkipClone = $false
if (Test-Path $FastFHIRDir) {
    Write-Warning "FastFHIR already exists at $FastFHIRDir"
    $response = Read-Host "Do you want to re-clone FastFHIR? (y/n)"
    if ($response -eq "y" -or $response -eq "Y") {
        Write-Warning "Removing existing FastFHIR checkout..."
        Remove-Item -Recurse -Force $FastFHIRDir
    } else {
        Write-Success "Using existing FastFHIR checkout."
        $SkipClone = $true
    }
}

# Clone FastFHIR if needed
if (-not $SkipClone) {
    Write-Warning "Cloning FastFHIR with submodules..."
    
    if (-not $FastFHIRRepo -or $FastFHIRRepo -eq "https://github.com/your-org/FastFHIR.git") {
        Write-Error "Error: FASTFHIR_REPO environment variable not set or using placeholder."
        Write-Host "Set the FastFHIR repository URL:"
        Write-Host "  `$env:FASTFHIR_REPO = '<your-fastfhir-repo-url>'"
        Write-Host "Then run this script again."
        exit 1
    }
    
    git clone --recurse-submodules $FastFHIRRepo $FastFHIRDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Error: Failed to clone FastFHIR repository"
        exit 1
    }
}

# Verify FastFHIR checkout
$FastFHIRHeader = Join-Path $FastFHIRDir "include" "FastFHIR.hpp"
if (-not (Test-Path $FastFHIRHeader)) {
    Write-Error "Error: FastFHIR.hpp not found in $FastFHIRDir\include\"
    Write-Host "The FastFHIR checkout may be incomplete or invalid."
    exit 1
}

Write-Success "FastFHIR checkout verified."

# Build and install FastFHIR separately
Write-Warning "Building FastFHIR library..."
$FastFHIRBuild = Join-Path $ExternalDir "FastFHIR-build"
$FastFHIRInstallPrefix = Join-Path $RepoRoot "build"

if (Test-Path $FastFHIRBuild) {
    Write-Warning "Removing existing FastFHIR build directory..."
    Remove-Item -Recurse -Force $FastFHIRBuild
}

$fastfhirConfigureOutput = cmake -S $FastFHIRDir -B $FastFHIRBuild `
    -DFASTFHIR_RUN_GENERATOR=ON `
    -DFASTFHIR_PRODUCTION_PROFILE=us `
    -DFASTFHIR_BUILD_SHARED=ON `
    -DCMAKE_INSTALL_PREFIX=$FastFHIRInstallPrefix 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "FastFHIR CMake configuration failed:"
    Write-Host $fastfhirConfigureOutput
    exit 1
}

$fastfhirBuildOutput = cmake --build $FastFHIRBuild 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "FastFHIR build failed:"
    Write-Host $fastfhirBuildOutput
    exit 1
}

$fastfhirInstallOutput = cmake --install $FastFHIRBuild 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "FastFHIR install failed:"
    Write-Host $fastfhirInstallOutput
    exit 1
}

# Temporary compatibility shim: stage generated/public headers that are required
# by installed public headers but are not yet installed by FastFHIR CMake.
$installInclude = Join-Path $FastFHIRInstallPrefix "include"
$installGenerated = Join-Path $FastFHIRInstallPrefix "generated_src"
New-Item -ItemType Directory -Path $installInclude -Force | Out-Null
New-Item -ItemType Directory -Path $installGenerated -Force | Out-Null

# Stage full public include tree to satisfy transitive header includes.
Get-ChildItem -Path (Join-Path $FastFHIRDir "include") -Filter "*.hpp" | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination (Join-Path $installInclude $_.Name) -Force
}

# Preserve generated header relative-include layout expected by FF_Primitives.hpp
Get-ChildItem -Path (Join-Path $FastFHIRDir "generated_src") -Filter "*.hpp" | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination (Join-Path $installGenerated $_.Name) -Force
}

foreach ($hdr in @("FF_Dictionary.hpp", "FF_R4_Dictionary.hpp", "FF_R5_Dictionary.hpp", "FF_CodeSystems.hpp", "FF_DataTypes.hpp", "FF_Patient.hpp")) {
    $src = Join-Path (Join-Path $FastFHIRDir "generated_src") $hdr
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination (Join-Path $installInclude $hdr) -Force
    }
}

Write-Success "FastFHIR built and installed to $FastFHIRInstallPrefix."

# Configure CMake
Write-Warning "Configuring CMake..."
$BuildDir = Join-Path (Join-Path $RepoRoot "build") "bench"

if (Test-Path $BuildDir) {
    Write-Warning "Removing existing build directory..."
    Remove-Item -Recurse -Force $BuildDir
}

$cmakeOutput = cmake -S $RepoRoot -B $BuildDir -DFASTFHIR_ROOT=$FastFHIRDir -DFASTFHIR_INSTALL_PREFIX=$FastFHIRInstallPrefix 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed:"
    Write-Host $cmakeOutput
    exit 1
}

# Build the project
Write-Warning "Building benchmark harness..."
$buildOutput = cmake --build $BuildDir 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed:"
    Write-Host $buildOutput
    exit 1
}

Write-Success "=== Setup Complete ==="
Write-Host "The benchmark harness is ready to run:"
Write-Host "  .\build\bench\bench\bench_harness.exe --smoke --iterations 1"
Write-Host ""
Write-Host "To run the full local benchmark stack:"
Write-Host "  .\scripts\local_up.sh"
Write-Host "  .\scripts\local_benchmark.sh"
