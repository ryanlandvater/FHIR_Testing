# FFHR + JSON + HL7v2 Implementation Checklist

Status legend: [ ] Not Started, [~] In Progress, [x] Done, [!] Blocked

## Scope Lock
- [x] Keep FastFHIR + JSON FHIR as baseline active arms
- [~] Re-enable HL7v2 as structured builder arm (ORU-style message object + dump)
- [ ] Extend conformance checks from two-arm parity to include HL7v2 parity gates

## Shared Assignment Layer
- [x] Add macro-based shared assignment header: bench/bench_test_1.hpp
- [x] Cover Patient + Observation + Encounter + Condition field mapping
- [ ] Expand assignment coverage to broader nested FHIR fields as needed

## FastFHIR Arm
- [x] Add bench/arm_fastfhir.cpp implementation
- [x] Test 1 serialize path implemented
- [x] Test 3 query path implemented (birthDate + LOINC 2085-9 match count)
- [x] Test 2 materialize placeholder emitted during taxonomy migration

## JSON FHIR Arm
- [x] Add bench/arm_json_fhir.cpp implementation
- [x] Stage1 serialize path implemented
- [x] Stage3 query path implemented (birthDate + LOINC 2085-9 match count)
- [ ] Stage2 transport metric decision finalized

## run_metrics + PostgreSQL
- [x] Concatenate FFHR + JSON metric vectors per measured iteration
- [x] Append concatenated metrics into benchmark_results (existing schema)
- [x] Keep DB write mode best-effort (log and continue)
- [x] Update smoke DB stage-key expectations for two-arm mode

## Verification Gates
- [x] Build bench_harness + bench_timing_conformance cleanly
- [x] bench_timing_conformance passes for FFHR + JSON
- [x] bench_harness passes without validate mismatches at 1MB
- [x] bench_harness passes without validate mismatches at 64MB
- [x] PostgreSQL row counts match two-arm expected totals

## Notes
- Current query requirement is birthDate plus LOINC 2085-9 observation match check.
- Current serialization scope includes Patient, Observation, Encounter, Condition.
- HL7v2 enablement is now in progress; this checklist tracks active migration from two-arm to three-arm runtime.
