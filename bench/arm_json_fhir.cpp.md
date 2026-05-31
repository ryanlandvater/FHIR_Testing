# `bench/arm_json_fhir.cpp` — JSON (nlohmann) Serialization Arm

## Purpose

Implements the **JSON serialization arm** using nlohmann::json. Converts the same
in-memory `PatientData` / `ObservationData` structs into a standard FHIR JSON
Bundle string. This arm serves as the human-readable, schema-less baseline against
which FastFHIR's binary format is compared.

## How It Works

### Test 1 — Serialize (Timed)

```
Timer start → serialize → Timer stop
```

1. **Construct Bundle shell**: Create `nlohmann::json` object with `resourceType`,
   `type`, and an empty `entry` array.
2. **Process patient entries**: For each `BundlePatient`:
   - `assign::assign_patient(item.patient, patient_json)` — the same assignment
     template from `bench_test_1.hpp`, now writing to nlohmann::json instead of
     FastFHIR handles.
   - Wrap in `{{"resource", patient_json}}` for FHIR Bundle entry structure.
3. **Flatten observation pointers**: Same pattern as FastFHIR arm.
4. **Process observation entries**: `assign::assign_observation()` into JSON.
5. **Merge**: Concatenate patient and observation JSON arrays into
   `bundle["entry"]`.
6. **Finalize**: `bundle.dump()` produces the wire-format JSON string.
7. **Record metric**: Stage `Test1Serialize` with elapsed nanoseconds.

### Platform Dispatch

Same parallel/sequential patterns as arm_fastfhir.cpp:
- macOS dispatch (commented out)
- Parallel STL (commented out)
- Sequential `std::transform` (active)

### Tests 2–4
- **Test 2**: `test_2::materialize(payload)` — parse the JSON string with simdjson
  DOM parser, walk tree counting nodes.
- **Test 3**: `test_3::query(payload)` — re-parse with simdjson, walk entries
  counting patients, LOINC matches, observation value types.
- **Test 4**: `test_4::enrich_json(payload, obs)` — parse with nlohmann::json,
  append new observation, re-serialize.

## Key Design Decisions

| Decision | Rationale |
|---|---|
| `nlohmann::json` for serialization, simdjson for materialize/query | nlohmann provides the most straightforward struct→JSON mapping (the `assign::` template writes into it). simdjson is faster for reading — the benchmark tests both libraries at their strength. |
| JSON serialization is allocation-heavy (sinks into `nlohmann::json::dump`) | This is inherent to DOM-based serializers. It's the baseline FastFHIR is measured against — a fair comparison of the full round-trip cost. |
| Enrichment re-parses the entire bundle with nlohmann | Realistic worst-case: a JSON-based system must parse, modify, and re-serialize to append data. FastFHIR's append-directly-to-arena avoids this. |

## Comparison to FastFHIR Arm

| Aspect | JSON arm | FastFHIR arm |
|---|---|---|
| Serialize target | nlohmann::json DOM → string | FastFHIR arena → sealed binary |
| Bundle append (enrich) | Parse → modify → re-serialize | Read root → append → re-finalize |
| Output format | Human-readable JSON string | Opaque binary bytes |
| Output size | ~2× source JSON (nlohmann overhead) | Typically smaller; arena is compact |
| Query | simdjson DOM walk | FastFHIR Parser walk |
