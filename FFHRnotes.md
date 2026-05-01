# FFHR Integration Notes for FastFHIR Repo

Purpose: This document records public API and documentation issues discovered while integrating FastFHIR as an external dependency from installed artifacts only (no internal include/build path coupling).

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
