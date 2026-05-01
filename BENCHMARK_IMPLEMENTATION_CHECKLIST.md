# FastFHIR Four-Arm Benchmark — Implementation Checklist

**Study**: FastFHIR vs. JSON-FHIR vs. Google FHIR vs. HL7v2  
**Reference**: `FastFHIR Benchmarking Study Design V1.1.md`  
**Status**: 🚧 In Progress

---

## Overview

The benchmark is a single harness executable that loops over multiple payload sizes. For each size it
accumulates `PatientData` records into an in-memory "EHR bundle" and then runs all four arms against
that same bundle. Each arm is responsible for serialization (Stage 1), a future Asio transport stub
(Stage 2), and query/traversal (Stage 3). Results are written to stdout CSV and optionally to
PostgreSQL for notebook analysis.

```
for each target_mb in [1, 2, 4, 8, 16, 32, 64, 256]:
    bundle = accumulate PatientData records until ingested >= target_mb
    for each arm in [fastfhir, json_fhir, google_fhir, hl7v2]:
        Stage 1 — serialize bundle → wire representation
        Stage 2 — (stub) simulate Asio send/receive
        Stage 3 — parse wire representation, extract birthDate + cholesterol LOINC 2085-9 value
        emit MetricEvent per stage
```

---

## Section 1 — Shared Data Model (`bench/harness.hpp`)

### 1.1 Source-of-Truth Types
- [x] `PatientData` struct (from FastFHIR generated headers) — used as EHR input for all arms
- [x] `CholesterolObservation` struct — `system`, `code`, `value`, `has_value`
- [x] `SyntheaFixture` struct — `PatientData` + `CholesterolObservation` vector + owned string storage
- [x] `BundleBenchFixture` struct — vector of `SyntheaFixture`, `target_size_bytes`, `actual_ingested_bytes`

### 1.2 Timing & Metrics Types
- [x] `Stage` enum — `Stage1Serialize`, `Stage2Transport`, `Stage3Query`, `Stage3Materialize`
- [x] `MetricEvent` struct — `arm` (string), `stage` (Stage), `duration_us` (int64)
- [x] `ArmRunResult` struct — `metrics` vector + `queried_value` string
- [x] `Timer` class — `start()` / `stop_us()` via `std::chrono::steady_clock`

### 1.3 Arm Function Signatures
All four arms share the same signature so `main.cpp` can call them uniformly:
```cpp
ArmRunResult run_fastfhir_bundle   (const BundleBenchFixture& fixture);
ArmRunResult run_json_bundle        (const BundleBenchFixture& fixture);
ArmRunResult run_google_fhir_bundle (const BundleBenchFixture& fixture);
ArmRunResult run_hl7v2_bundle       (const BundleBenchFixture& fixture);
```
- [x] `run_fastfhir_bundle` declared
- [x] `run_json_bundle` declared
- [ ] `run_google_fhir_bundle` — remove conflicting `void` overload, add correct `ArmRunResult` signature
- [ ] `run_hl7v2_bundle` — remove conflicting `void` overload, add correct `ArmRunResult` signature

### 1.4 Query Constants
- [x] `kPatientQueryField = "birthDate"`
- [x] `kCholesterolLoincCode = "2085-9"`
- [x] `kLoincSystem = "http://loinc.org"`

---

## Section 2 — Data Ingestion (`bench/synthea_fixture.cpp`)

- [x] `make_synthea_fixture(path)` — reads `.ffhr` file via `FastFHIR::Parser`
- [x] Extracts `PatientData.id`, `birthdate`, `gender`, `active` into owned strings
- [x] Extracts `CholesterolObservation` entries (LOINC 2085-9) from `.ffhr` observation resources
- [x] `clone_fixture()` — deep copies owned strings so bundle can be composed safely

---

## Section 3 — FastFHIR Arm (`bench/arm_fastfhir.cpp`)

### Stage 1 — Serialization
- [x] Pre-allocate `FastFHIR::Memory::create(arena_size)` and prefault before timing
- [x] Create `FastFHIR::Builder(mem, FHIR_VERSION_R5)`
- [x] **Timer START** — immediately before first field write
- [x] For each patient in fixture: `builder.append_obj(scaffold)` then field-by-field assign:
  - [x] `[PATIENT::ID] = p.patient.id`
  - [x] `[PATIENT::BIRTH_DATE] = p.patient.birthdate`
  - [x] `[PATIENT::ACTIVE] = ...`
  - [ ] For each `CholesterolObservation`: append `ObservationData` entry to bundle with LOINC code + value
- [x] Wrap entries in `BundleData` collection and call `builder.set_root()` + `builder.finalize()`
- [x] **Timer STOP** — immediately after `view = builder.finalize()`, before any transport

### Stage 2 — Transport (Stub)
- [ ] Add `// TODO(stage2): replace with asio::async_write(socket, view.data(), view.size())`
- [ ] Add `MetricEvent{..., Stage::Stage2Transport, 0}` placeholder with zero duration

### Stage 3 — Query / Traversal
- [x] **Timer START** — immediately before `FastFHIR::Parser parser(view.data(), view.size())`
- [x] `parser.root()` → navigate to `BUNDLE::ENTRY` array
- [x] For each entry: check `is<PATIENT>()`, extract `BIRTH_DATE` field
- [ ] For each entry: check `is<OBSERVATION>()`, find LOINC 2085-9, extract numeric value
- [x] **Timer STOP** — after all target values extracted into local variables
- [ ] Store both `birth_date` and `cholesterol_value` in `result.queried_value`

---

## Section 4 — JSON FHIR Arm (`bench/arm_json_fhir.cpp`)

### Stage 1 — Serialization
- [x] Pre-allocate `nlohmann::json bundle` object before timing
- [x] **Timer START** — immediately before first field append to json object
- [x] For each patient: build `patient_resource` object field-by-field:
  - [x] `patient_resource["resourceType"] = "Patient"`
  - [x] `patient_resource["birthDate"] = ...`
  - [x] `patient_resource["active"] = ...`
  - [ ] Add `observation` entries: `resourceType = "Observation"`, `code.coding[0].code = "2085-9"`, `valueQuantity.value = ...`
- [x] `bundle.dump()` to produce JSON string
- [x] **Timer STOP** — after `dump()` completes

### Stage 2 — Transport (Stub)
- [ ] Add `// TODO(stage2): send payload string over Asio socket`
- [ ] Add `MetricEvent{..., Stage::Stage2Transport, 0}` placeholder

### Stage 3 — Query / Traversal
- [x] **Timer START** — immediately before `nlohmann::json::parse(payload)`
- [x] Navigate parsed JSON: iterate `entry` array, find `resourceType == "Patient"`, extract `birthDate`
- [ ] Find `resourceType == "Observation"`, check `code.coding[].code == "2085-9"`, extract `valueQuantity.value`
- [x] **Timer STOP** — after target values extracted
- [ ] Store both extracted values in `result.queried_value`

---

## Section 5 — Google FHIR Arm (`bench/arm_google_fhir.cpp`)

> **Status**: Smoke test skeleton. Full implementation requires `google/fhir` C++ library integration.
> Document any API gaps in `FFHRnotes.md`.

### Dependency (Not Yet Integrated)
- [ ] Add `google/fhir` to `generate_repo.sh` (clone + Bazel/CMake build or pre-built package)
- [ ] Add CMake `find_package` or `FetchContent` for `google-fhir` and protobuf in `bench/CMakeLists.txt`
- [ ] Add `HAVE_GOOGLE_FHIR` compile guard in arm file and CMakeLists

### Stage 1 — Serialization (Smoke — current)
- [ ] **Timer START**
- [ ] Smoke: loop over patients, simulate per-patient serialization work
- [ ] **Timer STOP**
- [ ] Emit `MetricEvent{"google_fhir", Stage::Stage1Serialize, duration}`

### Stage 1 — Serialization (Real — future, requires google/fhir)
- [ ] `google::fhir::r4::Patient proto_patient` per patient
- [ ] Field-by-field: `proto_patient.set_id(...)`, `mutable_birth_date()->set_value_us(...)`
- [ ] Add `Observation` submessage per `CholesterolObservation`: set coding system/code + valueQuantity
- [ ] Serialize bundle: `google::fhir::PrintFhirToJsonString(bundle_proto)` or protobuf wire bytes

### Stage 2 — Transport (Stub)
- [ ] `// TODO(stage2): asio transport stub`
- [ ] `MetricEvent{"google_fhir", Stage::Stage2Transport, 0}` placeholder

### Stage 3 — Query (Smoke — current)
- [ ] **Timer START**
- [ ] Smoke: loop over patients, simulate per-patient query work
- [ ] **Timer STOP**
- [ ] Emit `MetricEvent{"google_fhir", Stage::Stage3Query, duration}`

### Stage 3 — Query (Real — future, requires google/fhir)
- [ ] `JsonFhirStringToProto<Bundle>(payload)` → `bundle_proto`
- [ ] Iterate `bundle_proto.entry()` → find `Patient`, access `birth_date()`
- [ ] Find `Observation` where `coding.code() == "2085-9"`, read `value_quantity().value()`

---

## Section 6 — HL7v2 Arm (`bench/arm_hl7v2.cpp`)

> **Status**: Smoke test skeleton. Full implementation requires `jcomellas/hl7parser` integration.
> Document any API gaps in `FFHRnotes.md`. Comparisons are limited to workflows naturally
> representable in HL7v2 without inventing nonstandard segments.

### Dependency (Not Yet Integrated)
- [ ] Add `jcomellas/hl7parser` to `generate_repo.sh`
- [ ] Add `FetchContent` in `bench/CMakeLists.txt`
- [ ] Add `HAVE_HL7PARSER` compile guard

### Stage 1 — Serialization (Smoke — current)
- [ ] **Timer START**
- [ ] Smoke: loop over patients, simulate per-patient message construction work
- [ ] **Timer STOP**
- [ ] Emit `MetricEvent{"hl7v2", Stage::Stage1Serialize, duration}`

### Stage 1 — Serialization (Real — future, requires hl7parser)
For each patient build an `ORU^R01` HL7v2 message:
```
MSH|^~\&|BENCH|BENCH|BENCH|BENCH|<timestamp>||ORU^R01|<msgid>|P|2.5
PID|1||<patient_id>|||||||<birthdate>|<gender>
OBX|1|NM|2085-9^Total Cholesterol^LN||<value>|mg/dL|...
```
- [ ] Concatenate segments with `\r` delimiter
- [ ] No nonstandard segments; if field has no HL7v2 mapping, omit and document in `FFHRnotes.md`

### Stage 2 — Transport (Stub)
- [ ] `// TODO(stage2): asio transport stub`
- [ ] `MetricEvent{"hl7v2", Stage::Stage2Transport, 0}` placeholder

### Stage 3 — Query (Smoke — current)
- [ ] **Timer START**
- [ ] Smoke: loop over patients, simulate per-patient parse + query work
- [ ] **Timer STOP**
- [ ] Emit `MetricEvent{"hl7v2", Stage::Stage3Query, duration}`

### Stage 3 — Query (Real — future, requires hl7parser)
- [ ] Parse message string using `hl7parser`
- [ ] Extract `PID.7` (date of birth)
- [ ] Find `OBX` segment where `OBX.3` code field == `"2085-9"`, read `OBX.5` (observation value)

---

## Section 7 — Main Harness Loop (`bench/main.cpp`)

- [x] Parse CLI args: `--iterations`, `--bundle-max-mb`, `--ff-vma-mb`, `--db`, `--bundle-targets-mb`
- [x] Load all `.ffhr` files from `datasets/synthea/` into `all_patients`
- [x] Loop over `target_sizes_bytes` — accumulate `BundleBenchFixture`
- [ ] **Fix**: Remove orphaned `jf`/`ff` dead code blocks from previous session
- [ ] **Fix**: Call all four arms uniformly via `ArmRunResult` pattern:
  ```cpp
  const auto ff  = bench::run_fastfhir_bundle(bundle);
  const auto jf  = bench::run_json_bundle(bundle);
  const auto gf  = bench::run_google_fhir_bundle(bundle);
  const auto h2  = bench::run_hl7v2_bundle(bundle);
  for (const auto* r : {&ff, &jf, &gf, &h2})
      for (const auto& m : r->metrics)
          { emit CSV row; maybe_insert_metric(m); }
  ```
- [x] CSV header: `arm,stage,duration_us,target_mb,patients_in_bundle`
- [x] PostgreSQL `maybe_insert_metric` lambda

---

## Section 8 — Build System (`bench/CMakeLists.txt`)

- [x] `nlohmann_json` via `FetchContent`
- [x] `FastFHIR` from `FASTFHIR_INSTALL_PREFIX`
- [x] Optional `PostgreSQL` / `libpq`
- [x] `bench_harness` includes all four arm `.cpp` files + `synthea_fixture.cpp`
- [x] `bench_timing_conformance` includes arm files
- [ ] Add optional `HAVE_GOOGLE_FHIR` block: `find_package(absl)`, `find_package(Protobuf)`, link google-fhir
- [ ] Add optional `HAVE_HL7PARSER` block: `FetchContent_Declare(hl7parser ...)`, link hl7parser

---

## Section 9 — Setup Script (`generate_repo.sh`)

- [x] Incremental FastFHIR build (stamp file, `FASTFHIR_RUN_GENERATOR=OFF`)
- [x] Synthea JSON download
- [x] `ff_ingestor` / `ff_ingest` conversion to `.ffhr`
- [ ] Clone and build `google/fhir` — or document as manual step in `FFHRnotes.md`
- [ ] Clone and build `jcomellas/hl7parser` — or document as manual step in `FFHRnotes.md`

---

## Section 10 — Stage 2 Transport (Future)

All stage 2 items are deferred. Zero-duration stubs must be in every arm so the CSV schema is stable.

- [ ] Asio TCP socket setup (loopback sender + receiver, same process or two threads)
- [ ] FastFHIR: `asio::async_write(socket, asio::buffer(view.data(), view.size()))`
- [ ] JSON FHIR: `asio::async_write(socket, asio::buffer(payload.data(), payload.size()))`
- [ ] Google FHIR: transport proto binary or JSON string
- [ ] HL7v2: transport MLLP-framed or raw message string
- [ ] Receive side: buffer read, hand off raw bytes to Stage 3 parser

---

## Section 11 — Results Notebook (`notebooks/benchmark_results.ipynb`)

- [x] Connect to PostgreSQL
- [x] Query `benchmark_results` table
- [ ] Update plots to handle all four arms: `fastfhir`, `json_fhir`, `google_fhir`, `hl7v2`
- [ ] Per arm per stage: P50/P95/P99 latency across `target_mb` sweep
- [ ] Payload-size comparison chart (bytes serialized per arm)
- [ ] Cold vs. warm run comparison (iteration 0 vs. iteration N)

---

## Implementation Priority Order

| Priority | Item                                                    | Status     |
|----------|---------------------------------------------------------|------------|
| P0       | Fix `harness.hpp` — remove conflicting declarations     | ☐ Todo     |
| P0       | Fix `main.cpp` — remove orphaned `jf`/`ff` dead code    | ☐ Todo     |
| P1       | `arm_fastfhir.cpp` — add Observation to Stage 1 + 3    | ☐ Todo     |
| P1       | `arm_json_fhir.cpp` — add Observation to Stage 1 + 3   | ☐ Todo     |
| P2       | `arm_google_fhir.cpp` — correct smoke (`ArmRunResult`)  | ☐ Todo     |
| P2       | `arm_hl7v2.cpp` — correct smoke (`ArmRunResult`)        | ☐ Todo     |
| P3       | `CMakeLists.txt` — `HAVE_GOOGLE_FHIR` / `HAVE_HL7PARSER`| ☐ Todo     |
| P4       | `generate_repo.sh` — google/fhir + hl7parser deps       | ☐ Todo     |
| P5       | Stage 2 Asio transport                                  | ☐ Future   |
