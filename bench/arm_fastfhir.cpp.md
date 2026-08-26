# `arm_fastfhir.cpp` — FastFHIR Serialization Arm

> ⚠️ **Ported 2026-08-25 — builds, but Test 1 no longer uses the shared
> assignment layer.** `Builder::set_root`/`finalize` are private, so sealing goes
> through `seal_stream()`. More importantly, this arm now calls
> `append_obj(item.patient)` / `append_obj(*observation)` on the **whole POCO**
> rather than appending an empty resource and amending fields.
>
> Why: FastFHIR stores datatype arrays (`Observation.category`, …) as
> `FF_ARRAY::INLINE_BLOCK`, and there is **no public API** to write that layout
> field-by-field. The old path wrote an offset array instead, which the reader
> walked as inline block headers and dereferenced as garbage.
>
> **Parity cost:** this arm now serializes every POCO field; the other three
> serialize only the ~25 the shared layer covers. See
> [notes.md](../notes.md) §3 — Test 1 is not publishable in this state.

## Purpose

Implements the **FastFHIR** benchmark arm: serializing in-memory `PatientData` and `ObservationData` structs into the FastFHIR binary arena format (`FastFHIR::Memory`). This is the primary arm being benchmarked — the one whose performance is compared against JSON, HL7v2, and Google Protobuf.

## How It Accomplishes Its Purpose

### Stage 1 (Serialize) — `run_fastfhir_bundle()`

1. **Arena Sizing**: Computes `arena_hint` from `4096` base + sum of all `BundlePatient::memory.capacity()` values — each patient's memory holds the original FFHR ingest output. This ensures the output arena has enough room for the combined bundle.

2. **Memory + Builder Creation**: Creates a `FastFHIR::Memory::create(arena_hint)` arena and a `FastFHIR::Builder(payload_memory, FHIR_VERSION_R5)` that writes FFHR binary into it.

3. **Bundle Header**: Sets `BundleData.type = BundleType::Collection`.

4. **Patient Entry Construction**: Iterates over `fixture.bundle`, for each patient:
   - Calls `builder.append_obj(PatientData{})` to create a FastFHIR binary object handle
   - Calls `assign::assign_patient(item.patient, patient_handle)` to copy struct fields into the binary arena
   - Stores the resulting `BundleentryData` with a `ResourceReference` cast from the handle

5. **Observation Flattening**: Gathers all `ObservationData*` pointers across all patients into a flat `observation_ptrs` vector.

6. **Observation Entry Construction**: Same pattern — `builder.append_obj(ObservationData{})` + `assign::assign_observation()`.

7. **Entry Concatenation**: Merges patient + observation entries into a single `entries` vector, assigns to `bundle.entry`.

8. **Finalization**: Writes the root `bundle` object via `builder.append_obj(bundle)`, calls `builder.set_root()`, then `builder.finalize()` with a no-op SHA256 checksum (benchmark mode — real checksums would add overhead).

### Parallel Dispatch (macOS Only, Currently Commented Out)

The code includes (but has `#if 0`-blocked) two dispatch paths:
- **macOS GCD**: `dispatch_apply_f()` with `EntryBuildContext` / `ObservationBuildContext` structs and C-linkage trampolines (`build_bundle_entry`, `build_observation_entry`)
- **C++17 PSTL**: `std::execution::par_unseq` on `std::transform`

Both are disabled — the fallback sequential `std::transform` path is active. This avoids measurement noise from thread scheduling during benchmarking.

### Stage 1b (Compact Archive) — IN-E, gated on losslessness

After the standard stream is sealed, the arm runs `Compactor::archive()` on it
and emits a `test_1_compact` row with `bytes_out` = the compact archive size.
The row is **withheld** unless the compact stream re-parses to JSON
semantically identical to the standard stream's (nlohmann comparison, not
string equality — the two layouts may legitimately order fields differently).
The gate exists because the upstream compactor silently dropped scalar arrays
until `459e8d8`; no compact number leaves without it (handoff.md Instrument E,
upstream I3.7).

### Stage 2 (Random Access) — Delegated to `bench_test_2.hpp`

The FFHR binary is passed to `test_2::random_access()` which picks N random
`Bundle.entry` ordinals and navigates to each from the root, reading the
resource's `id`. FastFHIR's per-read cost is offset arithmetic — O(1). Must run
before Test 4: the in-place enrich (PA-9) would otherwise move the ordinal
space under it (TASKS.md D4).

### Stage 3 (Query) — Delegated to `bench_test_3.hpp`

Searches the FFHR binary for:
- Patient `birthDate` fields
- Observations with LOINC code `2085-9` (Total Cholesterol)
- Value type breakdown (Quantity, CodeableConcept, String, Code)
- Effective time breakdown (dateTime vs Period)
- Component value presence and type classification

### Stage 4 (Enrich) — Delegated to `bench_test_4.hpp`

Deserializes the FFHR bundle, appends a new Observation (from `bench/enrich.json`), re-serializes. Measures the cost of "adding a lab result to an existing bundle" — a common EHR workflow.

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Sequential `std::transform` (no parallelism) | Avoids measurement noise; parallel paths are structurally preserved for verification |
| `2x` source-size arena hints | Derived from the original Synthea ingest sizing strategy — ensures no reallocation |
| No-op SHA256 checksum | Benchmarks serialization, not hashing; callback exists because `finalize()` requires it |
| `BundlePatient::memory` copied into output arena | Each input already contains FFHR binary from Synthea ingestion; output is a *new* combined bundle |

## Dependencies

- `FastFHIR` library: `FF_Bundle.hpp`, `FF_Patient.hpp`, `FF_Observation.hpp`, `FF_Compactor.hpp`, `FastFHIR.hpp`
- `harness.hpp`: BundlePatient, BundleBenchFixture, ArmRunResult, Timer
- `bench_test_1.hpp`: `assign::assign_patient()`, `assign::assign_observation()`
- `bench_test_2.hpp`: `test_2::random_access()`
- `bench_test_3.hpp`: `test_3::query()`
- `bench_test_4.hpp`: `test_4::BENCH_TEST_4_ENRICH_FN`

## Output

Returns an `ArmRunResult` containing (in push order):
- `metrics[0]`: `{"fastfhir", Test1Serialize, duration_ns}`
- `metrics[1]`: `{"fastfhir", Test1Compact, duration_ns}` — compact archive size
  (IN-E losslessness gate; withheld, with a warning, when the gate fails)
- `metrics[2]`: `{"fastfhir", Test3Query, duration_ns}`
- `metrics[3]`: `{"fastfhir", Test3QueryCompact, duration_ns}` — same census
  over the compact archive; answers must match the standard query
- `metrics[4]`: `{"fastfhir", Test2RandomAccess, duration_ns}`
- `metrics[5]`: `{"fastfhir", Test2RandomAccessCompact, duration_ns}` — same
  probe over the compact archive (bytes_out = id bytes read)
- `metrics[6]`: `{"fastfhir", Test4Enrich, duration_ns}`
- (no `Test4EnrichCompact` row — the API refuses a Builder on a compact
  archive, write-once; CAPI-10. Logged once per run.)
- `queried_value`: Formatted query summary string
- `reconstructed_bundle_json`: Empty (FFHR binary is not JSON-serializable in this context)
- `enriched_stream`: The new `FastFHIR::Memory` after enrichment
