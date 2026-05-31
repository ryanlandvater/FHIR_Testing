# FastFHIR Four-Arm Benchmark — Implementation Checklist

**Status**: ✅ Four arms live; 2 parity gaps remain

For detailed per-file architecture documentation, see the [bench/*.md](bench/) files indexed in [README.md](README.md).

---

## Active Implementation Status

| Component | Status | Docs | Key Details |
|---|---|---|---|
| harness.hpp (types) | ✅ Complete | [harness.hpp.md](bench/harness.hpp.md) | BundlePatient, BundleBenchFixture, ArmRunResult, Timer |
| synthea_fixture.cpp | ✅ Complete | [synthea_fixture.cpp.md](bench/synthea_fixture.cpp.md) | JSON to BundlePatient via FastFHIR Ingestor |
| arm_fastfhir.cpp | ✅ Complete | [arm_fastfhir.cpp.md](bench/arm_fastfhir.cpp.md) | All 4 stages implemented |
| arm_json_fhir.cpp | ✅ Complete | [arm_json_fhir.cpp.md](bench/arm_json_fhir.cpp.md) | nlohmann::json + simdjson |
| arm_hl7v2.cpp | ✅ Complete | [arm_hl7v2.cpp.md](bench/arm_hl7v2.cpp.md) | Custom hl7v2_message.hpp; zero external deps |
| arm_google_fhir.cpp | ⚠️ Staged | [arm_google_fhir.cpp.md](bench/arm_google_fhir.cpp.md) | S1 live; S2/S3 stubbed |
| main.cpp (entry point) | ✅ Complete | [main.cpp.md](bench/main.cpp.md) | CLI, bundle sweep, DB persistence |
| timing_conformance_test.cpp | ✅ Complete | [timing_conformance_test.cpp.md](bench/timing_conformance_test.cpp.md) | FFHR+JSON parity validation |
| bench_test_1.hpp (assign) | ✅ 4-arm | [bench_test_1.hpp.md](bench/bench_test_1.hpp.md) | Macro-guarded; identical field coverage |
| bench_test_2.hpp (materialize) | ⚠️ Partial | [bench_test_2.hpp.md](bench/bench_test_2.hpp.md) | Google stub (0 ns) |
| bench_test_3.hpp (query) | ⚠️ Partial | [bench_test_3.hpp.md](bench/bench_test_3.hpp.md) | Google stub (0 ns) |
| bench_test_4.hpp (enrich) | ✅ 4-arm | [bench_test_4.hpp.md](bench/bench_test_4.hpp.md) | All 4 arms enrich functionally |

## Critical Open Items

### P1 - Google FHIR Stages 2/3 (Must Fix)
- [ ] Implement test_2::materialize() for Google FHIR: iterate TLV records, deserialize protos
- [ ] Implement test_3::query() for Google FHIR: count patients, LOINC matches, value types

### P2 - Resource Coverage (Should Fix)
- [ ] Add Encounter, Condition, Procedure to serialization scope (hydrated but not assigned)
- [ ] Expand assign_patient() / assign_observation() for these resources across all 4 arms
- [ ] Add corresponding query logic in test_3 per arm
- [ ] Align HL7v2 birthdate format or document normalization difference

### P3 - Enrichment & Scale (Nice to Have)
- [ ] Add multi-resource enrichment tests (Encounter, Condition, Procedure)
- [ ] Multi-bundle enrichment test (enrich repeatedly, not just once)

## Parity Architecture

See the **Code Parity** sections in README.md and IMPLEMENTATION_SUMMARY.md for the macro-guarded bench_test_N.hpp design. This is the benchmark's single most important structural guarantee: each arm receives structurally identical assignment code, loop structure, and query logic. Any field added to the shared assignment layer is automatically included in all four arms.

## Build Targets

| Target | System | Status | Doc |
|---|---|---|---|
| bench_harness (all 4 arms) | Bazel | macOS | [BUILD.bazel.md](bench/BUILD.bazel.md) |
| bench_harness_win (no Google FHIR) | Bazel | Windows | [BUILD.bazel.md](bench/BUILD.bazel.md) |
| bench_timing_conformance | Bazel |  | [timing_conformance_test.cpp.md](bench/timing_conformance_test.cpp.md) |