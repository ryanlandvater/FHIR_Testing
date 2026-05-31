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

- **Parser**: `hl7v2::parse_batch(payload)` — builds the full 4-level `MessageTree` AST (Segment → Field → Component → Subcomponent), matching the parse-before-query cost model of all other arms
- **Walk**: Iterates parsed messages and their segments, using typed views for field access:
  - `PID` segments → `hl7v2::PidView` → extracts `birth_date()` (PID-7)
  - `OBX` segments → `hl7v2::ObxView` → extracts `observation_id()` (OBX-3.1), `value_type()` (OBX-2), `value()` (OBX-5)
- **LOINC matching**: Compares `ObxView::observation_id()` (first component of OBX-3) directly against `"2085-9"`
- **Value classification**: Maps OBX-2 value type field to `ValueKind`: `NM` → Quantity, `CE` → CodeableConcept, `ST` → String, `CWE` → Code
- **Cost model**: Full HL7v2 parse (message boundary scan + `\r` → `|` → `^` → `&` hierarchical splitting) + segment iteration + typed view access. Equivalent in structure to the FFHR/JSON/Google FHIR test 3 paths (parse → walk → extract).
- **Limitation**: `effective`, `issued`, and `component` fields are stored in ZFX custom segments and are not extracted by the query — these `QuerySummary` counters remain zero for the HL7v2 arm.

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
