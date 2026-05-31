# `bench_test_1.hpp` — Shared Field Assignment Layer (Test 1)

## Purpose

This is the **single most critical file in the benchmark** for fairness. It defines the `assign::` namespace containing overloaded `assign_patient()` and `assign_observation()` functions that write the same POCO data to **four different output types**:

| Output Target | Arm | Selected By |
|---|---|---|
| FastFHIR `ObjectHandle` | `arm_fastfhir.cpp` | `#define ARM_FASTFHIR` |
| `nlohmann::json` | `arm_json_fhir.cpp` | `#define ARM_JSON` |
| Google Protobuf message | `arm_google_fhir.cpp` | `#define ARM_GOOGLE_FHIR` |
| HL7v2 `OruR01Message` | `arm_hl7v2.cpp` | `#define ARM_HL7V2` |

## Design Philosophy

> **"This header is intentionally NOT representative of normal FastFHIR usage."**

In production, FastFHIR users populate structs and call `builder.append_obj()` directly. This benchmark instead uses per-line field assignment to guarantee **bit-identical data** reaches every serialization backend. The cost is verbose field-by-field copying — but that's the point: every arm pays the same struct-access cost, so the serialization technique is the only variable.

## Architecture

### Macro-Guarded Compilation

Each arm file `#define`s its arm macro before including this header:

```cpp
// arm_fastfhir.cpp:
#define ARM_FASTFHIR
#include "bench_test_1.hpp"
#undef ARM_FASTFHIR
```

The `#if defined(ARM_FASTFHIR)`, `#elif defined(ARM_JSON)`, etc. blocks inside this header expose only the assignment overloads relevant to that translation unit.

### Assignment Overloads

#### `assign::assign_patient(source, target)`

| Source | Target (per arm) |
|---|---|
| `const PatientData&` (POCO) | `FastFHIR::ObjectHandle` — Arena field writes |
| `const PatientData&` (POCO) | `nlohmann::json&` — JSON key-value assignment |
| `const PatientData&` (POCO) | `GooglePatientTarget` — Protobuf `Patient` message |
| `const PatientData&` (POCO) | `hl7v2::OruR01Message&` — PID segment fields + custom ZFX |

#### `assign::assign_observation(source, target)`

| Source | Target (per arm) |
|---|---|
| `const ObservationData&` (POCO) | `FastFHIR::ObjectHandle` |
| `const ObservationData&` (POCO) | `nlohmann::json&` |
| `const ObservationData&` (POCO) | `GoogleObservationTarget` — Protobuf `Observation` + patient ID |
| `const ObservationData&` (POCO) | `hl7v2::OruR01Message&` — OBX segments |

### Google FHIR Helper Types

```cpp
struct GooglePatientTarget {
  google::fhir::r4::core::Patient& patient;
};
struct GoogleObservationTarget {
  google::fhir::r4::core::Observation& observation;
  std::string_view fallback_patient_id;
};
```

These wrappers allow the template machinery to distinguish Google FHIR overloads at compile time.

### HL7v2 Helper Sink

```cpp
struct HL7v2Sink {
  bench::hl7v2::OruR01Message& message;
  ObxSegment current_obx{};
  bool has_open_observation = false;
  void begin_observation();
  void finish_observation();
};
```

Manages the stateful OBX segment lifecycle — HL7v2's flat-segment model requires explicit begin/end calls around each observation's fields.

### Google FHIR Mapping Helpers

- `google_birthdate_to_us()` — Parses `birthDate` string → microseconds since epoch
- `google_map_gender()` — `AdministrativeGender` → `AdministrativeGenderCode_Value`
- `google_map_observation_status()` — `ObservationStatus` → `ObservationStatusCode_Value`

### JSON Helper Functions

The JSON arm also includes a comprehensive set of `to_json_*()` conversion functions (about 30 helpers) that map every FHIR data type to its nlohmann::json representation:

- `to_json_meta()`, `to_json_human_name()`, `to_json_address()`, `to_json_contact_point()`
- `to_json_codeable_concept()`, `to_json_coding()`, `to_json_quantity()`, `to_json_ratio()`
- `to_json_period()`, `to_json_range()`, `to_json_annotation()`, `to_json_attachment()`
- `to_json_narrative()`, `to_json_extension()`, `to_json_sampled_data()`
- And the `write_choice()`/`put_if_*()` template helpers for optional fields

### Important Constants

```cpp
constexpr std::string_view kPatientQueryField = "birthDate";
constexpr std::string_view kCholesterolLoincCode = "2085-9";
constexpr std::string_view kLoincSystem = "http://loinc.org";
```

## Null-Safe Field Helpers

```cpp
// FastFHIR: has_u8(v) checks v != FF_NULL_UINT8, has_f64(v) checks v != FF_NULL_F64, etc.
// JSON: put_if_string(out, key, v) only writes the key if v is non-empty
// Both: avoid serializing sentinel values or empty optionals
```

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Per-line field assignment, not struct copy | Guarantees identical data reaches every serialization backend |
| Macro-guarded inclusion in one file | Single audit point for all assignment logic across all four arms |
| 30+ JSON helper functions | nlohmann::json requires explicit field construction (no automatic reflection) |
| HL7v2 sink manages OBX lifecycle | HL7v2's flat message model needs begin/end state for multi-field observations |
| No virtual dispatch | All overloads resolved at compile time — zero runtime indirection |

## What Gets Assigned

Both `assign_patient` and `assign_observation` cover the full FHIR R4 data model for these resources:

**Patient**: id, meta, text, extension, identifier, name, telecom, gender, birthDate, address, maritalStatus, multipleBirth, contact, communication, generalPractitioner, managingOrganization, link, active, deceased

**Observation**: id, meta, text, extension, identifier, status, category, code, subject, encounter, effective, issued, performer, value, dataAbsentReason, interpretation, note, bodySite, method, specimen, device, referenceRange, hasMember, derivedFrom, component
