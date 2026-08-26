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
BENCH_BAZEL_BUILD_ROOT="${EXTERNAL_DIR}/bazel-bench-build"
BENCH_BAZEL_OUTPUT_BASE="${BENCH_BAZEL_BUILD_ROOT}/output-base"
BENCH_BAZEL_REPOSITORY_CACHE="${BENCH_BAZEL_BUILD_ROOT}/repository-cache"
SYNTHEA_DIR="${REPO_ROOT}/datasets/synthea"
SYNTHEA_DATA_URL="${SYNTHEA_DATA_URL:-https://synthetichealth.github.io/synthea-sample-data/downloads/latest/synthea_sample_data_fhir_latest.zip}"
THREADS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
FASTFHIR_SYNC_REMOTE="${FASTFHIR_SYNC_REMOTE:-0}"
FORCE_FASTFHIR_REBUILD="${FORCE_FASTFHIR_REBUILD:-0}"
FORCE_BENCH_REBUILD="${FORCE_BENCH_REBUILD:-0}"
GOOGLE_FHIR_ENABLE="${GOOGLE_FHIR_ENABLE:-1}"
GOOGLE_FHIR_DIR="${EXTERNAL_DIR}/google-fhir"
GOOGLE_FHIR_BUILD="${EXTERNAL_DIR}/google-fhir-build"
GOOGLE_FHIR_HEADERS="${EXTERNAL_DIR}/google-fhir-headers"
GOOGLE_FHIR_ARTIFACTS="${EXTERNAL_DIR}/google-fhir-artifacts"
GOOGLE_FHIR_STAMP="${FASTFHIR_INSTALL}/.google_fhir_build_stamp"
GOOGLE_FHIR_DEFAULT_REPO="${GOOGLE_FHIR_DEFAULT_REPO:-https://github.com/google/fhir.git}"
GOOGLE_FHIR_SYNC_REMOTE="${GOOGLE_FHIR_SYNC_REMOTE:-0}"
FORCE_GOOGLE_FHIR_REBUILD="${FORCE_GOOGLE_FHIR_REBUILD:-0}"
GOOGLE_FHIR_BAZEL_VERSION="${GOOGLE_FHIR_BAZEL_VERSION:-7.7.1}"
GOOGLE_FHIR_BAZELISK_VERSION="${GOOGLE_FHIR_BAZELISK_VERSION:-v1.22.1}"
GOOGLE_FHIR_OUTPUT_BASE="${GOOGLE_FHIR_BUILD}/output-base"
GOOGLE_FHIR_REPOSITORY_CACHE="${GOOGLE_FHIR_BUILD}/repository-cache"
GOOGLE_FHIR_CLEAN_ARTIFACTS="${GOOGLE_FHIR_CLEAN_ARTIFACTS:-0}"
TEST_GOOGLE_FHIR_COMPONENTS="${TEST_GOOGLE_FHIR_COMPONENTS:-1}"
TEST_BENCH_COMPONENTS="${TEST_BENCH_COMPONENTS:-1}"
GOOGLE_FHIR_USE_SYSTEM_ZLIB="${GOOGLE_FHIR_USE_SYSTEM_ZLIB:-1}"
GOOGLE_FHIR_APPLY_PATCH="${GOOGLE_FHIR_APPLY_PATCH:-1}"
GOOGLE_FHIR_PATCH_FILE="${GOOGLE_FHIR_PATCH_FILE:-${REPO_ROOT}/patches/google-fhir-benchmark.patch}"
GOOGLE_FHIR_BUILD_STATIC_CLOSURE="${GOOGLE_FHIR_BUILD_STATIC_CLOSURE:-1}"
HL7PARSER_ENABLE="${HL7PARSER_ENABLE:-1}"
HL7PARSER_DIR="${EXTERNAL_DIR}/hl7parser"
HL7PARSER_DEFAULT_REPO="${HL7PARSER_DEFAULT_REPO:-https://github.com/jcomellas/hl7parser.git}"
HL7PARSER_SYNC_REMOTE="${HL7PARSER_SYNC_REMOTE:-0}"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
GOOGLE_FHIR_CLEANUP_ON_EXIT=0
GOOGLE_FHIR_JAVA_HOME="${GOOGLE_FHIR_JAVA_HOME:-}"
GOOGLE_FHIR_JDK_VERSION="${GOOGLE_FHIR_JDK_VERSION:-17}"
GOOGLE_FHIR_JDK_AUTO_DOWNLOAD="${GOOGLE_FHIR_JDK_AUTO_DOWNLOAD:-1}"
GOOGLE_FHIR_JDK_CACHE_DIR="${EXTERNAL_DIR}/jdk"
GOOGLE_FHIR_SYSTEM_ZLIB_REPO="${GOOGLE_FHIR_BUILD}/system-zlib-repo"

echo -e "${YELLOW}=== FastFHIR Benchmark Setup ===${NC}"

cleanup_google_fhir_artifacts() {
  if [[ "${GOOGLE_FHIR_ENABLE}" != "1" || "${GOOGLE_FHIR_CLEAN_ARTIFACTS}" != "1" ]]; then
    return 0
  fi

  if [[ "${GOOGLE_FHIR_CLEANUP_ON_EXIT}" != "1" ]]; then
    return 0
  fi

  echo -e "${YELLOW}Cleanup-on-exit: removing Google FHIR build artifacts...${NC}" >&2
  rm -rf "${GOOGLE_FHIR_OUTPUT_BASE}" "${GOOGLE_FHIR_REPOSITORY_CACHE}"
  rm -rf "${GOOGLE_FHIR_DIR}/bazel-bin" "${GOOGLE_FHIR_DIR}/bazel-out" "${GOOGLE_FHIR_DIR}/bazel-testlogs"
  find "${GOOGLE_FHIR_DIR}" -maxdepth 1 -type l -name 'bazel-*' -delete 2>/dev/null || true
  rm -rf "${GOOGLE_FHIR_HEADERS}" "${GOOGLE_FHIR_ARTIFACTS}"
  rm -rf "${GOOGLE_FHIR_BUILD}"
}

stage_google_fhir_runtime_artifacts() {
  local proto_src="${GOOGLE_FHIR_OUTPUT_BASE}/external/com_google_protobuf/src"
  local absl_src="${GOOGLE_FHIR_OUTPUT_BASE}/external/com_google_absl"
  local dylib_src="${GOOGLE_FHIR_OUTPUT_BASE}/execroot/com_google_fhir/bazel-out/darwin_arm64-fastbuild/bin/cc/google/fhir/liblibgoogle_fhir_bundled.dylib"
  local proto_dest="${GOOGLE_FHIR_HEADERS}/protobuf"
  local absl_dest="${GOOGLE_FHIR_HEADERS}/absl"
  local dylib_dest_dir="${GOOGLE_FHIR_ARTIFACTS}/lib"
  local dylib_dest="${dylib_dest_dir}/liblibgoogle_fhir_bundled.dylib"

  if [[ ! -f "${dylib_src}" ]]; then
    echo -e "${RED}Error: Google FHIR dylib not found at ${dylib_src}${NC}" >&2
    return 1
  fi

  echo -e "${YELLOW}Staging Google FHIR dependency headers for Bazel sandbox...${NC}"
  rm -rf "${GOOGLE_FHIR_HEADERS}"
  mkdir -p "${proto_dest}" "${absl_dest}"
  rsync -a --include='*/' --include='*.h' --include='*.inc' --include='*.proto' --exclude='*' \
    "${proto_src}/" "${proto_dest}/"
  rsync -a --include='*/' --include='*.h' --include='*.inc' --include='*.def' --exclude='*' \
    "${absl_src}/" "${absl_dest}/"
  echo -e "${GREEN}Header staging complete: ${GOOGLE_FHIR_HEADERS}${NC}"

  echo -e "${YELLOW}Staging relocatable Google FHIR runtime dylib...${NC}"
  rm -rf "${GOOGLE_FHIR_ARTIFACTS}"
  mkdir -p "${dylib_dest_dir}"
  cp "${dylib_src}" "${dylib_dest}"
  chmod u+w "${dylib_dest}"
  install_name_tool -id "@rpath/liblibgoogle_fhir_bundled.dylib" "${dylib_dest}"
  echo -e "${GREEN}Runtime dylib staging complete: ${dylib_dest}${NC}"
}

trap cleanup_google_fhir_artifacts EXIT

ensure_bazelisk() {
  local bin_dir="${EXTERNAL_DIR}/bin"
  local bazelisk_bin="${bin_dir}/bazelisk"
  local os arch asset url

  if [[ -x "${bazelisk_bin}" ]]; then
    echo "${bazelisk_bin}"
    return 0
  fi

  if command -v bazelisk >/dev/null 2>&1; then
    command -v bazelisk
    return 0
  fi

  mkdir -p "${bin_dir}"
  os="$(uname -s)"
  arch="$(uname -m)"

  case "${os}" in
    Darwin)
      case "${arch}" in
        arm64) asset="bazelisk-darwin-arm64" ;;
        x86_64) asset="bazelisk-darwin-amd64" ;;
        *)
          echo -e "${RED}Unsupported macOS arch for Bazelisk: ${arch}${NC}" >&2
          return 1
          ;;
      esac
      ;;
    Linux)
      case "${arch}" in
        aarch64|arm64) asset="bazelisk-linux-arm64" ;;
        x86_64|amd64) asset="bazelisk-linux-amd64" ;;
        *)
          echo -e "${RED}Unsupported Linux arch for Bazelisk: ${arch}${NC}" >&2
          return 1
          ;;
      esac
      ;;
    *)
      echo -e "${RED}Unsupported OS for Bazelisk: ${os}${NC}" >&2
      return 1
      ;;
  esac

  url="https://github.com/bazelbuild/bazelisk/releases/download/${GOOGLE_FHIR_BAZELISK_VERSION}/${asset}"
  echo -e "${YELLOW}Downloading Bazelisk ${GOOGLE_FHIR_BAZELISK_VERSION} (${asset})...${NC}" >&2
  curl -L "${url}" -o "${bazelisk_bin}"
  chmod +x "${bazelisk_bin}"

  echo "${bazelisk_bin}"
}

ensure_google_fhir_java() {
  local candidate=""
  local version_output=""
  local local_jdk_home=""

  detect_local_jdk_home() {
    local root="$1"
    local found=""
    if [[ ! -d "${root}" ]]; then
      echo ""
      return 0
    fi

    found="$(find "${root}" -type f -path '*/bin/java' | head -n 1 || true)"
    if [[ -n "${found}" ]]; then
      dirname "$(dirname "${found}")"
      return 0
    fi

    echo ""
    return 0
  }

  ensure_repo_local_jdk() {
    local os=""
    local arch=""
    local api_arch=""
    local api_os=""
    local download_url=""
    local tmp_archive=""
    local target_root="${GOOGLE_FHIR_JDK_CACHE_DIR}/temurin-${GOOGLE_FHIR_JDK_VERSION}"
    local detected_home=""

    mkdir -p "${GOOGLE_FHIR_JDK_CACHE_DIR}"

    detected_home="$(detect_local_jdk_home "${target_root}")"
    if [[ -n "${detected_home}" ]] && is_modern_java_home "${detected_home}"; then
      echo "${detected_home}"
      return 0
    fi

    if [[ "${GOOGLE_FHIR_JDK_AUTO_DOWNLOAD}" != "1" ]]; then
      echo ""
      return 0
    fi

    os="$(uname -s)"
    arch="$(uname -m)"

    case "${os}" in
      Darwin) api_os="mac" ;;
      Linux) api_os="linux" ;;
      *)
        echo ""
        return 0
        ;;
    esac

    case "${arch}" in
      arm64|aarch64) api_arch="aarch64" ;;
      x86_64|amd64) api_arch="x64" ;;
      *)
        echo ""
        return 0
        ;;
    esac

    download_url="https://api.adoptium.net/v3/binary/latest/${GOOGLE_FHIR_JDK_VERSION}/ga/${api_os}/${api_arch}/jdk/hotspot/normal/eclipse?project=jdk"
    tmp_archive="${GOOGLE_FHIR_JDK_CACHE_DIR}/temurin-${GOOGLE_FHIR_JDK_VERSION}-${api_os}-${api_arch}.tar.gz"

    echo -e "${YELLOW}Downloading repo-local Temurin JDK ${GOOGLE_FHIR_JDK_VERSION} (${api_os}/${api_arch})...${NC}" >&2
    rm -rf "${target_root}"
    mkdir -p "${target_root}"
    curl -fsSL "${download_url}" -o "${tmp_archive}"
    tar -xzf "${tmp_archive}" -C "${target_root}" --strip-components=1
    rm -f "${tmp_archive}"

    detected_home="$(detect_local_jdk_home "${target_root}")"
    if [[ -n "${detected_home}" ]] && is_modern_java_home "${detected_home}"; then
      echo "${detected_home}"
      return 0
    fi

    echo ""
    return 0
  }

  java_major_version() {
    local java_bin="$1"
    local first_line=""
    first_line="$(${java_bin} -version 2>&1 | head -n 1 || true)"
    if [[ "${first_line}" =~ \"([0-9]+)(\.[0-9]+)? ]]; then
      echo "${BASH_REMATCH[1]}"
      return 0
    fi
    echo ""
    return 0
  }

  is_modern_java_home() {
    local home="$1"
    local major_version=""
    if [[ ! -x "${home}/bin/java" ]]; then
      return 1
    fi
    major_version="$(java_major_version "${home}/bin/java")"
    [[ -n "${major_version}" && "${major_version}" -ge 17 ]]
  }

  # Allow explicit override first.
  if [[ -n "${GOOGLE_FHIR_JAVA_HOME}" ]] && is_modern_java_home "${GOOGLE_FHIR_JAVA_HOME}"; then
    echo "${GOOGLE_FHIR_JAVA_HOME}"
    return 0
  fi

  # Prefer a repo-local JDK cache for self-contained builds.
  local_jdk_home="$(ensure_repo_local_jdk)"
  if [[ -n "${local_jdk_home}" ]] && is_modern_java_home "${local_jdk_home}"; then
    echo "${local_jdk_home}"
    return 0
  fi

  # Prefer system-discovered JDK 21/17 on macOS.
  if command -v /usr/libexec/java_home >/dev/null 2>&1; then
    candidate="$(/usr/libexec/java_home -v 21 2>/dev/null || true)"
    if [[ -n "${candidate}" ]] && is_modern_java_home "${candidate}"; then
      echo "${candidate}"
      return 0
    fi

    candidate="$(/usr/libexec/java_home -v 17 2>/dev/null || true)"
    if [[ -n "${candidate}" ]] && is_modern_java_home "${candidate}"; then
      echo "${candidate}"
      return 0
    fi
  fi

  # Fall back to common Homebrew locations.
  for candidate in \
    "/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home" \
    "/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home" \
    "/usr/local/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home" \
    "/usr/local/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home"; do
    if is_modern_java_home "${candidate}"; then
      echo "${candidate}"
      return 0
    fi
  done

  echo -e "${RED}Error: Google FHIR build requires JDK 17+ (legacy JDK 8/11 causes Maven TLS handshake failures).${NC}" >&2
  if [[ -n "${GOOGLE_FHIR_JAVA_HOME}" ]]; then
    version_output="$(${GOOGLE_FHIR_JAVA_HOME}/bin/java -version 2>&1 | head -n 1 || true)"
    echo "Current GOOGLE_FHIR_JAVA_HOME is not Java 17+: ${GOOGLE_FHIR_JAVA_HOME} (${version_output})" >&2
  fi
  echo "Repo-local JDK auto-download is controlled by GOOGLE_FHIR_JDK_AUTO_DOWNLOAD=${GOOGLE_FHIR_JDK_AUTO_DOWNLOAD}." >&2
  echo "(Default is 1 and stores JDK under ${GOOGLE_FHIR_JDK_CACHE_DIR}/)" >&2
  echo "Set GOOGLE_FHIR_JAVA_HOME to a JDK 17+ home, e.g.:" >&2
  echo "  export GOOGLE_FHIR_JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home" >&2
  return 1
}

ensure_google_fhir_patch_applied() {
  if [[ "${GOOGLE_FHIR_APPLY_PATCH}" != "1" ]]; then
    return 0
  fi

  if [[ ! -f "${GOOGLE_FHIR_PATCH_FILE}" ]]; then
    echo -e "${YELLOW}Google FHIR patch file not found at ${GOOGLE_FHIR_PATCH_FILE}; continuing without patch.${NC}"
    return 0
  fi

  pushd "${GOOGLE_FHIR_DIR}" >/dev/null
  if git apply --check "${GOOGLE_FHIR_PATCH_FILE}" >/dev/null 2>&1; then
    echo -e "${YELLOW}Applying Google FHIR benchmark patch: ${GOOGLE_FHIR_PATCH_FILE}${NC}"
    git apply "${GOOGLE_FHIR_PATCH_FILE}"
  elif git apply -R --check "${GOOGLE_FHIR_PATCH_FILE}" >/dev/null 2>&1; then
    echo -e "${GREEN}Google FHIR benchmark patch already applied.${NC}"
  else
    echo -e "${RED}Google FHIR patch cannot be applied cleanly: ${GOOGLE_FHIR_PATCH_FILE}${NC}"
    echo -e "${RED}Upstream google/fhir BUILD layout likely changed. Update the patch file in patches/.${NC}"
    popd >/dev/null
    exit 1
  fi
  popd >/dev/null
}

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
    git clone --depth 1 --recurse-submodules --shallow-submodules "${FASTFHIR_REPO_URL}" "${FASTFHIR_DIR}"
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

# generated_src/ is produced by FastFHIR's CMake configure step, and its
# contents are PROFILE-DEPENDENT (FASTFHIR_PRODUCTION_PROFILE). Bazel does not
# run the generator -- FastFHIR's BUILD.bazel just globs generated_src/*.cpp --
# so whatever CMake last generated is what gets benchmarked. See README,
# "Result provenance".
#
# The generator moved: `tools/generator/make_lib.py` no longer exists. It is
# now the `generator` package, invoked as `python -m generator` or, preferably,
# via the CMake preset that also validates the code-system enums against the
# permanent ledger.
GENERATED_SENTINEL="${FASTFHIR_DIR}/generated_src/FF_Patient.hpp"
if [[ ! -f "${GENERATED_SENTINEL}" ]]; then
  echo -e "${YELLOW}generated_src missing; running FastFHIR generator once via 'cmake --preset ninja'...${NC}"

  if ! command -v cmake >/dev/null 2>&1; then
    echo -e "${RED}Error: cmake not found; cannot generate FastFHIR sources.${NC}"
    echo -e "${RED}Install CMake, or run 'python -m generator' from ${FASTFHIR_DIR}.${NC}"
    exit 1
  fi

  (
    cd "${FASTFHIR_DIR}"
    cmake --preset ninja
  )

  if [[ ! -f "${GENERATED_SENTINEL}" ]]; then
    echo -e "${RED}Error: generator ran but ${GENERATED_SENTINEL} is still missing.${NC}"
    exit 1
  fi
fi

if [[ -d "${FASTFHIR_DIR}/.git" ]]; then
  FASTFHIR_SOURCE_REV="$(git -C "${FASTFHIR_DIR}" rev-parse HEAD)"
else
  FASTFHIR_SOURCE_REV="nogit-$(stat -f %m "${FASTFHIR_DIR}/include/FastFHIR.hpp")"
fi

# ============================================================================
# Step 1B: Resolve Google FHIR source
# ============================================================================
if [[ "${GOOGLE_FHIR_ENABLE}" == "1" ]]; then
  echo -e "${YELLOW}Step 1B: Preparing Google FHIR source...${NC}"

  if [[ -n "${GOOGLE_FHIR_REPO:-}" ]]; then
    GOOGLE_FHIR_REPO_URL="${GOOGLE_FHIR_REPO}"
  else
    GOOGLE_FHIR_REPO_URL="${GOOGLE_FHIR_DEFAULT_REPO}"
  fi

  if [[ -d "${GOOGLE_FHIR_DIR}/.git" ]]; then
    git -C "${GOOGLE_FHIR_DIR}" remote set-url origin "${GOOGLE_FHIR_REPO_URL}" || true
    if [[ "${GOOGLE_FHIR_SYNC_REMOTE}" == "1" ]]; then
      git -C "${GOOGLE_FHIR_DIR}" fetch --tags --prune origin
      git -C "${GOOGLE_FHIR_DIR}" pull --ff-only origin "$(git -C "${GOOGLE_FHIR_DIR}" rev-parse --abbrev-ref HEAD)"
    else
      echo -e "${YELLOW}Skipping Google FHIR remote sync (set GOOGLE_FHIR_SYNC_REMOTE=1 to fetch/pull).${NC}"
    fi
  else
    rm -rf "${GOOGLE_FHIR_DIR}"
    git clone --depth 1 "${GOOGLE_FHIR_REPO_URL}" "${GOOGLE_FHIR_DIR}"
  fi

  if [[ ! -f "${GOOGLE_FHIR_DIR}/WORKSPACE" && ! -f "${GOOGLE_FHIR_DIR}/WORKSPACE.bazel" ]]; then
    echo -e "${RED}Error: Google FHIR workspace not found at ${GOOGLE_FHIR_DIR}${NC}"
    exit 1
  fi

  ensure_google_fhir_patch_applied

  GOOGLE_FHIR_PATCH_FINGERPRINT="patch=none"
  if [[ "${GOOGLE_FHIR_APPLY_PATCH}" == "1" && -f "${GOOGLE_FHIR_PATCH_FILE}" ]]; then
    GOOGLE_FHIR_PATCH_FINGERPRINT="patch=$(shasum -a 256 "${GOOGLE_FHIR_PATCH_FILE}" | awk '{print $1}')"
  fi

  if [[ -d "${GOOGLE_FHIR_DIR}/.git" ]]; then
    GOOGLE_FHIR_SOURCE_REV="$(git -C "${GOOGLE_FHIR_DIR}" rev-parse HEAD)"
  else
    GOOGLE_FHIR_SOURCE_REV="nogit-$(stat -f %m "${GOOGLE_FHIR_DIR}")"
  fi

  GOOGLE_FHIR_TARGET_FINGERPRINT="//cc/google/fhir:json_format //cc/google/fhir/r4:json_format //cc/google/fhir:libgoogle_fhir_bundled //proto/google/fhir/proto/r4/core/resources:patient_cc_proto @com_google_protobuf//:protobuf @com_google_absl//absl/...:all"
  GOOGLE_FHIR_BUILD_FINGERPRINT="rev=${GOOGLE_FHIR_SOURCE_REV};bazel=${GOOGLE_FHIR_BAZEL_VERSION};targets=${GOOGLE_FHIR_TARGET_FINGERPRINT};${GOOGLE_FHIR_PATCH_FINGERPRINT};static_closure=${GOOGLE_FHIR_BUILD_STATIC_CLOSURE}"

  NEEDS_GOOGLE_FHIR_BUILD=1
  if [[ "${FORCE_GOOGLE_FHIR_REBUILD}" == "1" ]]; then
    echo -e "${YELLOW}Forced Google FHIR rebuild requested.${NC}"
  elif [[ -f "${GOOGLE_FHIR_STAMP}" ]]; then
    EXISTING_GOOGLE_FHIR_STAMP="$(cat "${GOOGLE_FHIR_STAMP}")"
    if [[ "${EXISTING_GOOGLE_FHIR_STAMP}" == "${GOOGLE_FHIR_BUILD_FINGERPRINT}" ]]; then
      NEEDS_GOOGLE_FHIR_BUILD=0
    fi
  fi
else
  NEEDS_GOOGLE_FHIR_BUILD=0
fi

# ============================================================================
# Step 1C: Resolve HL7 parser source
# ============================================================================
if [[ "${HL7PARSER_ENABLE}" == "1" ]]; then
  echo -e "${YELLOW}Step 1C: Preparing HL7 parser source...${NC}"

  if [[ -n "${HL7PARSER_REPO:-}" ]]; then
    HL7PARSER_REPO_URL="${HL7PARSER_REPO}"
  else
    HL7PARSER_REPO_URL="${HL7PARSER_DEFAULT_REPO}"
  fi

  if [[ -d "${HL7PARSER_DIR}/.git" ]]; then
    git -C "${HL7PARSER_DIR}" remote set-url origin "${HL7PARSER_REPO_URL}" || true
    if [[ "${HL7PARSER_SYNC_REMOTE}" == "1" ]]; then
      git -C "${HL7PARSER_DIR}" fetch --tags --prune origin
      git -C "${HL7PARSER_DIR}" pull --ff-only origin "$(git -C "${HL7PARSER_DIR}" rev-parse --abbrev-ref HEAD)"
    else
      echo -e "${YELLOW}Skipping HL7 parser remote sync (set HL7PARSER_SYNC_REMOTE=1 to fetch/pull).${NC}"
    fi
  else
    rm -rf "${HL7PARSER_DIR}"
    git clone --depth 1 "${HL7PARSER_REPO_URL}" "${HL7PARSER_DIR}"
  fi

  if [[ ! -f "${HL7PARSER_DIR}/include/hl7parser/parser.h" ]]; then
    echo -e "${RED}Error: HL7 parser headers not found at ${HL7PARSER_DIR}${NC}"
    exit 1
  fi
else
  echo -e "${YELLOW}Step 1C: HL7 parser routine disabled (set HL7PARSER_ENABLE=1 to enable).${NC}"
fi

# FastFHIR is resolved via Bazel local_path_override in MODULE.bazel.
# Bazel handles building FastFHIR from source as a module dependency.

# ============================================================================
# Step 2B: Build Google FHIR components in .external
# ============================================================================
if [[ "${GOOGLE_FHIR_ENABLE}" == "1" ]]; then
  echo -e "${YELLOW}Step 2B: Building Google FHIR components...${NC}"
  BAZELISK_BIN="$(ensure_bazelisk)"
  GOOGLE_FHIR_JAVA_HOME="$(ensure_google_fhir_java)"
  echo -e "${YELLOW}Using Google FHIR JDK: ${GOOGLE_FHIR_JAVA_HOME}${NC}"
  GOOGLE_FHIR_CLEANUP_ON_EXIT=1

  GOOGLE_FHIR_TARGETS=(
    "//cc/google/fhir:json_format"
    "//cc/google/fhir/r4:json_format"
    "//cc/google/fhir:libgoogle_fhir_bundled"
    "//proto/google/fhir/proto/r4/core/resources:patient_cc_proto"
  )
  if [[ "${GOOGLE_FHIR_BUILD_STATIC_CLOSURE}" == "1" ]]; then
    GOOGLE_FHIR_TARGETS+=(
      "@com_google_protobuf//:protobuf"
      "@com_google_absl//absl/...:all"
    )
  fi

  if [[ "${NEEDS_GOOGLE_FHIR_BUILD}" == "1" ]]; then
    mkdir -p "${GOOGLE_FHIR_BUILD}" "${GOOGLE_FHIR_OUTPUT_BASE}" "${GOOGLE_FHIR_REPOSITORY_CACHE}"

    GOOGLE_FHIR_BAZEL_FLAGS=(
      "--enable_bzlmod=false"
      "--repository_cache=${GOOGLE_FHIR_REPOSITORY_CACHE}"
      "--noshow_progress"
      "--announce_rc"
    )

    if [[ "${GOOGLE_FHIR_USE_SYSTEM_ZLIB}" == "1" ]]; then
      mkdir -p "${GOOGLE_FHIR_SYSTEM_ZLIB_REPO}"
      cat > "${GOOGLE_FHIR_SYSTEM_ZLIB_REPO}/WORKSPACE" <<'EOF'
workspace(name = "zlib")
EOF
      cat > "${GOOGLE_FHIR_SYSTEM_ZLIB_REPO}/BUILD.bazel" <<'EOF'
cc_library(
    name = "zlib",
    linkopts = ["-lz"],
    visibility = ["//visibility:public"],
)
EOF
      GOOGLE_FHIR_BAZEL_FLAGS+=("--override_repository=zlib=${GOOGLE_FHIR_SYSTEM_ZLIB_REPO}")
      echo -e "${YELLOW}Google FHIR: using system zlib via override_repository (${GOOGLE_FHIR_SYSTEM_ZLIB_REPO})${NC}"
    fi

    pushd "${GOOGLE_FHIR_DIR}" >/dev/null
    export USE_BAZEL_VERSION="${GOOGLE_FHIR_BAZEL_VERSION}"
    export JAVA_HOME="${GOOGLE_FHIR_JAVA_HOME}"
    export PATH="${JAVA_HOME}/bin:${PATH}"

    for target in "${GOOGLE_FHIR_TARGETS[@]}"; do
      echo -e "${YELLOW}Building Google FHIR target: ${target}${NC}"
      "${BAZELISK_BIN}" \
        --output_base="${GOOGLE_FHIR_OUTPUT_BASE}" \
        build "${target}" \
        "${GOOGLE_FHIR_BAZEL_FLAGS[@]}" \
        --compilation_mode=fastbuild \
        --cxxopt=-mmacosx-version-min=15.0 \
        --conlyopt=-mmacosx-version-min=15.0 \
        --jobs="${THREADS}"
    done

    popd >/dev/null
    echo "${GOOGLE_FHIR_BUILD_FINGERPRINT}" > "${GOOGLE_FHIR_STAMP}"

  else
    echo -e "${GREEN}Google FHIR build is up to date; skipping rebuild.${NC}"
  fi

  stage_google_fhir_runtime_artifacts

  if [[ "${TEST_GOOGLE_FHIR_COMPONENTS}" == "1" ]]; then
    echo -e "${YELLOW}Step 2C: Testing Google FHIR components individually...${NC}"
    pushd "${GOOGLE_FHIR_DIR}" >/dev/null
    export USE_BAZEL_VERSION="${GOOGLE_FHIR_BAZEL_VERSION}"
    export JAVA_HOME="${GOOGLE_FHIR_JAVA_HOME}"
    export PATH="${JAVA_HOME}/bin:${PATH}"

    for target in "${GOOGLE_FHIR_TARGETS[@]}"; do
      echo -e "${YELLOW}Verifying Google FHIR target: ${target}${NC}"
      "${BAZELISK_BIN}" \
        --output_base="${GOOGLE_FHIR_OUTPUT_BASE}" \
        cquery "${target}" \
        --enable_bzlmod=false \
        --repository_cache="${GOOGLE_FHIR_REPOSITORY_CACHE}" \
        --noshow_progress >/dev/null
    done

    popd >/dev/null
  fi

  if [[ "${GOOGLE_FHIR_CLEAN_ARTIFACTS}" == "1" ]]; then
    echo -e "${YELLOW}Step 2D: Aggressive Google FHIR artifact cleanup...${NC}"
    if [[ -d "${GOOGLE_FHIR_BUILD}" ]]; then
      du -sh "${GOOGLE_FHIR_BUILD}" || true
    fi

    rm -rf "${GOOGLE_FHIR_OUTPUT_BASE}" "${GOOGLE_FHIR_REPOSITORY_CACHE}"
    rm -rf "${GOOGLE_FHIR_DIR}/bazel-bin" "${GOOGLE_FHIR_DIR}/bazel-out" "${GOOGLE_FHIR_DIR}/bazel-testlogs"
    find "${GOOGLE_FHIR_DIR}" -maxdepth 1 -type l -name 'bazel-*' -delete 2>/dev/null || true
    rm -rf "${GOOGLE_FHIR_BUILD}"

    echo -e "${GREEN}Google FHIR cleanup complete. Retained source checkout + stamp only.${NC}"
  fi

  GOOGLE_FHIR_CLEANUP_ON_EXIT=0
else
  echo -e "${YELLOW}Step 2B: Google FHIR routine disabled (set GOOGLE_FHIR_ENABLE=1 to enable).${NC}"
fi

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
# Step 4: Build benchmark targets with Bazel
# ============================================================================
echo -e "${YELLOW}Step 4: Building benchmark harness with Bazel...${NC}"

BAZELISK_BIN="$(ensure_bazelisk)"
mkdir -p "${BENCH_BAZEL_OUTPUT_BASE}" "${BENCH_BAZEL_REPOSITORY_CACHE}"

if [[ "${FORCE_BENCH_REBUILD}" == "1" ]]; then
  rm -rf "${BENCH_BAZEL_OUTPUT_BASE}" "${BENCH_BAZEL_REPOSITORY_CACHE}"
  mkdir -p "${BENCH_BAZEL_OUTPUT_BASE}" "${BENCH_BAZEL_REPOSITORY_CACHE}"
fi

BENCH_BAZEL_FLAGS=(
  "--enable_bzlmod=false"
  "--output_base=${BENCH_BAZEL_OUTPUT_BASE}"
  "--repository_cache=${BENCH_BAZEL_REPOSITORY_CACHE}"
  "--noshow_progress"
)

pushd "${REPO_ROOT}" >/dev/null
export USE_BAZEL_VERSION="${GOOGLE_FHIR_BAZEL_VERSION}"

if ! "${BAZELISK_BIN}" build \
  "${BENCH_BAZEL_FLAGS[@]}" \
  --compilation_mode=opt \
  --jobs="${THREADS}" \
  //bench:bench_harness //bench:bench_timing_conformance; then
  echo -e "${RED}Benchmark Bazel build failed${NC}"
  popd >/dev/null
  exit 1
fi

if [[ "${TEST_BENCH_COMPONENTS}" == "1" ]]; then
  echo -e "${YELLOW}Step 4B: Testing benchmark components individually...${NC}"
  if ! "${BAZELISK_BIN}" test \
    "${BENCH_BAZEL_FLAGS[@]}" \
    --compilation_mode=opt \
    --test_output=errors \
    --jobs="${THREADS}" \
    //bench:timing_conformance_test; then
    echo -e "${RED}Benchmark Bazel tests failed${NC}"
    popd >/dev/null
    exit 1
  fi
fi

popd >/dev/null

echo -e "${GREEN}=== Setup Complete ===${NC}"
echo ""
echo "Run the benchmark:"
echo "  cd ${REPO_ROOT}"
echo "  ./bazel-bin/bench/bench_harness"
echo ""
echo "Run validation test:"
echo "  ./bazel-bin/bench/bench_timing_conformance"
