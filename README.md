# FastFHIR Lightweight Benchmark

This repository is intentionally minimal. It benchmarks only four serialization paths from the same in-memory `PatientData`-based bundle model:

- `fastfhir`
- `json_fhir`
- `google_fhir`
- `HL7v2`

## Directives Implemented

- `generate_repo.sh` installs FastFHIR under `local/` from an external source first.
- Local `.external/FastFHIR` is used only as fallback when external source is not provided.
- Synthea JSON is downloaded automatically when missing.
- Benchmark ingests Synthea JSON files at runtime, creates in-memory bundle items `{Memory, PatientData}`, and writes results to stdout + optional PostgreSQL.
- Per ingested patient, `Memory::create()` is sized at `2x` source JSON file bytes.
- Notebook reads from PostgreSQL for analysis.

## Setup

```bash
export FASTFHIR_REPO=https://github.com/<org>/FastFHIR.git
./generate_repo.sh
```

Google FHIR routine is enabled by default and runs as a separate build flow in `.external/google-fhir`.
The script builds Google FHIR C++ components with Bazel, validates targets individually,
and fails fast if any target check fails.

Useful environment overrides:

```bash
# Google FHIR source and build controls
export GOOGLE_FHIR_ENABLE=1
export GOOGLE_FHIR_REPO=https://github.com/google/fhir.git
export GOOGLE_FHIR_SYNC_REMOTE=0
export FORCE_GOOGLE_FHIR_REBUILD=0

# Pin Bazel behavior for Google FHIR
export GOOGLE_FHIR_BAZEL_VERSION=7.7.1
export GOOGLE_FHIR_BAZELISK_VERSION=v1.22.1

# Component checks
export TEST_GOOGLE_FHIR_COMPONENTS=1
export TEST_BENCH_COMPONENTS=1

# Aggressive cleanup of Google build artifacts after successful checks
export GOOGLE_FHIR_CLEAN_ARTIFACTS=1
```

Set `GOOGLE_FHIR_ENABLE=0` only when you intentionally want to skip the Google FHIR routine.

Optional:

```bash
export SYNTHEA_DATA_URL=https://github.com/synthetichealth/synthea-sample-data/archive/refs/heads/master.zip
```

## Run Benchmark

```bash
DYLD_LIBRARY_PATH=local/lib ./bazel-bin/bench/bench_harness
```

With PostgreSQL persistence:

```bash
DYLD_LIBRARY_PATH=local/lib ./bazel-bin/bench/bench_harness \
  --db "host=localhost port=5432 dbname=fhir_benchmark user=postgres password=postgres"
```

Output columns:

```text
arm,stage,duration_ns,target_mb,patients_in_bundle
```

## Validate

```bash
DYLD_LIBRARY_PATH=local/lib ./bazel-bin/bench/bench_timing_conformance
```

Or run as a Bazel test:

```bash
bazel test //bench:timing_conformance_test
```

## Analyze

Use `notebooks/benchmark_results.ipynb`.

## Notes

API limitations and fallback decisions are documented in `FFHRnotes.md`.

## Warning: FastFHIR Ingest Consumer Build Contract

If `ff_ingest` works on a Synthea JSON file but `bench_harness` fails with `simdjson Exception: CAPACITY`, do not assume the file is too large and do not patch around it by calling `ff_ingest` from the benchmark.

The benchmark consumer must compile with the same required simdjson configuration as the working FastFHIR ingest toolchain. In this workspace the decisive fix was:

```cmake
target_compile_definitions(bench_core INTERFACE SIMDJSON_THREADS_ENABLED=1)
```

What future agents should not do first:

- Do not add an `ff_ingest` fallback inside benchmark code.
- Do not treat `-fPIC` or `-fPIE` as the root cause without proving it.
- Do not assume matching `libfastfhir_ingestor.dylib` paths alone is sufficient.

Use `FFHRnotes.md` as the authoritative incident log for this failure mode.
