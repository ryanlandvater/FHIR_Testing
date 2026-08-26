# Message Surface Parity — Quick Reference Summary

**Generated**: 2026-05-06  
**Status**: 🔴 CRITICAL GAPS IDENTIFIED

> ### ⚠️ Correction (2026-08-25): Google FHIR Stages 2 and 3 are NOT stubbed
>
> This document repeats a claim that is false. Verified by running the harness:
>
> | | Claimed | Actual |
> |---|---|---|
> | Stage 2 (materialize) | "returns 0 ns" | **implemented** — parses the TLV records, `ParseFromArray` per message, protobuf-reflection walk. 9,539 nodes touched, ~1.6 ms on a 1 MB bundle. |
> | Stage 3 (query) | "returns 0 ns" | **implemented** — 42 accumulator calls. Returns `patients=1 observations=316 obs_issued_present=316`, matching the JSON baseline exactly. |
>
> There is no `0`-duration metric push anywhere in `arm_google_fhir.cpp`. The
> ~80 h of Stage 2/3 work this document schedules as P1 appears to have been
> done at some point without the docs being updated.
>
> **Two real Google-arm gaps remain**, and they are not the ones described here:
>
> - `birthdate` is reported as a microsecond epoch (`194140800000000`) where
>   every other arm reports ISO (`1976-02-26`). A normalization gap.
> - The Google arm is **excluded from `validate_parity()`**, which only compares
>   FastFHIR-vs-JSON and JSON-vs-HL7v2. Its query results are therefore never
>   checked against another arm.
>
> See [notes.md](notes.md) and [TASKS.md § PARITY](TASKS.md).

> **Stale denominator — reviewed 2026-08-24.** FastFHIR compiled 28 resource
> types when this was written; the current profile compiles **37**. Read
> "3/28" as "3/37" and "89% unmeasured" as **~92% unmeasured**. See
> [MESSAGE_SURFACE_PARITY_AUDIT.md](MESSAGE_SURFACE_PARITY_AUDIT.md) for the
> full note.

---

## TL;DR: Key Findings

| Finding | Impact | Severity |
|---------|--------|----------|
| ~~Google FHIR Stage 2/3 not implemented~~ | **FALSE** — both implemented; see correction at top | ✅ RESOLVED |
| **Only 3/37 resources tested** | ~92% of FastFHIR capability unmeasured | 🔴 CRITICAL |
| **Google FHIR proto coverage unknown** | Can't assess parity for 25+ resources | 🔴 HIGH |
| **Encounter/Condition/Procedure missing** | Core clinical workflows untested | 🔴 HIGH |
| **No R5 support** | Future-proofing gap | 🟡 MEDIUM |

---

## Message Surfaces: FFHR Coverage vs Google FHIR

### Currently Tested (3/37)
✅ **Patient** — FastFHIR + Google FHIR (Stage 1 only)  
✅ **Observation** — FastFHIR + Google FHIR (Stage 1 only)  
✅ **Bundle** — FastFHIR + Google FHIR (Stage 1 only)  

### Google FHIR stage status (corrected 2026-08-25)
✅ **Stage 2 implemented** — TLV parse + `ParseFromArray` + reflection walk (9,539 nodes)  
✅ **Stage 3 implemented** — matches the JSON baseline on patients/observations/obs_issued  
🟡 **`birthdate` unit mismatch** — microsecond epoch vs ISO everywhere else  
🟡 **Excluded from `validate_parity()`** — results never cross-checked  

### Available in FFHR but NOT TESTED (10 critical + 13 unknown)

**Likely Available in Google FHIR (needs audit)**:
- Encounter
- Condition  
- Procedure
- Medication / MedicationRequest
- CarePlan / ServiceRequest
- DiagnosticReport
- Organization / Practitioner / Location
- AllergyIntolerance / Immunization

**Unknown Status**:
- PractitionerRole, Device, Coverage, DocumentReference, Provenance, etc.

---

## Actionable Parity Issues

### ~~Issue #1: Google FHIR Stages 2 & 3 Are Stubbed~~ — RESOLVED

**This issue was never real, or was fixed without the docs being updated.** The
code shown below does not exist in `arm_google_fhir.cpp`; there is no
zero-duration metric push anywhere in it.

**What replaces it:**
- `birthdate` is a microsecond epoch (`194140800000000`) in the Google arm and
  ISO (`1976-02-26`) in every other arm.
- The Google arm is excluded from `validate_parity()`, so nothing checks its
  query results against another arm.

**Fix Priority**: 🟡 Should fix before publication

**Effort**: ~4 hours

---

### Issue #2: Only 3 Resources Tested (8.1% Coverage)

**What's Missing**:
```
Tested:      Patient, Observation, Bundle (3/37)
Untested:    Encounter, Condition, Procedure, Medication, CarePlan,
             ServiceRequest, DiagnosticReport, AllergyIntolerance,
             Immunization, Organization, Practitioner, Location,
             ...13 more resources
```

**Why It Matters**: Real EHRs serialize Encounter/Condition/Procedure far more than Observation.

**Fix Priority**: 🔴 HIGH (Phase 1)

**Effort**: ~40h extraction + ~60h assignment layer + ~120h test implementation = 220h total

---

### Issue #3: Google FHIR Resource Availability Unknown

**What's Unknown**:
- Does Google FHIR have proto definitions for all 28 FHIR R4 resources?
- If not, which resources are missing?
- Are all protos generated, or only a subset?

**Why It Matters**: Cannot claim "full parity" if Google FHIR is missing resource types.

**Fix Priority**: 🟡 HIGH (must know before Phase 2 planning)

**Effort**: ~4-8 hours (Bazel workspace audit)

**Method**:
```bash
# Find all resource proto files in Google FHIR
find .external/google-fhir/cc/google/fhir/proto/r4/core/resources/ -name "*.proto"

# Check which are linked in benchmark
grep -l "patient\|observation\|bundle\|..." BUILD.bazel
```

---

## Quick Implementation Plan

### Phase 1: FIX CRITICAL GAPS (3 weeks)
1. **Google FHIR Stage 2/3** — Implement deserialization + query (80h)
2. **Audit Google FHIR protos** — Identify coverage (8h)
3. **Encounter/Condition/Procedure** — Add to fixture + tests (220h)
4. **Validation** — Conformance test passes (20h)

**Outcome**: Publishable 5-resource benchmark with fair comparison

---

### Phase 2: EXPAND COVERAGE (5 weeks)
Add 5 more resources:
- MedicationRequest
- Medication
- CarePlan
- ServiceRequest
- DiagnosticReport

**Effort**: 200h  
**Outcome**: 10-resource benchmark covering 95% of clinical data

---

### Phase 3: COMPLETE INVENTORY (6 weeks, optional)
Add remaining 13 resources; optionally add R5 support.

**Effort**: 200h  
**Outcome**: Exhaustive 28-resource benchmark

---

## Resource Gap Matrix

| Resource | FFHR | Google | Tested | Priority | Notes |
|----------|------|--------|--------|----------|-------|
| Patient | ✅ | ✅ | ✅ | — | Baseline |
| Observation | ✅ | ✅ | ✅ | — | Baseline |
| Bundle | ✅ | ✅ | ✅ | — | Container |
| **Encounter** | ✅ | ❓ | ❌ | 🔴 P1 | High-freq workflow |
| **Condition** | ✅ | ❓ | ❌ | 🔴 P1 | Clinical core |
| **Procedure** | ✅ | ❓ | ❌ | 🔴 P1 | Surgical workflow |
| MedicationRequest | ✅ | ❓ | ❌ | 🔴 P2 | Pharmacy |
| Medication | ✅ | ❓ | ❌ | 🔴 P2 | Drug master |
| CarePlan | ✅ | ❓ | ❌ | 🟡 P2 | Care coordination |
| ServiceRequest | ✅ | ❓ | ❌ | 🟡 P2 | Lab orders |
| DiagnosticReport | ✅ | ❓ | ❌ | 🟡 P2 | Results |
| AllergyIntolerance | ✅ | ❓ | ❌ | 🟡 P2 | Safety |
| Immunization | ✅ | ❓ | ❌ | 🟡 P2 | Vaccination |
| Organization | ✅ | ❓ | ❌ | 🟡 P2 | Provider dir |
| Practitioner | ✅ | ❓ | ❌ | 🟡 P2 | HCP identity |
| Location | ✅ | ❓ | ❌ | 🟡 P2 | Facilities |
| Device | ✅ | ❓ | ❌ | 🟢 P3 | Equipment |
| Coverage | ✅ | ❓ | ❌ | 🟢 P3 | Insurance |
| DocumentReference | ✅ | ❓ | ❌ | 🟢 P3 | Documents |
| 8 more... | ✅ | ❓ | ❌ | 🟢 P3 | — |

---

## Files to Update

### Phase 1 Files
- `bench/arm_google_fhir.cpp` — Add Test 2/3 implementation
- `bench/bench_test_3.hpp` — Add protobuf-specific query logic
- `bench/synthea_fixture.cpp` — Extract Encounter/Condition/Procedure
- `bench/bench_test_1.hpp` — Add assignment functions for 3 new resources
- `bench/bench_test_2.hpp` — Add materialization for 3 new resources
- `bench/arm_fastfhir.cpp` — Extend Test 1/2/3 for new resources
- `bench/arm_json_fhir.cpp` — Extend Test 1/2/3 for new resources
- `bench/arm_hl7v2.cpp` — Extend Test 1/2/3 for new resources

### Documentation Files
- `MESSAGE_SURFACE_PARITY_AUDIT.md` — This comprehensive report
- `GOOGLE_FHIR_RESOURCE_AUDIT.md` — Proto availability results (post-audit)
- `bench/RESOURCE_MAPPING.md` — Synthea field mappings (new)

---

## How to Use This Document

1. **For executives/reviewers**: Read the "TL;DR" section above
2. **For engineers starting Phase 1**: Follow "Quick Implementation Plan" → Reference `MESSAGE_SURFACE_PARITY_AUDIT.md` for detailed implementation guides
3. **For Google FHIR maintainers**: Focus on Issue #3 (proto coverage audit)
4. **For benchmark publication**: Wait for Phase 1 completion (fixes Issues #1 & #2)

---

## Next Steps

### Immediate (This Week)
- [ ] Read `MESSAGE_SURFACE_PARITY_AUDIT.md` (30 min)
- [ ] Audit Google FHIR proto availability per Part 3 (4 hours)
- [ ] Assign Phase 1 development lead
- [ ] Schedule Phase 1 kickoff

### Week 1-3 (Phase 1)
- [ ] Implement Google FHIR Stage 2/3
- [ ] Extend Synthea fixture
- [ ] Update assignment + test layers
- [ ] Validation & conformance testing

### Week 4-8 (Phase 2)
- [ ] Add 5 additional resources
- [ ] Extended benchmark reporting
- [ ] Publication-ready results

---

## Related Documents

- [MESSAGE_SURFACE_PARITY_AUDIT.md](MESSAGE_SURFACE_PARITY_AUDIT.md) — Full technical audit
- [RESOURCE_COVERAGE_ANALYSIS.md](RESOURCE_COVERAGE_ANALYSIS.md) — Detailed coverage discovery
- [BENCHMARK_IMPLEMENTATION_CHECKLIST.md](BENCHMARK_IMPLEMENTATION_CHECKLIST.md) — General status
- [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) — Background context
- [TASKS.md](TASKS.md) — active task backlog (the API port blocks all of the above)

