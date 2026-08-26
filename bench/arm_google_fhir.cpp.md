# `arm_google_fhir.cpp` — Google FHIR (Protobuf) Serialization Arm

## Purpose

Benchmarks all four test stages for **Google FHIR** — Google's protobuf-based FHIR implementation. This arm constructs real `google::fhir::r4::core::Patient` and `google::fhir::r4::core::Observation` protobuf messages, serializes them to a custom packed record format (TLV-like), then materializes and queries them.

**Status:** Stage 1 (Serialize) is live. Stages 2/3/4 are stubbed with minimal implementations pending full protobuf materialization support.

## Architecture

```
  BundleBenchFixture (POCO structs)
         │
         ▼
  google::fhir::r4::core::Patient + Observation protobuf messages
         │
         ▼
  Packed TLV records: 'P'|len|protobuf_bytes, 'O'|len|protobuf_bytes
         │
    ┌────┴──────────────┬──────────────────┐
    ▼                   ▼                  ▼
  test_2::random_access  test_3::query    test_4::enrich
  (TLV scan + parse)    (reflect query)  (re-serialize TLV)
```

## Stage Breakdown

### Test 1 — Serialize (`run_google_fhir_bundle`)

**What it measures:** Wall-clock from constructing the first protobuf message through serialization of all TLV records.

**Algorithm:**

1. **Payload reservation** — Pre-allocates `payload.reserve(fixture.bundle.size() * 320)`.

2. **Per-patient protobuf construction** — For each `BundlePatient`:
   - Creates `google::fhir::r4::core::Patient`
   - Wraps in `assign::GooglePatientTarget{patient}` and calls `assign::assign_patient()`
   - Calls `patient.SerializeToString()` → packs as `'P' | 4-byte LE length | bytes`

3. **Per-observation protobuf construction** — For each observation:
   - Creates `google::fhir::r4::core::Observation`
   - Wraps in `assign::GoogleObservationTarget{obs, patient_id}`
   - Calls `assign::assign_observation()` — maps each POCO field to the protobuf field via the shared assignment layer in `bench_test_1.hpp`
   - Serializes and packs as `'O' | 4-byte LE length | bytes`

4. **Custom TLV format** — The `append_record()` / `append_u32_le()` helpers create a lightweight framing layer around each serialized protobuf. This is **not** a standard FHIR wire format — it is a benchmark-internal envelope that preserves record boundaries without requiring a Bundle wrapper.

### Test 2 — Random Access

Delegates to `test_2::random_access()` which, per read, walks the length
prefixes from the start of the payload to the i-th record and
`ParseFromArray()`s the target — O(i), the property under test (no index into
a length-prefixed stream). See `bench_test_2.hpp.md`.

### Test 3 — Query

Delegates to `test_3::query()` which iterates deserialized protobufs via reflection, counts patients, observations, and LOINC `2085-9` code matches. See `bench_test_3.hpp.md`.

### Test 4 — Enrich

Delegates to `test_4::enrich_google_fhir()` which:
1. Parses all TLV records back into protobuf messages
2. Constructs a new `Observation` via `assign::assign_observation()`
3. Appends it to the observations list
4. Re-serializes all records in original order plus the new observation
5. Writes back to TLV format

## The Custom TLV Record Format

```
Byte 0:       Record type ('P' = Patient, 'O' = Observation)
Bytes 1-4:    Record length as little-endian uint32
Bytes 5+:     Serialized protobuf bytes (len bytes)
```

This is a benchmark-only format. Real Google FHIR uses `Bundle` protobuf messages wrapping individual resources.

## Key Design Decisions

| Decision | Rationale |
|---|---|
| TLV envelope instead of Bundle proto | Avoids recursive Bundle protobuf serialization complexity; keeps Stage 1 focused on resource-level serialization speed |
| `DYNAMIC_BUNDLED` linkage | Google FHIR + Protobuf + absl are ~65 MB linked dynamically; static archive caused hundreds of undefined symbols |
| Runtime dylib patching via `install_name_tool` | Bazel-built dylib has relative install-name; must be rewritten to `@rpath` for CMake-built binaries to find it |

## Build Chronology (macOS)

1. **Static (Attempt 1):** Failed — `rules_jvm_external` TLS handshake errors on legacy JDK 11
2. **Static (Attempt 2):** Switched to OpenJDK 17 → Maven TLS resolved
3. **Static (Attempt 3):** Failed — `@zlib` `fdopen` macro expansion under Xcode/Clang
4. **Static (Attempt 4, with real protobuf code):** Link closure explosion — hundreds of undefined symbols from absl/protobuf/utf8_range
5. **Resolution:** `DYNAMIC_BUNDLED` via `//cc/google/fhir:libgoogle_fhir_bundled`

## Dependencies

- **Google FHIR** — `proto/google/fhir/proto/r4/core/codes.pb.h`, `datatypes.pb.h`, `resources/observation.pb.h`, `resources/patient.pb.h`
- **Protobuf** — via Google FHIR Bazel build
- **Internal** — `harness.hpp`, `bench_test_1.hpp` (assign), `bench_test_2.hpp` (materialize), `bench_test_3.hpp` (query), `bench_test_4.hpp` (enrich)

## Output

Populates `ArmRunResult`:
- `metrics` — One `MetricEvent` per test stage
- `queried_value` — Formatted LOINC 2085-9 summary (via protobuf reflection)
- `reconstructed_bundle_json` — Informational string (`"protobuf_payload_bytes=<N>"`), not actual JSON
- `enriched_stream` — The TLV payload string with appended observation record
- `enrich_metrics_summary` — Byte-count and timing summary
