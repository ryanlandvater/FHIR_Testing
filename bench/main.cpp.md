# `main.cpp` — Benchmark Harness Entry Point

> ✅ **Builds and runs 2026-08-25.** Two changes worth knowing:
>
> - **`--seed N`** fixes bundle composition (default `20260825`; `--seed 0`
>   restores random). It was previously seeded from `std::random_device`, so two
>   runs of the same command measured different data — which made a corruption
>   bug reproduce only intermittently. See [notes.md](../notes.md) §5.
> - **PostgreSQL is opt-in.** The default `//bench:bench_harness` links no libpq
>   and warns if `--db` is passed. Build `//bench:bench_harness_pg` for `--db`.
>
> ⚠️ `main.cpp:31` still prefers a hardcoded developer path for the corpus —
> tracked in [TASKS.md § INFRA](../TASKS.md).

## Purpose

The **main executable** of the benchmark harness. It discovers Synthea JSON files, builds `BundleBenchFixture` instances at various target sizes, runs all four arms (FastFHIR, JSON, HL7v2, Google FHIR) with configurable iterations and warmup, and writes results to stdout (CSV) and optionally to PostgreSQL.

## Flow Diagram

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Parse CLI args (--iterations, --warmup, --runs, --db)    │
│ 2. Locate Synthea data directory (platform-specific)         │
│ 3. Ingest all patient JSON files → vector<BundlePatient>     │
│ 4. For each target_size in [1MB, 2MB, 4MB, …, 256MB]:       │
│    a. Random-sample patients to reach target size            │
│    b. Clone sampled patients into BundleBenchFixture         │
│    c. Warmup (default: 1 run of all arms, results discarded) │
│    d. Run arms × iterations (default: 10 runs)               │
│    e. Record metrics to stdout + PostgreSQL                   │
│ 5. Print per-arm aggregate stats                             │
└─────────────────────────────────────────────────────────────┘
```

## Command-Line Interface

| Argument | Default | Description |
|---|---|---|
| `--iterations N` | 1 | Number of measurement iterations per run |
| `--warmup-iterations N` | 1 | Warmup rounds (0 to disable) |
| `--runs N` | 10 | Bundle build + arm execution repetitions |
| `--bundle-max-mb N` | 256 | Cap on largest bundle target (MB) |
| `--bundle-max-mb-explicit` | off | When set, only the specified max is used (no sweep) |
| `--fastfhir-vma-mb N` | auto | FastFHIR VMA footprint in MB (for pre-allocation) |
| `--db connstr` | none | PostgreSQL connection string |

## Key Implementation Details

### Synthea Data Discovery

On macOS: checks a hardcoded primary path (`/Users/RyanLandvater/.../datasets/synthea`) then falls back to `./datasets/synthea`. On other platforms: checks `./datasets/synthea` only. All `.json` files in the directory are ingested via `make_bundle_patient_from_json()`.

### Bundle Size Sweep

Default target sizes: 1, 2, 4, 8, 16, 32, 64, 256 MB. For each target size:

1. **Random sampling**: Draws random patients from the ingested pool until `sum(memory.capacity()) >= target_size`.
2. **Cloning**: Each sampled patient is deep-copied via `clone_bundle_patient()` to create independent fixtures.
3. **Warmup**: Runs all available arms once (results discarded) to warm caches/pipelines.
4. **Benchmark loop**: For each of `N` runs:
   - Runs FastFHIR arm → collects metrics
   - Runs JSON arm → collects metrics
   - Runs HL7v2 arm (if available) → collects metrics
   - Runs Google FHIR arm (if available) → collects metrics
5. **Cross-arm validation**: If Google FHIR is enabled, validates message-surface parity between all arms.

### PostgreSQL Persistence

When `--db` is provided:

1. Connects to PostgreSQL
2. Creates/verifies schema columns (`duration_ns`, `duration_us`)
3. Inserts a `benchmark_runs` row with host label and iteration count
4. For each metric event, inserts a row into `benchmark_results` via `PQexecParams` (parameterized query)

### Platform Dispatch

| Function | macOS | Windows | Linux |
|---|---|---|---|
| `run_fastfhir_bundle` | ✓ | ✓ | ✓ |
| `run_json_bundle` | ✓ | ✓ | ✓ |
| `run_hl7v2_bundle` | ✓ | ✓ | ✓ (via `HAVE_HL7V2`) |
| `run_google_fhir_bundle` | ✓ | ✗ | ✗ (via `HAVE_GOOGLE_FHIR`) |

Google FHIR and HL7v2 arms are conditionally compiled via `HAVE_GOOGLE_FHIR` and `HAVE_HL7V2` macros.

### Cross-Arm Validation (macOS Only)

When Google FHIR is available, `validate_parity()` is called after each run to verify that FastFHIR, JSON, and HL7v2 queried the same patient birthdates and LOINC code matches. Failures are printed to stderr but do not abort the benchmark.

## Output Format

### CSV Header (stdout)
```
arm,test,duration_ns,target_mb,patients_in_bundle
```

### CSV Rows
```
fastfhir,test_1_serialize,<ns>,<MB>,<N>
json_fhir,test_1_serialize,<ns>,<MB>,<N>
...
```

### Database Schema
```sql
benchmark_results (run_id, arm, stage, duration_ns, duration_us, target_mb, patients_in_bundle)
benchmark_runs (id, hostname, iterations, notes)
```

## Dependencies

- `harness.hpp` — All arm function declarations and types
- `libpq-fe.h` — PostgreSQL client (optional, via `HAVE_LIBPQ`)
- Standard library: `<algorithm>`, `<cstdlib>`, `<filesystem>`, `<fstream>`, `<iostream>`, `<random>`, `<sstream>`

## Error Handling

- Missing Synthea directory → prints error, exits 1
- Empty ingest result → prints error, exits 1
- Database connection failure → runs without DB (prints error to stderr)
- Individual patient JSON parse failure → skips file (prints warning to stderr)
