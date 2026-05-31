# `bench/arm_hl7v2.cpp` — HL7v2 Serialization Arm

## Purpose

Implements the **HL7v2 serialization arm** of the benchmark. Converts the same
in-memory FHIR structs into HL7v2 ORU^R01 pipe-delimited messages. This arm
demonstrates the cost of mapping a modern resource-oriented data model (FHIR)
into a legacy flat-file healthcare messaging format.

## How It Works

### Test 1 — Serialize (Timed)

```
Timer start → serialize → Timer stop
```

1. **Pre-allocate**: Reserve `bundle.size() * 512` bytes for the output string —
   a rough estimate of per-patient HL7v2 message size.
2. **For each BundlePatient**:
   - Create a new `hl7v2::OruR01Message` (which produces MSH + PID headers).
   - `assign::assign_patient(item.patient, message)` — writes patient fields
     into the PID segment via template overloads from `bench_test_1.hpp`.
   - For each observation: `assign::assign_observation(observation, message)` —
     appends OBX segments to the message.
   - `message.dump()` serializes the complete message string.
   - Accumulate into `payload` (no array wrapper — HL7v2 is stream-oriented).
3. **Record metric**: Stage `Test1Serialize` with elapsed nanoseconds.

### Serialization Pattern

Unlike the FastFHIR and JSON arms which produce a single Bundle container, the
HL7v2 arm produces **one ORU^R01 message per patient**, concatenated into a
continuous string. This reflects real HL7v2 usage: each ADT/ORU message is
self-contained with its own MSH segment.

### Tests 2–4
- **Test 2**: `test_2::materialize(payload)` — parses the concatenated HL7v2
  stream into a 4-level `MessageTree` AST per message (`Segment → Field →
  Component → Subcomponent`), then walks every syntactic node counting
  `touched_nodes`. Full parse + tree walk cost.
- **Test 3**: `test_3::query(payload)` — scans the raw concatenated payload
  line-by-line, using `segment_field()` for on-demand field extraction
  (no parse tree allocated). Extracts patient count, birthdate, LOINC matches,
  and observation value type classification.
- **Test 4**: `test_4::enrich_hl7v2(payload, obs)` — appends a new ORU^R01
  message to the payload string.

## Key Design Decisions

| Decision | Rationale |
|---|---|
| One ORU^R01 per patient | HL7v2 has no "Bundle" equivalent. Each patient+observations is a separate message. |
| `hl7v2_message.hpp` defines the segment types | Keeps the arm file focused on the benchmark flow; message structure is reusable. |
| No parallel dispatch | HL7v2 serialization is primarily string concatenation — the bottleneck is not CPU-bound enough to benefit from parallelism in this context. |
| Output is single concatenated string | Real HL7v2 ingest pipelines batch-delimit messages and process them sequentially. The benchmark preserves this realistic model. |

## FHIR → HL7v2 Mapping Challenges

The `assign::assign_patient()` and `assign::assign_observation()` templates for
HL7v2 must handle significant semantic gaps:

| FHIR Concept | HL7v2 Equivalent | Mapping Complexity |
|---|---|---|
| `Patient.birthDate` (date string) | `PID-7` (DTM) | Digit-stripping normalization |
| `Patient.gender` (enum) | `PID-8` (M/F/O/U) | Enum→char mapping |
| `Patient.name` (HumanName) | `PID-5` (XPN) | Family^Given |
| `Patient.address` (Address) | `PID-11` (XAD) | Line^^City^State^Postal^Country |
| `Observation.code.coding[]` | `OBX-3` (CE) | First matching LOINC code^display^LN |
| `Observation.value` (Choice) | `OBX-5` (string) | All FHIR value types map to string |
| Bundle concept | N/A | No equivalent — emits multiple independent messages |
