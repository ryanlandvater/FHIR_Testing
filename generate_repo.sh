#!/bin/bash
set -euo pipefail

# Minimal benchmark bootstrap:
# 1) Resolve FastFHIR source (external URL first, local checkout fallback)
# 2) Build/install FastFHIR to local/
# 3) Download Synthea JSON if missing
# 4) Build this benchmark repo

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTERNAL_DIR="${REPO_ROOT}/.external"
FASTFHIR_DIR="${EXTERNAL_DIR}/FastFHIR"
FASTFHIR_BUILD="${EXTERNAL_DIR}/FastFHIR-build"
FASTFHIR_INSTALL="${REPO_ROOT}/local"
FASTFHIR_STAMP="${FASTFHIR_INSTALL}/.fastfhir_install_stamp"
FASTFHIR_DEFAULT_REPO="${FASTFHIR_DEFAULT_REPO:-https://github.com/ryanlandvater/FastFHIR.git}"
FASTFHIR_SIMDJSON_HEADER="${FASTFHIR_BUILD}/_deps/simdjson-src/include/simdjson.h"
BUILD_DIR="${REPO_ROOT}/build/bench"
SYNTHEA_DIR="${REPO_ROOT}/datasets/synthea"
SYNTHEA_DATA_URL="${SYNTHEA_DATA_URL:-https://synthetichealth.github.io/synthea-sample-data/downloads/latest/synthea_sample_data_fhir_latest.zip}"
THREADS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
FASTFHIR_SYNC_REMOTE="${FASTFHIR_SYNC_REMOTE:-0}"
FORCE_FASTFHIR_REBUILD="${FORCE_FASTFHIR_REBUILD:-0}"
FORCE_BENCH_REBUILD="${FORCE_BENCH_REBUILD:-0}"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${YELLOW}=== FastFHIR Benchmark Setup ===${NC}"

# ============================================================================
# Step 1: Resolve FastFHIR source
# ============================================================================
echo -e "${YELLOW}Step 1: Preparing FastFHIR...${NC}"

mkdir -p "${EXTERNAL_DIR}" "${SYNTHEA_DIR}"

if [[ -n "${FASTFHIR_REPO:-}" ]]; then
  FASTFHIR_REPO_URL="${FASTFHIR_REPO}"
  echo -e "${YELLOW}Using configured FastFHIR source: ${FASTFHIR_REPO_URL}${NC}"
elif [[ -d "${FASTFHIR_DIR}/.git" ]]; then
  FASTFHIR_REPO_URL=""
  echo -e "${YELLOW}FASTFHIR_REPO not set; using existing local checkout at ${FASTFHIR_DIR}${NC}"
else
  FASTFHIR_REPO_URL="${FASTFHIR_DEFAULT_REPO}"
  echo -e "${YELLOW}FASTFHIR_REPO not set; defaulting to ${FASTFHIR_REPO_URL}${NC}"
fi

if [[ -n "${FASTFHIR_REPO_URL}" ]]; then
  if [[ -d "${FASTFHIR_DIR}/.git" ]]; then
    git -C "${FASTFHIR_DIR}" remote set-url origin "${FASTFHIR_REPO_URL}" || true
    if [[ "${FASTFHIR_SYNC_REMOTE}" == "1" ]]; then
      git -C "${FASTFHIR_DIR}" fetch --tags --prune origin
      git -C "${FASTFHIR_DIR}" pull --ff-only origin "$(git -C "${FASTFHIR_DIR}" rev-parse --abbrev-ref HEAD)"
      git -C "${FASTFHIR_DIR}" submodule update --init --recursive
    else
      echo -e "${YELLOW}Skipping remote sync (set FASTFHIR_SYNC_REMOTE=1 to fetch/pull).${NC}"
    fi
  else
    rm -rf "${FASTFHIR_DIR}"
    git clone --recurse-submodules "${FASTFHIR_REPO_URL}" "${FASTFHIR_DIR}"
  fi
else
  echo -e "${YELLOW}Using existing local checkout at ${FASTFHIR_DIR}${NC}"
fi

if [[ ! -f "${FASTFHIR_DIR}/include/FastFHIR.hpp" ]]; then
  echo -e "${RED}Error: FastFHIR source unavailable.${NC}"
  echo "Provide FASTFHIR_REPO or place a valid checkout at ${FASTFHIR_DIR}."
  exit 1
fi

echo -e "${GREEN}FastFHIR located at ${FASTFHIR_DIR}${NC}"

GENERATED_SENTINEL="${FASTFHIR_DIR}/generated_src/FF_Patient.hpp"
if [[ ! -f "${GENERATED_SENTINEL}" ]]; then
  echo -e "${YELLOW}generated_src missing; running FastFHIR generator once via tools/generator/make_lib.py...${NC}"
  PYTHON_BIN="${PYTHON_BIN:-}"
  if [[ -z "${PYTHON_BIN}" ]]; then
    if command -v python3 >/dev/null 2>&1; then
      PYTHON_BIN="python3"
    elif command -v python >/dev/null 2>&1; then
      PYTHON_BIN="python"
    else
      echo -e "${RED}Error: python3/python not found; cannot run make_lib.py${NC}"
      exit 1
    fi
  fi

  (
    cd "${FASTFHIR_DIR}"
    "${PYTHON_BIN}" tools/generator/make_lib.py
  )
fi

if [[ -d "${FASTFHIR_DIR}/.git" ]]; then
  FASTFHIR_SOURCE_REV="$(git -C "${FASTFHIR_DIR}" rev-parse HEAD)"
else
  FASTFHIR_SOURCE_REV="nogit-$(stat -f %m "${FASTFHIR_DIR}/include/FastFHIR.hpp")"
fi
FASTFHIR_BUILD_FINGERPRINT="rev=${FASTFHIR_SOURCE_REV};profile=us;shared=ON;ingestor=ON"
NEEDS_FASTFHIR_BUILD=1

if [[ "${FORCE_FASTFHIR_REBUILD}" == "1" ]]; then
  echo -e "${YELLOW}Forced FastFHIR rebuild requested.${NC}"
elif [[ -f "${FASTFHIR_STAMP}" && \
        -f "${FASTFHIR_INSTALL}/include/FastFHIR.hpp" && \
        -f "${FASTFHIR_INSTALL}/lib/libfastfhir.dylib" && \
  -f "${FASTFHIR_INSTALL}/generated_src/FF_Recovery.hpp" ]]; then
  EXISTING_STAMP="$(cat "${FASTFHIR_STAMP}")"
  if [[ "${EXISTING_STAMP}" == "${FASTFHIR_BUILD_FINGERPRINT}" ]]; then
    NEEDS_FASTFHIR_BUILD=0
  fi
fi

if [[ "${NEEDS_FASTFHIR_BUILD}" == "0" && ! -f "${FASTFHIR_SIMDJSON_HEADER}" ]]; then
  echo -e "${YELLOW}FastFHIR build tree is missing bundled simdjson headers; rebuilding FastFHIR to restore build artifacts.${NC}"
  NEEDS_FASTFHIR_BUILD=1
fi

# ============================================================================
# Step 2: Build and Install FastFHIR
# ============================================================================
echo -e "${YELLOW}Step 2: Building FastFHIR...${NC}"

if [[ "${NEEDS_FASTFHIR_BUILD}" == "1" ]]; then
  mkdir -p "${FASTFHIR_BUILD}" "${FASTFHIR_INSTALL}"

  cmake -S "${FASTFHIR_DIR}" -B "${FASTFHIR_BUILD}" \
    -G "Ninja" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFASTFHIR_RUN_GENERATOR=OFF \
    -DFASTFHIR_PRODUCTION_PROFILE=us \
    -DFASTFHIR_BUILD_SHARED=ON \
    -DFASTFHIR_BUILD_INGESTOR=ON \
    -DCMAKE_INSTALL_PREFIX="${FASTFHIR_INSTALL}"

  if ! cmake --build "${FASTFHIR_BUILD}" --parallel "${THREADS}"; then
    echo -e "${RED}FastFHIR build failed${NC}"
    exit 1
  fi

  if ! cmake --install "${FASTFHIR_BUILD}"; then
    echo -e "${RED}FastFHIR install failed${NC}"
    exit 1
  fi

  echo "${FASTFHIR_BUILD_FINGERPRINT}" > "${FASTFHIR_STAMP}"
else
  echo -e "${GREEN}FastFHIR install is up to date; skipping rebuild/codegen.${NC}"
fi

# Stage generated headers to include/ for consumer use
if [[ -d "${FASTFHIR_DIR}/generated_src" ]]; then
  mkdir -p "${FASTFHIR_INSTALL}/generated_src"
  cp "${FASTFHIR_DIR}/generated_src"/*.hpp "${FASTFHIR_INSTALL}/include/" 2>/dev/null || true
  cp "${FASTFHIR_DIR}/generated_src"/*.hpp "${FASTFHIR_INSTALL}/generated_src/" 2>/dev/null || true
fi

echo -e "${GREEN}FastFHIR built and installed to ${FASTFHIR_INSTALL}${NC}"

# ============================================================================
# Step 3: Download and Prepare Synthea Data
# ============================================================================
echo -e "${YELLOW}Step 3: Preparing Synthea data...${NC}"

JSON_COUNT=$(find "${SYNTHEA_DIR}" -maxdepth 1 -type f -name '*.json' | wc -l | tr -d ' ')
if [[ "${JSON_COUNT}" == "0" ]]; then
  echo -e "${YELLOW}No Synthea JSON found. Downloading sample dataset...${NC}"
  SYNTHEA_TMP="${EXTERNAL_DIR}/synthea-download"
  rm -rf "${SYNTHEA_TMP}"
  mkdir -p "${SYNTHEA_TMP}"

  curl -L "${SYNTHEA_DATA_URL}" -o "${SYNTHEA_TMP}/synthea.zip"
  unzip -q "${SYNTHEA_TMP}/synthea.zip" -d "${SYNTHEA_TMP}"

  COPIED=0
  while IFS= read -r json_path; do
    cp "${json_path}" "${SYNTHEA_DIR}/$(basename "${json_path}")"
    COPIED=$((COPIED + 1))
  done < <(find "${SYNTHEA_TMP}" -type f -name '*.json')

  if [[ "${COPIED}" == "0" ]]; then
    echo -e "${RED}Failed to extract Synthea JSON from ${SYNTHEA_DATA_URL}${NC}"
    exit 1
  fi
  echo -e "${GREEN}Downloaded ${COPIED} Synthea JSON files${NC}"
fi

echo -e "${GREEN}Synthea JSON is ready for runtime ingestion by bench_harness.${NC}"

# ============================================================================
# Step 4: Configure and Build Benchmark
# ============================================================================
echo -e "${YELLOW}Step 4: Building benchmark harness...${NC}"

mkdir -p "${BUILD_DIR}"

if [[ "${FORCE_BENCH_REBUILD}" == "1" ]]; then
  rm -rf "${BUILD_DIR}"
  mkdir -p "${BUILD_DIR}"
fi

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
  -DFASTFHIR_ROOT="${FASTFHIR_DIR}" \
  -DFASTFHIR_INSTALL_PREFIX="${FASTFHIR_INSTALL}" \
  -DFASTFHIR_BUILD_DIR="${FASTFHIR_BUILD}"

if ! cmake --build "${BUILD_DIR}" --parallel "${THREADS}"; then
  echo -e "${RED}Benchmark build failed${NC}"
  exit 1
fi

echo -e "${GREEN}=== Setup Complete ===${NC}"
echo ""
echo "Run the benchmark:"
echo "  cd ${REPO_ROOT}"
echo "  DYLD_LIBRARY_PATH=${FASTFHIR_INSTALL}/lib ./build/bench/bench/bench_harness"
echo ""
echo "Run validation test:"
echo "  DYLD_LIBRARY_PATH=${FASTFHIR_INSTALL}/lib ./build/bench/bench/bench_timing_conformance"
