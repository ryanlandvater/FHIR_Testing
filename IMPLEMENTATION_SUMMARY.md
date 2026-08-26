# FastFHIR Benchmarking: Implementation & Validation Summary

**Status**: ✅ Builds and runs (ported 2026-08-25) · Results not publishable · 2/37 Resources Tested

---

> **Read [notes.md](notes.md) first.** The harness was ported to FastFHIR's
> `FF_*` façade API on 2026-08-25 and builds and runs again. Doing so exposed
> five defects that had been live and silent — including an ODR violation across
> the four arms and a Test 2 walk that visited 1 node instead of ~8,000.
>
> The architecture below is accurate as *design*. The numbers the harness now
> produces are proof it works end to end, **not results to publish**: Test 1 is
> not at parity, `value[x]` is excluded from every arm, and Test 3 still
> charges the FastFHIR arm a `print_json` penalty (PA-7). Test 2 is the
> random-access stage (D4, 2026-08-26).

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
| Test 2 - Random Access | N random entry reads from the root (replaced materialize, D4) | Offset arithmetic | simdjson at(i) scan | MSH scan | TLV scan + parse |
| Test 3 - Query | Tree walk for LOINC 2085-9 | Node reflection | simdjson path | Segment walking | Proto field walk |
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

This was intended to ensure **identical loop structure, field coverage, and
query logic** across all arms.

⚠️ **As of 2026-08-25 the FastFHIR arm's Test 1 no longer uses this layer** — it
serializes the whole POCO via `append_obj`, because the public API cannot write
FastFHIR's inline-block arrays field-by-field. It therefore covers *more* fields
than the other three arms. The headers also required a per-arm inline namespace
to avoid an ODR violation. See [notes.md](notes.md) §1 and §3.

**Per-file documentation:**

| Header | Doc | What It Contains |
|---|---|---|
| `bench_test_1.hpp` | [doc](bench/bench_test_1.hpp.md) | Field assignment - assign_patient(), assign_observation() |
| `bench_test_2.hpp` | [doc](bench/bench_test_2.hpp.md) | Random access - random_access() per arm (replaced materialize, D4) |
| `bench_test_3.hpp` | [doc](bench/bench_test_3.hpp.md) | Query - LOINC 2085-9 search, value type classification |
| `bench_test_4.hpp` | [doc](bench/bench_test_4.hpp.md) | Enrichment - append observation to existing bundle |

---

## Implementation Phases

### Phase 1: Foundation & Architecture Analysis

**Key Decision**: Treat native C++ structs (`PatientData`, `ObservationData`) as the canonical "EHR ground truth." Every arm serializes from the same in-memory POCO representation. This guarantees the benchmark measures **format serialization speed**, not data-access differences.

See: [harness.hpp.md](bench/harness.hpp.md) for type definitions and [bench_test_1.hpp.md](bench/bench_test_1.hpp.md) for the shared assignment layer.

### Phase 2: Synthea Integration

- Synthea FHIR JSON patient Bundles ingested via FastFHIR Ingestor. The corpus
  in use holds 342 files, of which **336 yield a Patient**; the other six are
  `hospitalInformation*` / `practitionerInformation*` bundles and are skipped.
- Ingested into `BundlePatient` structs holding FFHR arenas + hydrated POCOs
- Semantic query focus: LOINC code `2085-9` (Total Cholesterol) within `Observation.code.coding`

See: [synthea_fixture.cpp.md](bench/synthea_fixture.cpp.md)

### Phase 3: Arm Implementation

Build status verified 2026-08-25 with `bazel build -c opt --keep_going //bench:all`:

| Arm | File | Doc | Build Status | Notes |
|---|---|---|---|---|
| FastFHIR | `arm_fastfhir.cpp` | [doc](bench/arm_fastfhir.cpp.md) | ⚠️ builds; Test 1 bypasses the shared layer | Primary arm; arena-based serialization |
| JSON FHIR | `arm_json_fhir.cpp` | [doc](bench/arm_json_fhir.cpp.md) | ✅ builds | nlohmann::json serialize; simdjson read |
| HL7v2 | `arm_hl7v2.cpp` | [doc](bench/arm_hl7v2.cpp.md) | ✅ builds | ORU^R01 messages; zero external deps |
| Google FHIR | `arm_google_fhir.cpp` | [doc](bench/arm_google_fhir.cpp.md) | ✅ builds; all 4 stages live | DYNAMIC_BUNDLED dylib linkage |

Worth remembering from the port: the JSON and HL7v2 arms broke too, despite
having no FastFHIR serialization path of their own. `harness.hpp` uses
FastFHIR's generated POCOs and code-system enums as the shared ground-truth
types, so an upstream rename reaches every arm.

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
bazel test -c opt //bench:timing_conformance_test
```

Measured node counts per arm on a 1 MB bundle (Test 2), for calibration:
fastfhir 4,443 · json_fhir 8,327 · hl7v2 8,008 · google_fhir 9,539. These are
**not** normalized across formats — see [notes.md](notes.md) §2.

---

## Remaining Parity Gaps

### ~~P1: Google FHIR Stages 2 & 3~~ — RESOLVED (verified 2026-08-25)

**This gap does not exist.** `arm_google_fhir.cpp` pushes no zero-duration
metrics. `test_2::materialize()` parses the TLV records, runs `ParseFromArray()`
per message and walks via protobuf reflection (9,539 nodes, ~1.6 ms on a 1 MB
bundle). `test_3::query()` is implemented with 42 accumulator calls and returns
`patients=1 observations=316 obs_issued_present=316`, matching the JSON
baseline. *(The materialize stage itself was retired 2026-08-26 — TASKS.md D4;
Test 2 is random access now. The verification record above is retained as
history.)*

**Two real Google-arm gaps remain instead:**
- `birthdate` is a microsecond epoch (`194140800000000`) where every other arm
  reports ISO (`1976-02-26`).
- The Google arm is excluded from `validate_parity()`, so its results are never
  cross-checked against another arm.

### P2: Resource Coverage: Only 2 of 37 Resources Tested

Only `Patient` and `Observation` are actively serialized/queried. Encounter, Condition, Procedure, and 32 other FHIR resources compiled into FastFHIR at the current profile are hydrated in `BundlePatient` structs but never enter the benchmark pipeline.

The denominator is **profile-dependent**: 37 is the count for
`us-core,billing,medication-admin,supply` as of 2026-08-24. Resource types
outside the compiled profile are not absent from the stream — they are
retained as opaque JSON, which round-trips losslessly but is **not
typed-navigable**. See [README § Result provenance](README.md#result-provenance).

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
| **P0** | **Repair parity exposed by the port** | [TASKS.md § PARITY](TASKS.md) | **Test 1 not at parity; `value[x]` excluded; Test 2 unnormalized** |
| P1 | Normalize the Google arm's `birthdate` (microsecond epoch vs ISO) | bench_test_3.hpp | Cross-arm query parity |
| P1 | Include the Google arm in `validate_parity()` | main.cpp | Its results are never cross-checked |
| P2 | Add Encounter, Condition, Procedure to serialization scope | bench_test_1.hpp | Only 2/37 resources benchmarked |
| P2 | Decide the query corpus given opaque resources | [TASKS.md § CORPUS](TASKS.md) | Stage 3 silently skips 1,444 `ImagingStudy` records |
| P2 | Record profile + upstream SHA with every result | [TASKS.md § PROFILE](TASKS.md) | Results are not reproducible without it |
| P2 | Align HL7v2 birthdate output format or document normalization | hl7v2_message.hpp | Cross-arm query parity |
| P3 | R5 dictionary query support | arm_fastfhir.cpp | R5 coverage gap |

## Upstream FastFHIR Issues

Reviewed 2026-08-24 against `a9fd4e9` — see [FFHRnotes.md](FFHRnotes.md) for
the full audit.

**Still open:**
- CODE field assignment type expectations underdocumented — now a hard compile
  error (`TypeTraits<std::string>` is undefined), and the reason this repo
  hand-rolled a wire encoding that has since gone invalid.

**Resolved upstream:** missing installed headers (moot under the Bazel module),
Ingestor symbol export (misdiagnosed — separate target), install-interface
include semantics, `Fields`/`FieldKeys` naming, and compile-tested examples.
