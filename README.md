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
- Synthea JSON is pre-converted to `.ffhr` with `ff_ingestor` (or `ff_ingest` fallback).
- Benchmark reads `.ffhr` files, creates in-memory patient bundles, serializes both arms, and writes results to stdout + optional PostgreSQL.
- Notebook reads from PostgreSQL for analysis.

## Setup

```bash
export FASTFHIR_REPO=https://github.com/<org>/FastFHIR.git
./generate_repo.sh
```

Optional:

```bash
export SYNTHEA_DATA_URL=https://github.com/synthetichealth/synthea-sample-data/archive/refs/heads/master.zip
```

## Run Benchmark

```bash
DYLD_LIBRARY_PATH=local/lib ./build/bench/bench/bench_harness
```

With PostgreSQL persistence:

```bash
DYLD_LIBRARY_PATH=local/lib ./build/bench/bench/bench_harness \
  --db "host=localhost port=5432 dbname=fhir_benchmark user=postgres password=postgres"
```

Output columns:

```text
arm,stage,duration_us,target_mb,patients_in_bundle
```

## Validate

```bash
DYLD_LIBRARY_PATH=local/lib ./build/bench/bench/bench_timing_conformance
```

## Analyze

Use `notebooks/benchmark_results.ipynb`.

## Notes

API limitations and fallback decisions are documented in `FFHRnotes.md`.
