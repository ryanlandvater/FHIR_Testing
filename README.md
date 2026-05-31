# FastFHIR Lightweight Benchmark

Four-arm comparative benchmark measuring serialization, materialization, query, and enrichment performance across **FastFHIR**, **JSON FHIR** (nlohmann::json + simdjson), **Google FHIR** (protobuf), and **HL7v2** (ORU^R01).

## Architecture

```
Synthea JSON files (119 patients)
        │
        ▼
  synthea_fixture.cpp ── FastFHIR Ingest ──► BundlePatient (POCO + arena)
        │
        ▼
  BundleBenchFixture ── identical C++ structs fed to all 4 arms
        │
   ┌────┼────┬────┐
   ▼    ▼    ▼    ▼
 arm_   arm_ arm_ arm_
 fastfhir.json hl7v2 google_
 .cpp  .cpp .cpp fhir.cpp
  │     │     │     │
  ▼     ▼     ▼     ▼
  bench_test_1.hpp — shared assign_patient() / assign_observation()
  bench_test_2.hpp — shared materialize() per format
  bench_test_3.hpp — shared query() per format
  bench_test_4.hpp — shared enrich() per format
  │     │     │     │
  ▼     ▼     ▼     ▼
  4× MetricEvent per arm → stdout CSV + optional PostgreSQL
```

Each arm receives the **exact same in-memory `PatientData`/`ObservationData` structs** and serializes them into its respective wire format. See the per-file docs:

| Doc File | What It Describes |
|---|---|
| [`arm_fastfhir.cpp.md`](bench/arm_fastfhir.cpp.md) | FastFHIR binary arena serialization — the primary arm |
| [`arm_json_fhir.cpp.md`](bench/arm_json_fhir.cpp.md) | nlohmann::json → FHIR JSON bundle |
| [`arm_hl7v2.cpp.md`](bench/arm_hl7v2.cpp.md) | HL7v2 ORU^R01 pipe-delimited messages |
| [`arm_google_fhir.cpp.md`](bench/arm_google_fhir.cpp.md) | Google protobuf FHIR (Stage 1 live; Stages 2/3 stubbed) |
| [`harness.hpp.md`](bench/harness.hpp.md) | Core types, Timer, Stage enum, ArmRunResult |
| [`main.cpp.md`](bench/main.cpp.md) | CLI args, Synthea discovery, bundle sweep, DB persistence |
| [`synthea_fixture.cpp.md`](bench/synthea_fixture.cpp.md) | JSON → BundlePatient ingestion pipeline |
| [`bench_test_1.hpp.md`](bench/bench_test_1.hpp.md) | Shared assignment layer (the fairness guarantee) |
| [`bench_test_2.hpp.md`](bench/bench_test_2.hpp.md) | Materialization: parse wire format → tree |
| [`bench_test_3.hpp.md`](bench/bench_test_3.hpp.md) | Query: LOINC 2085-9 search + value classification |
| [`bench_test_4.hpp.md`](bench/bench_test_4.hpp.md) | Enrichment: append observation to existing bundle |
| [`hl7v2_message.hpp.md`](bench/hl7v2_message.hpp.md) | HL7v2 segment types, builder, batch parser |
| [`timing_conformance_test.cpp.md`](bench/timing_conformance_test.cpp.md) | Build-time parity validation |
| [`enrich.json.md`](bench/enrich.json.md) | BMP panel test fixture (LOINC 24321-3) |
| [`BUILD.bazel.md`](bench/BUILD.bazel.md) | Bazel target graph and conditional arms |

---

## Code Parity: The Fundamental Requirement

This benchmark's validity depends on one axiom: **every arm serializes bit-identical source data through structurally identical assignment code.** Any deviation — a missing field, a different code path, a skipped edge case — invalidates the comparison.

### How Parity Is Enforced

The entire assignment layer lives in a **single header per test stage**, guarded by `#define ARM_*` macros:

```cpp
// arm_fastfhir.cpp:
#define ARM_FASTFHIR
#include "bench_test_1.hpp"   // assign_patient() → FastFHIR ObjectHandle
#include "bench_test_2.hpp"   // materialize()    → FastFHIR::Parser tree
#include "bench_test_3.hpp"   // query()          → FFHR node walk
#include "bench_test_4.hpp"   // enrich()         → FFHR arena append
#undef ARM_FASTFHIR

// arm_json_fhir.cpp:
#define ARM_JSON
#include "bench_test_1.hpp"   // assign_patient() → nlohmann::json
#include "bench_test_2.hpp"   // materialize()    → simdjson DOM
#include "bench_test_3.hpp"   // query()          → simdjson walk
#include "bench_test_4.hpp"   // enrich()         → JSON parse→modify→dump
#undef ARM_JSON
```

Inside each `bench_test_N.hpp`, `#if defined(ARM_FASTFHIR) / #elif defined(ARM_JSON) / #elif defined(ARM_HL7V2) / #elif defined(ARM_GOOGLE_FHIR)` blocks expose the format-specific implementation of the *same logical operation*. The call sites in the arm files are identical — only the backend changes.

This pattern guarantees:
- **Same loop structure** — no arm gets an algorithmic advantage
- **Same field coverage** — every field assigned by one arm is assigned by all arms
- **Same query logic** — LOINC matching, value type classification, component traversal are line-for-line equivalent
- **One audit point** — add a field to the shared assignment, and all four arms get it simultaneously

### Critical Open TODOs for Full Parity

| # | TODO | Impact | Effort |
|---|---|---|---|
| 1 | **Google FHIR Stage 2 (Materialize)** — currently stubs at 0 ns. Must parse the TLV record format and walk protobufs via reflection, matching `touch_tree()` in the other arms. | 🔴 Stage 2 results meaningless for google_fhir | ~40h |
| 2 | **Google FHIR Stage 3 (Query)** — currently stubs at 0 ns. Must iterate deserialized protobufs counting patients, LOINC 2085-9 matches, value types, components. | 🔴 Stage 3 results meaningless for google_fhir | ~40h |
| 3 | **Google FHIR Stage 4 (Enrich)** — functional but re-parses *all* records. Verify parity with other arms' enrich semantics (FastFHIR appends without re-parse; JSON re-parses all). | 🟡 Enrich cost model differs fundamentally | ~8h doc |
| 4 | **HL7v2 birthdate normalization mismatch** — `normalize_birthdate()` strips non-digits (`1990-03-21` → `19900321`). All other arms preserve ISO format. The conformance checker accounts for this, but the serialization output differs. | 🟡 Cross-arm query parity | ~2h fix |
| 5 | **Only 2/28 FHIR R4 resources tested** — Patient + Observation are active. Encounter, Condition, Procedure, and 24 others are hydated in fixture structs but never serialized. The benchmark measures a tiny fraction of real-world EHR workload. | 🟡 Generalizability | ~200h per phase |
| 6 | **No multi-bundle enrichment test** — Test 4 enriches once. Real EHRs enrich millions of times. The one-shot cost profile may not extrapolate. | 🟡 Realism | ~4h config |

---

## Setup

```bash
export FASTFHIR_REPO=https://github.com/<org>/FastFHIR.git
./generate_repo.sh
```

Google FHIR routine is enabled by default (separate Bazel build in `.external/google-fhir`).
Useful environment overrides:

```bash
# Google FHIR controls
export GOOGLE_FHIR_ENABLE=1
export GOOGLE_FHIR_REPO=https://github.com/google/fhir.git
export GOOGLE_FHIR_SYNC_REMOTE=0
export FORCE_GOOGLE_FHIR_REBUILD=0
export GOOGLE_FHIR_BAZEL_VERSION=7.7.1
export GOOGLE_FHIR_BAZELISK_VERSION=v1.22.1
export TEST_GOOGLE_FHIR_COMPONENTS=1
export TEST_BENCH_COMPONENTS=1
export GOOGLE_FHIR_CLEAN_ARTIFACTS=1

# Data source
export SYNTHEA_DATA_URL=https://github.com/synthetichealth/synthea-sample-data/archive/refs/heads/master.zip
```

Set `GOOGLE_FHIR_ENABLE=0` to skip the Google FHIR routine entirely.

---

## Build

```bash
bazel build //bench:bench_harness
```

## Run Benchmark

```bash
./bazel-bin/bench/bench_harness
```

With PostgreSQL persistence:

```bash
./bazel-bin/bench/bench_harness \
  --db "host=localhost port=5432 dbname=fhir_benchmark user=postgres password=postgres"
```

### CLI Arguments

| Argument | Default | Description |
|---|---|---|
| `--iterations N` | 1 | Measurement iterations per bundle run |
| `--warmup-iterations N` | 1 | Warmup rounds (0 to disable) |
| `--runs N` | 10 | Number of bundle build + arm execution repetitions |
| `--bundle-max-mb N` | 256 | Cap on largest target bundle size (MB) |
| `--bundle-max-mb-explicit` | off | If set, only the specified max is used (no sweep) |
| `--db connstr` | none | PostgreSQL connection string |

### Output Columns (stdout CSV)

```text
arm,stage,duration_ns,target_mb,patients_in_bundle
```

Example:
```
fastfhir,test_1_serialize,452831,1,3
json_fhir,test_1_serialize,891234,1,3
```

---

## Validate

```bash
# Bazel test
bazel test //bench:timing_conformance_test

# Direct execution
./bazel-bin/bench/bench_timing_conformance
```

The conformance test verifies:
1. All metrics have positive duration values
2. FastFHIR and JSON arms both report exactly `patients=1`
3. Both arms report matching birthdate values

---

## Analyze

Use `notebooks/benchmark_results.ipynb` to read from PostgreSQL.

---

## Known Issues & Incident Log

| Issue | Document | Summary |
|---|---|---|
| FastFHIR ingest CAPACITY errors | [`FFHRnotes.md`](FFHRnotes.md) | `SIMDJSON_THREADS_ENABLED=1` must match between ff_ingest and benchmark consumer |
| Missing installed headers | [`FFHRnotes.md`](FFHRnotes.md) | Transitive public API headers not installed by FastFHIR build |
| Ingestor symbols not exported | [`FFHRnotes.md`](FFHRnotes.md) | `libfastfhir` doesn't export `Ingestor` symbols; use struct-based Builder |
| Google FHIR static link failure | [`arm_google_fhir.cpp.md`](bench/arm_google_fhir.cpp.md) | Static archives cause undefined symbol explosions from absl/protobuf/utf8_range |
| Google FHIR Stages 2/3 stubbed | [`MESSAGE_SURFACE_PARITY_AUDIT.md`](MESSAGE_SURFACE_PARITY_AUDIT.md) | Cannot compare deserialization or query performance against FastFHIR |
| Resource coverage gap | [`RESOURCE_COVERAGE_ANALYSIS.md`](RESOURCE_COVERAGE_ANALYSIS.md) | Only 2/28 FHIR resources tested |
