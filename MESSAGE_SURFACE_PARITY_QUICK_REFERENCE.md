# Message Surface Parity — Quick Reference Summary

**Generated**: 2026-05-06  
**Status**: 🔴 CRITICAL GAPS IDENTIFIED

---

## TL;DR: Key Findings

| Finding | Impact | Severity |
|---------|--------|----------|
| **Google FHIR Stage 2/3 not implemented** | Test 2/3 return 0 ns; unfair comparison | 🔴 CRITICAL |
| **Only 3/28 resources tested** | 89% of FastFHIR capability unmeasured | 🔴 CRITICAL |
| **Google FHIR proto coverage unknown** | Can't assess parity for 25+ resources | 🔴 HIGH |
| **Encounter/Condition/Procedure missing** | Core clinical workflows untested | 🔴 HIGH |
| **No R5 support** | Future-proofing gap | 🟡 MEDIUM |

---

## Message Surfaces: FFHR Coverage vs Google FHIR

### Currently Tested (3/28)
✅ **Patient** — FastFHIR + Google FHIR (Stage 1 only)  
✅ **Observation** — FastFHIR + Google FHIR (Stage 1 only)  
✅ **Bundle** — FastFHIR + Google FHIR (Stage 1 only)  

### BLOCKED for Google FHIR (0 resources)
🔴 **Stage 2/3 not implemented** — Returns zero metrics  
🔴 **Deserialization untested** — Protobuf → message conversion invisible  
🔴 **Query performance untested** — Field traversal unmeasured  

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

### Issue #1: Google FHIR Stages 2 & 3 Are Stubbed
**What's Broken**:
```cpp
// arm_google_fhir.cpp — Currently:
out.metrics.push_back({"google_fhir", Stage::Test2Materialize, 0});     // ❌ Wrong
out.metrics.push_back({"google_fhir", Stage::Test3Query, 0});            // ❌ Wrong
```

**Why It Matters**: Cannot compare deserialization speed or query latency against FastFHIR.

**Fix Priority**: 🔴 MUST FIX BEFORE PUBLICATION

**Effort**: ~80 hours

---

### Issue #2: Only 3 Resources Tested (10.7% Coverage)

**What's Missing**:
```
Tested:      Patient, Observation, Bundle (3/28)
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

