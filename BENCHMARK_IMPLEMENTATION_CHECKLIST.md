# FastFHIR Four-Arm Benchmark — Implementation Checklist

**Status**: ✅ **Builds and runs** against upstream FastFHIR `a9fd4e9`
(ported 2026-08-25). Results are not yet publishable.

> `bazel build -c opt //bench:all` is green and `//bench:timing_conformance_test`
> passes. Exit code 2 from the harness is a cross-arm **parity mismatch**, not a
> crash.
>
> The port exposed several defects that had been live and silent — an ODR
> violation across the arms, a Test 2 walk that visited 1 node instead of 8,000,
> and array writes that produced invalid streams. **Read
> [notes.md](notes.md)** before quoting any number or designing new tests.

For detailed per-file architecture documentation, see the [bench/*.md](bench/) files indexed in [README.md](README.md).

---

## Active Implementation Status

| Component | Design | Builds | Docs | Key Details |
|---|---|---|---|---|
| harness.hpp (types) | ✅ Complete | ✅ builds | [harness.hpp.md](bench/harness.hpp.md) | BundlePatient, BundleBenchFixture, ArmRunResult, Timer. Pulls FastFHIR generated POCOs, so upstream renames propagate into every arm. |
| synthea_fixture.cpp | ✅ Complete | ✅ builds | [synthea_fixture.cpp.md](bench/synthea_fixture.cpp.md) | JSON to BundlePatient via FastFHIR Ingestor |
| arm_fastfhir.cpp | ✅ Complete | ⚠️ builds; Test 1 bypasses the shared assignment layer | [arm_fastfhir.cpp.md](bench/arm_fastfhir.cpp.md) | All 4 stages implemented |
| arm_json_fhir.cpp | ✅ Complete | ✅ builds | [arm_json_fhir.cpp.md](bench/arm_json_fhir.cpp.md) | nlohmann::json + simdjson |
| arm_hl7v2.cpp | ✅ Complete | ✅ builds; missing obs_issued / component values | [arm_hl7v2.cpp.md](bench/arm_hl7v2.cpp.md) | Custom hl7v2_message.hpp; zero external deps |
| arm_google_fhir.cpp | ✅ Complete | ✅ builds | [arm_google_fhir.cpp.md](bench/arm_google_fhir.cpp.md) | All 4 stages live (S2/S3 were **not** stubbed — verified 2026-08-25) |
| main.cpp (entry point) | ✅ Complete | ✅ builds | [main.cpp.md](bench/main.cpp.md) | CLI, bundle sweep, DB persistence. Hardcoded corpus path — TASKS.md § INFRA. |
| timing_conformance_test.cpp | ✅ Complete | ✅ **PASSES** | [timing_conformance_test.cpp.md](bench/timing_conformance_test.cpp.md) | FFHR+JSON parity validation |
| read_path_bench.cpp | ✅ Complete | ✅ **builds** | [read_path_bench.cpp.md](bench/read_path_bench.cpp.md) | Written against the current API — **the port template** |
| bench_test_1.hpp (assign) | ✅ 4-arm | ✅ builds; `value[x]` skipped in all arms | [bench_test_1.hpp.md](bench/bench_test_1.hpp.md) | Macro-guarded; identical field coverage |
| bench_test_2.hpp (random access) | ✅ 4-arm | ✅ replaced materialize (D4); cross-arm byte gate enforces identical reads | [bench_test_2.hpp.md](bench/bench_test_2.hpp.md) | All arms implemented |
| bench_test_3.hpp (query) | ✅ 4-arm | ✅ builds; FastFHIR pays a print_json penalty | [bench_test_3.hpp.md](bench/bench_test_3.hpp.md) | All arms implemented |
| bench_test_4.hpp (enrich) | ✅ 4-arm | ✅ builds | [bench_test_4.hpp.md](bench/bench_test_4.hpp.md) | All 4 arms enrich functionally |

## Critical Open Items

### P0 - Repair what the port exposed (Blocks Publishing)
- [ ] See **[TASKS.md § PARITY](TASKS.md)** and **[notes.md](notes.md)**. The
      harness runs, but Test 1 is not at parity, `value[x]` is excluded from every
      arm, and Test 3 still charges the FastFHIR arm a `print_json` penalty
      (PA-7). Test 2 is the random-access stage (D4) with a cross-arm byte gate.

### P1 - Google FHIR arm (revised 2026-08-25)
- [x] ~~Implement test_2::materialize() for Google FHIR~~ — **already implemented**
- [x] ~~Implement test_3::query() for Google FHIR~~ — **already implemented**
- [ ] Normalize the Google arm's `birthdate`: it reports a microsecond epoch
      (`194140800000000`) where every other arm reports ISO (`1976-02-26`)
- [ ] Include the Google arm in `validate_parity()` — it is currently never
      cross-checked against another arm

### P2 - Resource Coverage (Should Fix)
- [ ] Add Encounter, Condition, Procedure to serialization scope (hydrated but not assigned)
- [ ] Expand assign_patient() / assign_observation() for these resources across all 4 arms
- [ ] Add corresponding query logic in test_3 per arm
- [ ] Align HL7v2 birthdate format or document normalization difference
- [ ] Decide the query corpus in light of opaque resources — 1,444 Synthea
      `ImagingStudy` records are not typed-navigable and silently skip stage 3
      (TASKS.md § CORPUS)

### P2 - Result Provenance (Should Fix)
- [ ] Emit compiled profile, upstream git SHA, and `--compilation_mode` into
      the results CSV and PostgreSQL schema (TASKS.md § PROFILE)

### P3 - Enrichment & Scale (Nice to Have)
- [ ] Add multi-resource enrichment tests (Encounter, Condition, Procedure)
- [ ] Multi-bundle enrichment test (enrich repeatedly, not just once)

## Parity Architecture

See the **Code Parity** sections in README.md and IMPLEMENTATION_SUMMARY.md for the macro-guarded bench_test_N.hpp design.

⚠️ **That guarantee is currently partial.** Two things weakened it during the
2026-08-25 port:

1. **The FastFHIR arm's Test 1 no longer goes through the shared assignment
   layer.** It serializes the whole POCO via `append_obj`, because the public API
   cannot write FastFHIR's inline-block arrays field-by-field. It therefore
   serializes *more* fields than the other three arms, not fewer.
2. **The shared headers needed a per-arm inline namespace.** Compiling one header
   four ways under one set of names was an ODR violation that caused heap
   corruption. The macro-guarded design is not free — see [notes.md](notes.md) §1
   for the alternative worth considering in the redesign.

## Build Targets

Verified 2026-08-25 with `bazel build -c opt --keep_going //bench:all`:

| Target | System | Status | Doc |
|---|---|---|---|
| bench_harness (all 4 arms) | Bazel | macOS — ✅ builds, no libpq | [BUILD.bazel.md](bench/BUILD.bazel.md) |
| bench_harness_win (all 4 arms) | Bazel | Windows-only; blocked by macOS-only Google FHIR build | [BUILD.bazel.md](bench/BUILD.bazel.md) |
| bench_timing_conformance | Bazel | ✅ builds | [timing_conformance_test.cpp.md](bench/timing_conformance_test.cpp.md) |
| timing_conformance_test | Bazel | ✅ **PASSES** | [timing_conformance_test.cpp.md](bench/timing_conformance_test.cpp.md) |
| read_path_bench | Bazel | ✅ builds | [read_path_bench.cpp.md](bench/read_path_bench.cpp.md) |
| bench_harness_pg | Bazel | macOS, `manual` tag; needs host libpq for `--db` | [BUILD.bazel.md](bench/BUILD.bazel.md) |