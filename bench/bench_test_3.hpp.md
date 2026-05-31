# `bench_test_3.hpp` — Query (Test 3)

## Purpose

Implements **Test 3 (Query)**: searching each arm's serialized payload for specific clinical data — specifically patients with a `birthDate` field and observations with LOINC code `2085-9` (Total Cholesterol). This test measures the cost of **semantic data extraction** from each wire format.

## Architecture

Defines one `query()` overload per arm, guarded by `#define ARM_*`. Each overload produces a `QuerySummary` with detailed counts of what was found.

### Common Types

```cpp
struct QuerySummary {
  std::size_t patients;                       // Total patient entries
  std::string birthdate;                      // First patient birthdate found
  std::size_t observations;                   // Total observation entries
  std::size_t loinc_2085_9_matches;           // Observations with LOINC 2085-9
  std::size_t obs_value_present;              // Observations with any value
  std::size_t obs_value_quantity;             // Value is Quantity
  std::size_t obs_value_codeableconcept;      // Value is CodeableConcept
  std::size_t obs_value_string;               // Value is string
  std::size_t obs_value_code;                 // Value is code
  std::size_t obs_effective_datetime;         // Effective is dateTime
  std::size_t obs_effective_period;           // Effective is Period
  std::size_t obs_issued_present;             // Has issued field
  std::size_t obs_component_value_present;    // Has component values
  std::size_t obs_component_value_quantity;    // Component is Quantity
  std::size_t obs_component_value_codeableconcept;
  std::size_t obs_component_value_string;
  std::size_t obs_component_value_code;
};

inline std::string format_query_summary(const QuerySummary&);
```

### `detail::QueryAccumulator`

A helper class used inside each arm's `query()` that accumulates counts and provides `finalize()` → `QuerySummary`:

```cpp
struct QueryAccumulator {
  void note_patient(birthdate);
  void note_observation();
  void note_loinc_2085_9();
  void note_observation_value(ValueKind);
  void note_component_value(ValueKind);
  void note_effective_datetime();
  void note_effective_period();
  void note_issued();
};
```

### `detail::ValueKind` Enum

```cpp
enum class ValueKind { Quantity, CodeableConcept, String, Code };
```

Maps to FHIR's Observation.value[x] choice types. The query cares about which variant is used because it affects serialization cost and query complexity.

### Per-Arm Implementations

#### FastFHIR (`ARM_FASTFHIR`)

```cpp
static inline QuerySummary query(std::string_view payload);  // raw bytes from FFHR arena
```

- **Parser**: `FastFHIR::Parser(payload.data(), payload.size())` — zero-copy
- **Walk**: Iterates `root[BUNDLE.ENTRY]`, checks each resource's type via `resource_node.is<RESOURCETYPE::PATIENT>()` / `is<RESOURCETYPE::OBSERVATION>()`
- **Patient**: Reads `resource_node[PATIENT.BIRTH_DATE].as<std::string_view>()`
- **Observation**: Uses `resource_node.as<ObservationData>()` to get the POCO struct, then checks:
  - `observation.code->coding[]` for LOINC `2085-9` system/code match
  - `observation.value.tag` for value type via `value_kind_from_choice_tag()`
  - `observation.effective.tag` for dateTime vs Period
  - `observation.issued` presence
  - `observation.component[]` for component value type classification
- **Choice tag mapping**: Uses `RECOVER_FF_*` constants (`RECOVER_FF_QUANTITY`, `RECOVER_FF_CODEABLECONCEPT`, etc.) to map FFHR's choice-type tags to `ValueKind`

#### JSON (`ARM_JSON`)

```cpp
static inline QuerySummary query(const std::string& payload);
```

- **Parser**: `simdjson::dom::parser` — second parse (materialize already parsed; this is a fresh parse to simulate independent query)
- **Walk**: Iterates `doc["entry"][]["resource"]`, checks `resourceType = "Patient"` or `"Observation"`
- **Patient**: Reads `resource["birthDate"]`
- **Observation**: Checks `resource["code"]["coding"][]` for LOINC 2085-9, classifies value types from JSON field presence

#### Google FHIR (`ARM_GOOGLE_FHIR`)

```cpp
static inline QuerySummary query(const std::string& payload);
```

- **Parser**: `test_2::materialize()` first to deserialize protobufs, then walks the resulting vectors
- **Walk**: Iterates patients and observations, using protobuf reflection and direct field access
- **Value classification**: Checks `observation.has_value()` and inspects which `value_*` oneof field is set

#### HL7v2 (`ARM_HL7V2`)

```cpp
static inline QuerySummary query(const std::string& payload);
```

- **Parser**: None — no `parse_batch()` call. Scans the raw concatenated payload line-by-line via `\r` splitting, using `segment_field()` (a lightweight `|`-delimited field extractor) to pull specific fields on demand
- **Walk**: Scans each line looking for `PID|` and `OBX|` prefixes:
  - `PID|` → increments patient count, extracts PID-7 (birthdate) via `segment_field(seg, 8)` (1-based)
  - `OBX|` → increments observation count, extracts OBX-3 (code), OBX-2 (value type), OBX-5 (value) via `segment_field()`
- **LOINC matching**: Checks if OBX-3 starts with `"2085-9"` (LOINC cholesterol code)
- **Value classification**: Uses OBX-2 value type field: `NM` → Quantity, `CE` → CodeableConcept, `ST` → String, `CWE` → Code
- **Cost model**: Single-pass line scan with on-demand field extraction — zero heap allocations for parsing, intentionally lighter than materialize(). This models the common EHR pattern of streaming through HL7v2 messages without loading the full parse tree into memory.

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Re-parse payload (don't reuse materialize output) | Simulates independent query against serialized data (the common EHR query pattern) |
| Detailed value type breakdown | Reveals how choice-type fields affect serialization efficiency across formats |
| First birthdate, not all birthdates | QuerySummary is for validation parity, not census — one birthdate is enough |
| Separate LOINC match vs all observations | Measures selectivity cost: checking every coding vs total observation count |

## Dependencies

- `bench_test_2.hpp` (for Google FHIR arm — reuses materialize)
- FastFHIR: `FF_Bundle.hpp`, `FF_Observation.hpp`, `FastFHIR.hpp`
- `simdjson.h` (for JSON arm)
- Google FHIR protobuf headers (for Google FHIR arm)
- `hl7v2_message.hpp` (for HL7v2 arm)
