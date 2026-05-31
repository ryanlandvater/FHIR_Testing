# FastFHIR Benchmarking: Implementation & Validation Summary

**Status**: Four Arms Live; Google FHIR Stages 2/3 Stubbed; 2/28 Resources Tested

---

This document is a curated overview of the benchmark architecture, build status, and parity guidance.
For detailed per-file API documentation, see the [bench/*.md](bench/) files indexed in [README.md](README.md).

## Architecture (4 Arms x 4 Stages)

```
                BundleBenchFixture (identical POCOs to all arms)
               -------------------+------------------+------------------
               |                  |                  |                  |
          arm_fastfhir        arm_json          arm_hl7v2      arm_google_fhir
               |                  |                  |                  |
               +------------------+------------------+------------------+
               Shared bench_test_{1,2,3,4}.hpp
               (macro-guarded per arm)
```

| Stage | What It Measures | FastFHIR | JSON | HL7v2 | Google FHIR |
|---|---|---|---|---|---|
| Test 1 - Serialize | POCO struct to wire format | OK Arena build | OK nlohmann::json dump | OK ORU^R01 build | OK Protobuf TLV |
| Test 2 - Materialize | Wire format to in-memory tree | OK Parser tree walk | OK simdjson DOM walk | OK Batch parse count | STUBBED (0 ns) |
| Test 3 - Query | Tree walk for LOINC 2085-9 | OK Node reflection | OK simdjson path | OK Segment walking | STUBBED (0 ns) |
| Test 4 - Enrich | Append observation to bundle | OK Arena append | OK Parse-modify-dump | OK String concat | OK Re-serialize all |

## Code Parity: The Macro-Guarded Assignment Layer

Every arm's `assign_patient()` and `assign_observation()` live in the **same header** (`bench_test_1.hpp`), selected by `#define ARM_*`:

```cpp
// arm_fastfhir.cpp:
#define ARM_FASTFHIR
#include "bench_test_1.hpp"  // assign_patient to FastFHIR::ObjectHandle

// arm_json_fhir.cpp:
#define ARM_JSON
#include "bench_test_1.hpp"  // assign_patient to nlohmann::json&
```

This ensures **identical loop structure, field coverage, and query logic** across all arms.

**Per-file documentation:**

| Header | Doc | What It Contains |
|---|---|---|
| `bench_test_1.hpp` | [doc](bench/bench_test_1.hpp.md) | Field assignment - assign_patient(), assign_observation() |
| `bench_test_2.hpp` | [doc](bench/bench_test_2.hpp.md) | Materialization - materialize() + touch_tree() |
| `bench_test_3.hpp` | [doc](bench/bench_test_3.hpp.md) | Query - LOINC 2085-9 search, value type classification |
| `bench_test_4.hpp` | [doc](bench/bench_test_4.hpp.md) | Enrichment - append observation to existing bundle |

---

## Implementation Phases

### Phase 1: Foundation & Architecture Analysis

**Key Decision**: Treat native C++ structs (`PatientData`, `ObservationData`) as the canonical "EHR ground truth." Every arm serializes from the same in-memory POCO representation. This guarantees the benchmark measures **format serialization speed**, not data-access differences.

See: [harness.hpp.md](bench/harness.hpp.md) for type definitions and [bench_test_1.hpp.md](bench/bench_test_1.hpp.md) for the shared assignment layer.

### Phase 2: Synthea Integration

- 119 Synthea-generated FHIR JSON patient Bundles ingested via FastFHIR Ingestor
- Ingested into `BundlePatient` structs holding FFHR arenas + hydrated POCOs
- Semantic query focus: LOINC code `2085-9` (Total Cholesterol) within `Observation.code.coding`

See: [synthea_fixture.cpp.md](bench/synthea_fixture.cpp.md)

### Phase 3: Arm Implementation

| Arm | File | Doc | Build Status | Notes |
|---|---|---|---|---|
| FastFHIR | `arm_fastfhir.cpp` | [doc](bench/arm_fastfhir.cpp.md) | OK | Primary arm; arena-based serialization |
| JSON FHIR | `arm_json_fhir.cpp` | [doc](bench/arm_json_fhir.cpp.md) | OK | nlohmann::json serialize; simdjson read |
| HL7v2 | `arm_hl7v2.cpp` | [doc](bench/arm_hl7v2.cpp.md) | OK | ORU^R01 messages; zero external deps |
| Google FHIR | `arm_google_fhir.cpp` | [doc](bench/arm_google_fhir.cpp.md) | Stages 2/3 stubbed | DYNAMIC_BUNDLED dylib linkage |

### Phase 4: Build & Dependency Resolution

| Dependency | Mechanism | Status | Documentation |
|---|---|---|---|
| FastFHIR | `local_path_override` in MODULE.bazel | OK Bazel builds from `.external/FastFHIR` | [BUILD.bazel.md](bench/BUILD.bazel.md) |
| nlohmann/json | External dep (bazel_dep) | OK Bazel resolves automatically | Same |
| Google FHIR | Bazel-built dylib (DYNAMIC_BUNDLED) | macOS only; 65 MB dylib | [arm_google_fhir.cpp.md](bench/arm_google_fhir.cpp.md) |
| PostgreSQL (libpq) | System library | OK Optional; enables DB persistence | [main.cpp.md](bench/main.cpp.md) |

### Phase 5: Benchmark Execution

See [main.cpp.md](bench/main.cpp.md) for the full CLI reference, bundle sweep algorithm, and PostgreSQL schema.

Default run command:
```bash
bazel run //bench:bench_harness -- --runs 10
```

Conformance validation:
```bash
bazel test //bench:timing_conformance_test
```

---

## Remaining Parity Gaps

### P1: Google FHIR Stages 2 & 3 (Materialize + Query)

`arm_google_fhir.cpp` currently pushes zero-duration metrics for Stages 2 and 3. This means **no comparison data is generated** for Google FHIR's deserialization or query latency.

**Required**: Implement `test_2::materialize()` for Google FHIR's custom TLV record format (iterate records, `ParseFromArray()` each protobuf, walk via reflection counting nodes) and `test_3::query()` (iterate deserialized protobufs counting patients, LOINC matches, value types).

### P2: Resource Coverage: Only 2 of 28 Resources Tested

Only `Patient` and `Observation` are actively serialized/queried. Encounter, Condition, Procedure, and 24 other FHIR R4 resources available in FastFHIR are hydrated in `BundlePatient` structs but never enter the benchmark pipeline.

### P2: HL7v2 Birthdate Normalization

`hl7v2_message.hpp::normalize_birthdate()` strips non-digit characters from FHIR dates (`"1990-03-21"` to `"19900321"`). The conformance test accounts for this, but cross-arm string comparisons must use digit-only comparison.

### P2: Enrichment Cost Model Divergence

Each arm implements Test 4 (Enrich) with fundamentally different cost profiles:
- **FastFHIR**: Appends to arena + re-finalizes. No existing data re-parsed.
- **JSON**: Parses entire bundle with nlohmann::json, appends, re-serializes everything.
- **HL7v2**: String concatenation - cheapest path.
- **Google FHIR**: Re-parses every TLV record and re-serializes all.

### P3: R5 Dictionary Query Support

The benchmark uses `FHIR_VERSION_R5` builder but only R4 resources are tested.

## Critical Parity TODOs

| Priority | TODO | Where | Impact |
|---|---|---|---|
| P1 | Implement Google FHIR Stage 2 (Materialize) | bench_test_2.hpp | Stage 2 returns 0 ns for google_fhir |
| P1 | Implement Google FHIR Stage 3 (Query) | bench_test_3.hpp | Stage 3 returns 0 ns for google_fhir |
| P2 | Add Encounter, Condition, Procedure to serialization scope | bench_test_1.hpp | Only 2/28 resources benchmarked |
| P2 | Align HL7v2 birthdate output format or document normalization | hl7v2_message.hpp | Cross-arm query parity |
| P3 | R5 dictionary query support | arm_fastfhir.cpp | R5 coverage gap |

## Upstream FastFHIR Issues

Documented in [FFHRnotes.md](FFHRnotes.md):
- Missing installed transitive headers (generated headers not installed by build system)
- Ingestor symbols not exported in `libfastfhir`
- Install-interface include semantics need cleanup
- CODE field assignment type expectations underdocumented
