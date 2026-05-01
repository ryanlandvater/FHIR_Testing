#!/bin/bash
set -euo pipefail

# FastFHIR Benchmark Repository Setup
# This script initializes the repository by cloning FastFHIR and configuring the build.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTERNAL_DIR="${REPO_ROOT}/.external"
FASTFHIR_DIR="${EXTERNAL_DIR}/FastFHIR"

# Load environment variables from .env if it exists
if [[ -f "${REPO_ROOT}/.env" ]]; then
    set +a  # Don't auto-export
    source "${REPO_ROOT}/.env"
    set -a
fi

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== FastFHIR Benchmark Repository Setup ===${NC}"
echo "Repository root: ${REPO_ROOT}"

# Validate that we're in the correct directory
if [[ ! -f "${REPO_ROOT}/CMakeLists.txt" ]]; then
    echo -e "${RED}Error: CMakeLists.txt not found. Are you in the repository root?${NC}"
    exit 1
fi

# Create external directory
if [[ ! -d "${EXTERNAL_DIR}" ]]; then
    echo -e "${YELLOW}Creating .external directory...${NC}"
    mkdir -p "${EXTERNAL_DIR}"
fi

# Check if FastFHIR is already cloned
if [[ -d "${FASTFHIR_DIR}" ]]; then
    echo -e "${YELLOW}FastFHIR already exists at ${FASTFHIR_DIR}${NC}"
    read -p "Do you want to re-clone FastFHIR? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${YELLOW}Removing existing FastFHIR checkout...${NC}"
        rm -rf "${FASTFHIR_DIR}"
    else
        echo -e "${GREEN}Using existing FastFHIR checkout.${NC}"
        SKIP_CLONE=1
    fi
else
    SKIP_CLONE=0
fi

# Clone FastFHIR if needed
if [[ ${SKIP_CLONE:-0} -eq 0 ]]; then
    echo -e "${YELLOW}Cloning FastFHIR with submodules...${NC}"
    # Replace with the actual FastFHIR repository URL
    FASTFHIR_REPO="${FASTFHIR_REPO:-https://github.com/your-org/FastFHIR.git}"
    
    if [[ "${FASTFHIR_REPO}" == "https://github.com/your-org/FastFHIR.git" ]]; then
        echo -e "${RED}Error: FASTFHIR_REPO environment variable not set or using placeholder.${NC}"
        echo "Set the FastFHIR repository URL:"
        echo "  export FASTFHIR_REPO=<your-fastfhir-repo-url>"
        echo "Then run this script again."
        exit 1
    fi
    
    git clone --recurse-submodules "${FASTFHIR_REPO}" "${FASTFHIR_DIR}"
fi

# Verify FastFHIR checkout
if [[ ! -f "${FASTFHIR_DIR}/include/FastFHIR.hpp" ]]; then
    echo -e "${RED}Error: FastFHIR.hpp not found in ${FASTFHIR_DIR}/include/${NC}"
    echo "The FastFHIR checkout may be incomplete or invalid."
    exit 1
fi

echo -e "${GREEN}FastFHIR checkout verified.${NC}"

# Build FastFHIR separately
echo -e "${YELLOW}Building FastFHIR library...${NC}"
FASTFHIR_BUILD="${EXTERNAL_DIR}/FastFHIR-build"
FASTFHIR_INSTALL_PREFIX="${REPO_ROOT}/local"

if [[ -d "${FASTFHIR_BUILD}" ]]; then
    echo -e "${YELLOW}Removing existing FastFHIR build directory...${NC}"
    rm -rf "${FASTFHIR_BUILD}"
fi

cmake -S "${FASTFHIR_DIR}" -B "${FASTFHIR_BUILD}" \
    -DFASTFHIR_RUN_GENERATOR=ON \
    -DFASTFHIR_PRODUCTION_PROFILE=us \
    -DFASTFHIR_BUILD_SHARED=ON \
    -DCMAKE_INSTALL_PREFIX="${FASTFHIR_INSTALL_PREFIX}"

if ! cmake --build "${FASTFHIR_BUILD}"; then
    echo -e "${RED}FastFHIR build failed.${NC}"
    exit 1
fi

if ! cmake --install "${FASTFHIR_BUILD}"; then
    echo -e "${RED}FastFHIR install failed.${NC}"
    exit 1
fi

# Temporary compatibility shim: stage generated/public headers that are required
# by installed public headers but are not yet installed by FastFHIR CMake.
mkdir -p "${FASTFHIR_INSTALL_PREFIX}/include"
mkdir -p "${FASTFHIR_INSTALL_PREFIX}/generated_src"

# Stage full public include tree to satisfy transitive header includes.
find "${FASTFHIR_DIR}/include" -maxdepth 1 -name "*.hpp" -exec cp {} "${FASTFHIR_INSTALL_PREFIX}/include/" \;

# Preserve generated header relative-include layout expected by FF_Primitives.hpp
find "${FASTFHIR_DIR}/generated_src" -maxdepth 1 -name "*.hpp" -exec cp {} "${FASTFHIR_INSTALL_PREFIX}/generated_src/" \;

# Expose selected generated API headers at include root for consumers.
for hdr in FF_Dictionary.hpp FF_R4_Dictionary.hpp FF_R5_Dictionary.hpp FF_CodeSystems.hpp FF_DataTypes.hpp FF_Patient.hpp; do
    if [[ -f "${FASTFHIR_DIR}/generated_src/${hdr}" ]]; then
        cp "${FASTFHIR_DIR}/generated_src/${hdr}" "${FASTFHIR_INSTALL_PREFIX}/include/${hdr}"
    fi
done

echo -e "${GREEN}FastFHIR built and installed to ${FASTFHIR_INSTALL_PREFIX}.${NC}"

# Configure CMake
echo -e "${YELLOW}Configuring CMake...${NC}"
BUILD_DIR="${REPO_ROOT}/build/bench"

if [[ -d "${BUILD_DIR}" ]]; then
    echo -e "${YELLOW}Removing existing build directory...${NC}"
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DFASTFHIR_ROOT="${FASTFHIR_DIR}" \
    -DFASTFHIR_INSTALL_PREFIX="${FASTFHIR_INSTALL_PREFIX}"

# Build the project
echo -e "${YELLOW}Building benchmark harness...${NC}"
cmake --build "${BUILD_DIR}"

echo -e "${GREEN}=== Setup Complete ===${NC}"
echo "The benchmark harness is ready to run:"
echo "  ./build/bench/bench/bench_harness --smoke --iterations 1"
echo ""
echo "To run the full local benchmark stack:"
echo "  ./scripts/local_up.sh"
echo "  ./scripts/local_benchmark.sh"
