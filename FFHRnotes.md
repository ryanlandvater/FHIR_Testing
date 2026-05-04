# FFHR Integration Notes for FastFHIR Repo

Purpose: This document records public API and documentation issues discovered while integrating FastFHIR as an external dependency from installed artifacts only (no internal include/build path coupling).

Primary goal: This file is a maintainer-facing improvement guide for FastFHIR itself. Every entry should help improve one or more of:
- FastFHIR public API ergonomics,
- FastFHIR public documentation clarity,
- FastFHIR installed-artifact reliability for external consumers.

How to use this file upstream:
- Treat each issue as actionable product feedback, not local benchmark-only noise.
- Prioritize items that cause external consumer breakage or ambiguous API behavior.
- Convert resolved items into README/API docs, tests, and install-surface CI checks.

Scope: The benchmark harness in this repo intentionally consumes only FastFHIR public API surfaces and uses this integration as an ergonomics test bed.

## Integration Context

- Consumer project installs FastFHIR to `build/` and links against installed `libfastfhir`.
- Consumer include path is `build/include` (public headers only).
- Consumer should not require `.external/FastFHIR/include` or `.external/FastFHIR/generated_src` at compile time.

## Required Fixes in FastFHIR (Upstream)

### 1) Missing Installed Headers Required by Public API

Observed failure chain:

- `FastFHIR.hpp` includes `FF_Parser.hpp`.
- `FF_Parser.hpp` includes `FF_Dictionary.hpp`.
- `FF_Primitives.hpp` includes `../generated_src/FF_Recovery.hpp`.
- `FF_Dictionary.hpp` includes `FF_R4_Dictionary.hpp` and `FF_R5_Dictionary.hpp`.

Current install was missing required transitive headers for public consumption.

Fix needed upstream:

- Install all required hand-written public headers from `include/*.hpp`.
- Install required generated headers from `generated_src/*.hpp` that are transitively included by public headers.
- Ensure install-time include layout matches include directives used by installed headers.

Minimum generated set confirmed required by this consumer:

- `FF_Memory.hpp`
- `FF_Dictionary.hpp`
- `FF_R4_Dictionary.hpp`
- `FF_R5_Dictionary.hpp`
- `FF_Recovery.hpp`
- `FF_CodeSystems.hpp`
- `FF_DataTypes.hpp`
- `FF_Patient.hpp`
- `FF_Bundle.hpp`
- `FF_Observation.hpp`

Note: In practice, installing all `generated_src/*.hpp` excluding `*_internal.hpp` might be safer and less brittle than maintaining a partial allowlist. Consider this. 

### 2) Ingestor Library Symbols Not Exported in Public libfastfhir

Observed: Headers define `FastFHIR::Ingest::Ingestor` class, but linker cannot resolve symbols when attempting to instantiate or call methods on objects of this type.

Implication: `libfastfhir.dylib` (or equivalent on other platforms) does not export implementation symbols for the Ingestor class.

Workaround: Use struct-based Builder approach instead (fully available and working):
- Create struct instances representing FHIR resources (PatientData, BundleData, ObservationData, etc.)
- Use `Builder::append_obj<T>()` to serialize struct into arena
- Use `Parser` to navigate binary result (not Ingestor)

This also aligns better with the consumer's design model: treating native C++ structs as the "EHR ground truth", with both FastFHIR and JSON arms serializing from the same in-memory struct representation.

### 3) Install-Interface Include Semantics Need Cleanup

Current headers reference relative paths such as:

- `../generated_src/FF_Recovery.hpp`

This is fragile for installed consumers.

Fix needed upstream (preferred):

- Move install-facing includes to stable public include style.
- Example: include generated headers as `#include "FF_Recovery.hpp"` and ensure they are installed into the same include root (or documented subdir included by exported target).

### 3) Public Namespace/API Naming Consistency

Field key namespace in generated headers uses:

- `FastFHIR::Fields::...`

Some docs/examples and integration assumptions used `FastFHIR::FieldKeys::...`.

Fix needed upstream:

- Standardize one canonical namespace name in public docs/examples.
- If aliases are intended, provide explicit aliases and document them.

### 4) Assignment Operator Type Expectations for CODE Fields Are Underdocumented

During assignment-operator serialization path:

- Assigning `const char*` to a CODE field triggered undefined `TypeTraits<const char*>` compile error.
- Correct assignment for CODE fields was `uint32_t` dictionary code (`FF_GetDictionaryCode(...)`).

Fix needed upstream:

- Document assignment type expectations per field kind (STRING, CODE, BOOL, ARRAY, BLOCK, CHOICE).
- If string assignment to CODE fields is intended, add the supporting conversion path in public API.
- Otherwise explicitly document required dictionary code conversion.

### 5) Example Patterns Should Be Compile-Tested Against Installed Public API

Problems surfaced from example mismatch and ambiguity.

Fix needed upstream:

- Add CI examples that compile against installed artifacts only.
- Include at least these paths:
  - Struct-based path (`PatientData` append/store).
  - Reflective assignment-operator path.
  - Parse + query path from `Parser`.

### 6) Header/Library Surface Mismatch for Ingestor

Observed failure:

- `FF_Ingestor.hpp` is present in installed headers and compiles.
- Linking against installed `libfastfhir` fails with unresolved symbol for `FastFHIR::Ingest::Ingestor::ingest(...)`.

Impact:

- Installed consumers cannot rely on Ingestor even though headers advertise it.
- JSON-to-FFHR ingest pathways are blocked unless consumers bypass install artifacts.

Fix needed upstream:

- Ensure Ingestor implementation is linked into exported install libraries, or
- Export/link an additional ingest library target and document required linkage explicitly.
- Add install-surface ABI checks in CI to ensure declared public headers match exported symbols.

### 7) Compile-Pipeline Mismatch Causes Runtime Ingest Failure (Hard Blocker)

Observed and reproduced in this consumer:

- The same source file (`FF_Ingest.cpp`) and same input JSON can produce opposite runtime behavior depending on compile profile.
- Compiling with consumer-style flags/includes produced runtime ingest failure:
  - `simdjson Exception: CAPACITY: This parser can't support a document that big`
- Compiling with FastFHIR build-style profile succeeded on the same file and produced valid `.ffhr` output.

Key deltas identified during repro:

- Build mode/flags drift mattered (`Release` profile with `-O3 -DNDEBUG -fPIE` vs non-Release defaults).
- Include path provenance mattered (FastFHIR source/generated headers + bundled simdjson include roots vs consumer-local include assumptions).
- Required compile definitions were not reliably propagated to consumers unless manually duplicated.

Impact:

- Public integration appears successful at compile/link time but can fail at runtime on valid clinical payloads.
- This creates a high-risk false-positive integration state for downstream consumers.

Fix needed upstream:

- Export complete, authoritative compile usage requirements in FastFHIR CMake package targets:
  - required include directories,
  - required compile definitions,
  - required transitive dependencies,
  - and any profile-sensitive options that affect ABI/runtime behavior.
- Ensure installed target usage reproduces the same runtime behavior as FastFHIR's own tools.
- Add CI parity test:
  - Build a tiny external consumer against installed targets only.
  - Compile and run ingest on a representative large JSON bundle.
  - Assert runtime success and stable parsed-count semantics.

### 8) Public Install Contract Must Not Require Source-Tree Coupling

Observed integration pressure:

- Consumer had to reference source/build-tree internals (for example source include roots and generated/build include roots) to match working behavior.
- This violates the expected installed-artifact contract for third-party consumers.

Impact:

- Installed package is not self-sufficient for robust external integration.
- Consumers become tightly coupled to FastFHIR checkout/build layout.

Fix needed upstream:

- Make installed artifacts fully self-contained for all supported public APIs.
- Ensure exported CMake targets are sufficient by themselves for:
  - compile,
  - link,
  - and runtime-correct execution.
- Eliminate need for downstream use of `.external/FastFHIR/include`, `.external/FastFHIR/generated_src`, or build `_deps` include paths.

### 9) Must-Address Priority

These are not optional polish items for this consumer. They are integration blockers:

- Ingestor surface/link parity,
- compile-profile/runtime parity,
- and install-contract self-sufficiency.

Upstream should treat these as release-gating for external package quality.

## Usage Guidance Confirmed by This Consumer

### Ground Truth In-Memory Model

- `PatientData` is the canonical C++ in-memory source for benchmark tests.
- All arms should derive payload work from the same in-memory ground truth object.

### Parity Serialization Path

Even though FastFHIR can serialize `PatientData` in one call, this consumer intentionally uses assignment operators for parity with other systems under test.

Important caveat:

- Start from a valid `PatientData` scaffold object and then assign fields on the handle.
- Avoid creating a raw object shell in ways that produce field/type mismatch during pointer amendment.

## Additional Documentation Gaps to Address Upstream

- Clarify when to use:
  - struct append/store (`append_obj(PatientData)`)
  - reflective assignment operators (`handle[key] = value`)
- Clarify required conversions for code systems (enum -> literal -> dictionary code where required).
- Clarify transitive public header dependencies and install contract.
- Clarify which generated headers are part of stable public API.

## Maintenance Rule (Important)

Whenever integration is blocked by unclear docs, ambiguous behavior, or missing public API details:

1. Add the issue to this file immediately.
2. Record exact symptom, root cause, and the upstream fix needed.
3. Keep this file current as the primary ergonomics feedback log for FastFHIR.

This rule is mandatory for this repository.

## Current Status, Resolutions, and Open Questions (2026-05-04)

Consumer direction received:
- FFHR arm should be a thin caller only.
- CODE assignment semantics are confirmed as:
  - dictionary lookup first,
  - custom-string fallback with `FF_CUSTOM_STRING_FLAG` and relative pointer,
  - empty string => `FF_CODE_NULL`.

README confirmation received:
- The above CODE semantics are formally documented in FastFHIR README (Code Assignment Semantics section).
- Choice write/read model is also documented in README (10-byte variant slot + tag; scalar inline, complex via offset).

Resolved in consumer implementation (now guidance for upstream docs/tests):
- Unified assignment architecture is now enforced in consumer harness:
  - FFHR arm is thin caller.
  - stream-first patient assignment helper lives in shared assignment layer.
- CODE custom-string relative-offset math was aligned to object-header base semantics.
- CHOICE assignment path now uses `Builder::amend_variant(...)` for scalar and string-backed variants.

Upstream documentation improvements implied by these resolutions:
- Add a direct reflective-assignment recipe for CODE fields (dictionary/custom/null path).
- Add a direct reflective-assignment recipe for CHOICE fields (`amend_variant` mental model, scalar vs offset payload examples).
- Add explicit guidance on parity-friendly architecture (thin arm callers, centralized assignment path).

These were observed while implementing the requested stream-first patient assignment pattern:

1) CODE field assignment via `MutableEntry` remains ergonomically confusing

- `MutableEntry` validation for CODE fields expects `RECOVER_FF_CODE`.
- `TypeTraits<std::string_view>` is `RECOVER_FF_STRING`, so direct string assignment to CODE fields fails schema validation.
- `MutableEntry` arithmetic assignment writes raw scalar bytes and does not appear to perform dictionary encoding for CODE values.
- Public API does not provide an obvious `amend_code(...)` equivalent.

Resulting confusion:
- What is the intended public assignment-operator path for CODE fields in reflective mutation?

2) CHOICE field assignment behavior remains under-specified at the `MutableEntry` level

- Patient `deceased[x]` / `multipleBirth[x]` use CHOICE layout.
- `MutableEntry` arithmetic assignment can set scalar CHOICE values and tag.
- Non-arithmetic assignment route does not clearly provide CHOICE tag patching behavior for string/date-like variants.
- Key metadata for these CHOICE fields currently advertises `RECOVER_FF_BOOL`, which is ambiguous for string/date variants.

Resulting confusion:
- What is the canonical reflective assignment workflow for CHOICE values that are string/date/dateTime (not numeric/bool)?

3) Nested object append safety from parsed source structs is under-specified

- Directly appending nested objects copied from parsed fixtures can preserve internal references in ways that may require explicit deep-clone assignment logic.
- Existing `bench_assign.hpp` already has deep-clone routines for parity.

Resulting confusion:
- Is appending nested structs taken from parser output guaranteed safe, or is deep-clone assignment required before append in reflective workflows?

4) Unified assignment location expectation (resolved in consumer)

- Benchmark design intent is unified assignment semantics in `bench_assign.hpp`.
- Initial refactor introduced patient stream assignment logic directly inside `bench/arm_fastfhir.cpp`, which conflicts with that design intent.

Resulting confusion:
- Should stream-style FFHR assignment helpers be moved into `bench_assign.hpp` under `#if defined(ARM_FASTFHIR)` so both FFHR and JSON arms stay structurally unified?

Status update:
- This has now been implemented: patient stream-assignment helper moved to `bench/bench_assign.hpp`, and `bench/arm_fastfhir.cpp` now calls it as a thin delegator.

5) Runtime crash still present with broad patient stream assignment (active blocker)

Observed:
- Build succeeds.
- `bench_timing_conformance` passes.
- Minimal `bench_harness` run still segfaults during 1 MB case.

Resulting confusion:
- Which specific patient field family is currently causing invalid memory state in this reflective assignment path?
- Candidate risk areas remain: CHOICE non-scalar handling, contained/resource-reference semantics, or one/more deep object array assignment paths.

Post-README implementation update:
- `bench_assign.hpp` now aligns CODE custom-string relative-offset math to object header base.
- `bench_assign.hpp` now uses `Builder::amend_variant(...)` for scalar and string-backed CHOICE variants.
- Build still succeeds, but `bench_harness --iterations 1 --warmup-iterations 0 --runs 1` still segfaults in 1 MB case.

Upstream value of this blocker:
- This is now a concrete candidate for FastFHIR reflective-assignment robustness hardening and/or documentation of unsupported mutation combinations.
- Recommended upstream follow-through: add a reproducer test that exercises stream-first reflective patient assignment with mixed scalar, code, block, array, and choice fields.
