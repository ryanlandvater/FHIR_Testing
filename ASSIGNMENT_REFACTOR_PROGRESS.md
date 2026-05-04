# Assignment Refactor Progress

Date: 2026-05-04
Scope: FFHR + JSON only (no protobuf/HL7v2)

## Goal
- Keep unified assignment semantics in `bench_test_1.hpp`.
- Use stream-first FFHR pattern (`append_obj(blank)` + field-level assignment) without shortcuts.

## Current Status
- Implemented: patient stream-first assignment helper in `bench/bench_test_1.hpp` (unified parity layer).
- Implemented: `bench/arm_fastfhir.cpp` reduced to thin caller for patient assignment (`assign::append_patient_stream(...)`).
- Implemented: architecture note in `BENCHMARK_BIG_PICTURE.md` requiring stream-first FFHR assignment.
- Implemented: README-aligned CODE assignment behavior in helper (dictionary -> custom-string + flag -> null sentinel).
- Implemented: README-aligned CHOICE assignment behavior in helper via `Builder::amend_variant(...)` for scalar and string-backed variants.
- Observed: runtime instability still present with broad patient reflective assignment.
- Current runtime state: broad patient stream assignment in unified layer still segfaults in `bench_harness` 1 MB run.

## What Is Confirmed
- `bench_timing_conformance` passes after stream-first patient path changes.
- Full `bench_harness` run with broad patient reflective assignment currently fails (segfault).
- Compile is clean with centralized helper and thin-caller architecture.
- Segfault persists even after CODE/CHOICE semantic alignment with README.

## Open Confusions (mirrored in FFHRnotes.md)
1. CODE field reflective assignment path is unclear (`RECOVER_FF_CODE` expectations vs string TypeTraits behavior).
2. CHOICE field reflective assignment for non-scalar variants is unclear.
3. Safety of appending nested parsed structs without deep-clone assignment is unclear.
4. Exact placement of FFHR stream-assignment logic: should live in `bench_test_1.hpp` to preserve unified-assignment design.

## Next Step (blocked on clarification)
- Isolate the crashing field family in unified patient stream assignment, then restore full assignment coverage safely.

## Most Likely Immediate Diagnostic
- Add temporary field-family toggles (code, choice, blocks, each array family) in patient helper and run a deterministic isolation sweep to identify exactly which assignment causes the crash.
