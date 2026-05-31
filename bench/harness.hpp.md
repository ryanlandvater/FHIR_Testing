# `harness.hpp` — Core Types, Timer, Fixtures, and Validation

## Purpose

This is the **central type definition and interface header** for the entire benchmark harness. It defines:

- The shared data model (`BundlePatient`, `BundleBenchFixture`, `EnrichmentObservationFixture`)
- The `Timer` class for wall-clock measurement
- The `Stage` enum and `MetricEvent` / `ArmRunResult` result types
- The four arm function signatures (`run_fastfhir_bundle`, `run_json_bundle`, `run_hl7v2_bundle`, `run_google_fhir_bundle`)
- Cross-arm validation functions (`validate_results`, `validate_hl7_results`)
- The `clone_bundle_patient()` utility

Every arm implementation file and every test header includes this first.

## Key Types

### `Stage` Enum

```cpp
enum class Stage {
  Test1Serialize,    // Measure struct-to-wire-format conversion
  Test2Materialize,  // Measure wire-format-to-tree deserialization
  Test3Query,        // Measure tree walk for LOINC 2085-9 search
  Test4Enrich,       // Measure append-one-observation workflow
};
```

Legacy aliases (`Stage1Serialize`, `Stage2Transport`, `Stage3Query`, `Stage3Materialize`) exist for backward compatibility during the stage→test migration.

### `MetricEvent`

```cpp
struct MetricEvent {
  std::string arm;       // e.g. "fastfhir", "json_fhir", "hl7v2", "google_fhir"
  Stage stage;           // Which test stage
  std::int64_t duration_ns;  // Elapsed nanoseconds (floor 1)
};
```

### `ArmRunResult`

```cpp
struct ArmRunResult {
  std::vector<MetricEvent> metrics;              // One per stage
  std::string queried_value;                      // Formatted QuerySummary
  std::string reconstructed_bundle_json;          // Wire-format output string (or placeholder)
  std::variant<std::monostate, FastFHIR::Memory, std::string> enriched_stream;
  std::string enrich_metrics_summary;             // Formatted EnrichMetricsSummary
};
```

### `BundlePatient`

Per-patient in-memory container, holding both the original FastFHIR arena and all parsed FHIR structs:

```cpp
struct BundlePatient {
  FastFHIR::Memory memory;          // FFHR arena from Synthea ingestion
  PatientData patient;               // Demographics
  std::vector<EncounterData> encounters;
  std::vector<ConditionData> conditions;
  std::vector<ProcedureData> procedures;
  std::vector<ObservationData> observations;
};
```

### `BundleBenchFixture`

Input to every arm function:

```cpp
struct BundleBenchFixture {
  std::vector<BundlePatient> bundle;
  int64_t target_size_bytes;         // Desired bundle size (for run-scaling)
  int64_t actual_ingested_bytes;     // Actual bytes ingested
  int64_t fastfhir_vma_bytes;        // VMA footprint of FastFHIR arenas
};
```

### `Timer`

```cpp
class Timer {
  void start();
  std::int64_t stop_ns() const;  // Returns elapsed ns (minimum 1)
};
```

Uses `std::chrono::steady_clock`. Minimum return of 1 ns avoids zero-duration artifacts.

## Utility Functions

### `to_string(Stage)`

Maps `Stage` enum to CSV-safe string: `"test_1_serialize"`, `"test_2_materialize"`, etc.

### `print_metric(const MetricEvent&)`

Writes CSV line to stdout for real-time monitoring.

### `clone_bundle_patient(const BundlePatient&)`

Deep-clones a `BundlePatient` by copying its `FastFHIR::Memory` arena and re-hydrating all POCO fields via `detail::hydrate_bundle_resources()`. Used when constructing fixtures that need independent arena copies.

## Validation Functions

### `validate_results(const ArmRunResult&, const ArmRunResult&)`

Compares queried values (patient count, birthdate) between two arms to verify message-surface parity. Returns `true` if they match.

### `validate_hl7_results(const ArmRunResult&, const ArmRunResult&, const ArmRunResult&)`

Three-way validation between FastFHIR, JSON, and HL7v2 arms. Accounts for HL7v2's intrinsic birthdate normalization (digits-only).

## Constants

```cpp
constexpr std::string_view kPatientQueryField = "birthDate";
constexpr std::string_view kCholesterolLoincCode = "2085-9";
constexpr std::string_view kLoincSystem = "http://loinc.org";
```

## Dependencies

- FastFHIR headers: `FastFHIR.hpp`, `FF_Bundle.hpp`, `FF_Condition.hpp`, `FF_Encounter.hpp`, `FF_FieldKeys.hpp`, `FF_Observation.hpp`, `FF_Patient.hpp`, `FF_Procedure.hpp`
- Standard: `<chrono>`, `<cstdint>`, `<filesystem>`, `<iostream>`, `<variant>`, `<string>`, `<vector>`
