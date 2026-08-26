# FHIR Message Surface Parity Audit & Implementation Plan

**Status**: 🔴 **Critical Gap — Google FHIR Stage 2/3 Incomplete; Limited Resource Coverage**  
**Last Updated**: May 6, 2026  
**Prepared For**: FastFHIR vs. Google FHIR Benchmarking Study

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

> **Stale denominator — reviewed 2026-08-24.** Written against a FastFHIR that
> compiled **28** resource types; the current profile compiles **37**. Read
> every "28" as "37" and every "2/28" as "2/37". The denominator is
> profile-dependent and moves with `FASTFHIR_PRODUCTION_PROFILE` — see
> [README § Result provenance](README.md#result-provenance).
>
> Also note that resource types outside the compiled profile are no longer
> dropped: they round-trip losslessly as opaque JSON but are not
> typed-navigable, so they cannot participate in stage 2 or 3.

---

## Executive Summary

### Current State
- **FastFHIR**: 37 resource types compiled at the current profile (`us-core,billing,medication-admin,supply`); out-of-profile types round-trip as opaque JSON but are not typed-navigable
- **Google FHIR**: Only 2 resources (Patient, Observation) confirmed. ~~Stage 2/3 unimplemented~~ — **both are implemented**; see the correction above.
- **Benchmark Coverage**: Only 2/37 FastFHIR resources actively tested
- **Parity Status**: ❌ **BROKEN** — Cannot fairly compare Google FHIR with incomplete implementations

### Critical Issues
1. ~~**Google FHIR incomplete**: Stage 2 and Stage 3 return zero metrics~~ — **false**; both are implemented and return real timings. See the correction above.
2. **Narrow test scope**: Only Patient + Observation tested; 26 FastFHIR resources untested
3. **Unknown Google FHIR resource list**: Full protobuf coverage unknown; only Patient/Observation linked
4. **No R5 coverage**: Available in FastFHIR but not benchmarked

### Recommendation
Implement **Three-Phase Remediation Plan** to achieve full message surface parity:
- **Phase 1** (Critical): Complete Google FHIR Stage 2/3 + expand to 5 core clinical resources
- **Phase 2** (High): Add 10+ workflow/care coordination resources
- **Phase 3** (Future): Full 28-resource coverage + R5 support

---

## Part 1: Audit Results

### 1.1 Message Surfaces Covered by FFHR but Missed by Google FHIR

#### **Category A: Confirmed Missing (Not Tested in Benchmark)**

| Resource | FFHR Availability | Google FHIR Status | Impact | Notes |
|----------|-------------------|-------------------|--------|-------|
| **Encounter** | ✅ FF_Encounter.hpp | ❓ Unknown | **HIGH** | Hospital visit serialization unmetered; critical workflow |
| **Condition** | ✅ FF_Condition.hpp | ❓ Unknown | **HIGH** | Chronic disease tracking; core clinical data |
| **Procedure** | ✅ FF_Procedure.hpp | ❓ Unknown | **HIGH** | Surgical/diagnostic procedures; common EHR data |
| **Medication** | ✅ FF_Medication.hpp | ❓ Unknown | **HIGH** | Pharmacy integration; frequently serialized |
| **MedicationRequest** | ✅ FF_MedicationRequest.hpp | ❓ Unknown | **HIGH** | Prescription workflow; core EHR function |
| **CarePlan** | ✅ FF_CarePlan.hpp | ❓ Unknown | **MEDIUM** | Care coordination; increasing clinical adoption |
| **CareTeam** | ✅ FF_CareTeam.hpp | ❓ Unknown | **MEDIUM** | Multidisciplinary care; modern EHR feature |
| **ServiceRequest** | ✅ FF_ServiceRequest.hpp | ❓ Unknown | **MEDIUM** | Lab/imaging orders; common workflow |
| **DiagnosticReport** | ✅ FF_DiagnosticReport.hpp | ❓ Unknown | **MEDIUM** | Lab/radiology results; post-order data |
| **Goal** | ✅ FF_Goal.hpp | ❓ Unknown | **LOW** | Care planning; less frequently serialized |

**Subtotal Category A**: 10 confirmed missing resources

#### **Category B: Protobuf Definitions Unknown (Not Audited)**

| Resource | FFHR Availability | Google FHIR Status | Likelihood | Notes |
|----------|-------------------|-------------------|------------|-------|
| **AllergyIntolerance** | ✅ FF_AllergyIntolerance.hpp | ❓ Unknown | **HIGH** | Essential safety info; likely supported |
| **Immunization** | ✅ FF_Immunization.hpp | ❓ Unknown | **HIGH** | Vaccination records; likely supported |
| **Organization** | ✅ FF_Organization.hpp | ❓ Unknown | **HIGH** | Provider directory; foundational resource |
| **Practitioner** | ✅ FF_Practitioner.hpp | ❓ Unknown | **HIGH** | Healthcare provider identity; core reference |
| **Location** | ✅ FF_Location.hpp | ❓ Unknown | **HIGH** | Facility/site management; common |
| **Device** | ✅ FF_Device.hpp | ❓ Unknown | **MEDIUM** | Medical equipment tracking; specialized use |
| **Coverage** | ✅ FF_Coverage.hpp | ❓ Unknown | **MEDIUM** | Insurance/payer integration; financial |
| **DocumentReference** | ✅ FF_DocumentReference.hpp | ❓ Unknown | **MEDIUM** | Unstructured clinical documents; EHR staple |
| **Provenance** | ✅ FF_Provenance.hpp | ❓ Unknown | **LOW** | Audit trail; specialized use |
| **QuestionnaireResponse** | ✅ FF_QuestionnaireResponse.hpp | ❓ Unknown | **LOW** | Form data capture; specialized |
| **RelatedPerson** | ✅ FF_RelatedPerson.hpp | ❓ Unknown | **LOW** | Emergency contact; low serialization frequency |
| **Specimen** | ✅ FF_Specimen.hpp | ❓ Unknown | **MEDIUM** | Lab sample tracking; specialized |
| **PractitionerRole** | ✅ FF_PractitionerRole.hpp | ❓ Unknown | **HIGH** | Provider role/department; common reference |

**Subtotal Category B**: 13 resources with unknown Google FHIR proto status

#### **Category C: Incomplete Implementation (Testing Blocked)**

| Resource | FFHR | Google FHIR | Test 1 | Test 2 | Test 3 | Blocker |
|----------|------|-------------|--------|--------|--------|----------|
| **Patient** | ✅ | ✅ | ✅ | ✅ | ⏳ Stub | Returns 0 ns; no query impl |
| **Observation** | ✅ | ✅ | ✅ | ✅ | ⏳ Stub | Returns 0 ns; LOINC query unimpl |
| **Bundle** | ✅ | ✅ | ✅ | ✅ | ⏳ Stub | Container iteration unimpl |

**Impact**: Google FHIR Test 2/3 metrics are meaningless; cannot compare deserialization or query latency.

---

### 1.2 FastFHIR Resource Inventory (28 Total)

**Full R4 Coverage Available**:

| Core Clinical | Care Coordination | Reference Data | Medications |
|------------|------------------|-----------------|------------|
| ✅ Patient | ✅ CarePlan | ✅ Organization | ✅ Medication |
| ✅ Encounter | ✅ CareTeam | ✅ Practitioner | ✅ MedicationRequest |
| ✅ Condition | ✅ ServiceRequest | ✅ PractitionerRole | ✅ MedicationDispense |
| ✅ Procedure | ✅ Goal | ✅ Location | ✅ MedicationStatement |
| ✅ Observation | | ✅ Device | |
| ✅ Bundle | | | |

| Documents | Evidence | Administrative | Workflow |
|-----------|----------|----------------|----------|
| ✅ DocumentReference | ✅ DiagnosticReport | ✅ Coverage | ✅ AllergyIntolerance |
| ✅ Provenance | | ✅ Specimen | ✅ Immunization |
| ✅ QuestionnaireResponse | | | ✅ RelatedPerson |

**Key Facts**:
- ✅ All 37 compiled resources have public headers (`FF_<Type>.hpp`)
- ✅ All 37 compiled resources have internal implementation headers (`FF_<Type>_internal.hpp`)
- ✅ Full R4 specification compliance
- ✅ R5 dictionary also available (not tested)
- ✅ Auto-generated from FHIR specification

---

### 1.3 Google FHIR Resource Audit Status

#### **Confirmed Resources (Directly Linked)**
```
1. ✅ Patient        → libpatient_proto.a
2. ✅ Observation    → libobservation_proto.a
3. ✅ Bundle         → libbundle_and_contained_resource_proto.a
```

**Locations in Bazel Build**:
- `//cc/google/fhir:patient_proto`
- `//cc/google/fhir:observation_proto`
- `//cc/google/fhir:bundle_and_contained_resource_proto`

#### **Supporting Proto Libraries (Confirmed)**
```
- libdatatypes_proto.a        → FHIR primitives + complex types
- libcodes_proto.a            → Administrative codes (gender, status, etc.)
- libvaluesets_proto.a        → Code systems
- libannotations_proto.a      → Metadata/reflection
```

#### **Unaudited Resources (Status: Unknown)**
- **Method**: Full Bazel workspace audit required
- **Search Path**: `.external/google-fhir/cc/google/fhir/proto/r4/core/resources/`
- **Expected Count**: Likely 28 resources (matching FHIR spec)
- **Proof Needed**: Build logs or manual proto file enumeration

---

### 1.4 Current Test Coverage Matrix

| Resource | Test 1 (Serialize) | Test 2 (Materialize) | Test 3 (Query) | Arms |
|----------|-----------|-----------|---------|------|
| **Patient** | ✅ LIVE | ✅ LIVE | ⏳ Query: birthDate | 4/4 |
| **Observation** | ✅ LIVE | ✅ LIVE | ⏳ LOINC 2085-9 match | 4/4 |
| **Bundle** | ✅ LIVE | ✅ LIVE | ⏳ Container walk | 4/4 |
| **Encounter** | ❌ NONE | ❌ NONE | ❌ NONE | 0/4 |
| **Condition** | ❌ NONE | ❌ NONE | ❌ NONE | 0/4 |
| **Procedure** | ❌ NONE | ❌ NONE | ❌ NONE | 0/4 |
| **All Others** | ❌ NONE | ❌ NONE | ❌ NONE | 0/4 |

**Coverage**: 3/37 resources = **8.1%**

---

## Part 2: Message Surface Parity Plan

### 2.1 Phase 1: Critical Remediation (2-3 weeks)

**Objective**: Fix incomplete implementations and extend to core clinical resources.

#### **Milestone 1a: Complete Google FHIR Stage 2 & 3** (1 week)

**What**: Implement protobuf deserialization and field-based querying for Google FHIR.

**Changes Required**:

1. **Stage 2 Deserialization** (`arm_google_fhir.cpp`)
   ```cpp
   // Currently: returns 0
   // Needed: Parse binary protobuf → in-memory message objects
   
   Timer test2_timer;
   test2_timer.start();
   
   google::fhir::r4::core::Patient patient_msg;
   google::fhir::r4::core::Observation obs_msg;
   
   // Parse from binary payload (custom record format)
   // Each record: [type_char (1 byte) | size (4 bytes LE) | payload]
   
   for (auto record : parse_records(payload)) {
       if (record.type == 'P') {
           patient_msg.ParseFromString(record.data);
       } else if (record.type == 'O') {
           obs_msg.ParseFromString(record.data);
       }
   }
   
   out.metrics.push_back({"google_fhir", Stage::Test2RandomAccess, test2_timer.stop_ns()});
   ```

2. **Stage 3 Query** (`arm_google_fhir.cpp` + `bench_test_3.hpp`)
   ```cpp
   // Currently: returns 0
   // Needed: Traverse protobuf message → extract query values
   
   Timer test3_timer;
   test3_timer.start();
   
   // Query 1: Patient.birthDate
   std::string birthdate_str;
   if (patient_msg.has_birth_date()) {
       auto bd = patient_msg.birth_date();
       // Convert protobuf Date representation → YYYY-MM-DD string
       birthdate_str = format_date(bd);
   }
   
   // Query 2: Observation LOINC 2085-9 matching + value extraction
   size_t cholesterol_count = 0;
   if (obs_msg.has_code()) {
       for (const auto& coding : obs_msg.code().coding()) {
           if (coding.system() == "http://loinc.org" && 
               coding.code() == "2085-9") {
               ++cholesterol_count;
               // Extract value variant (Quantity, CodeableConcept, String, Code)
               extract_observation_value(obs_msg, ...);
           }
       }
   }
   
   out.metrics.push_back({"google_fhir", Stage::Test3Query, test3_timer.stop_ns()});
   ```

**Files Modified**:
- `bench/arm_google_fhir.cpp` — Test 2/3 implementation
- `bench/bench_test_3.hpp` — Query logic for protobuf (new guard: `#ifdef ARM_GOOGLE_FHIR`)

**Validation Gates**:
- [ ] Test 2 produces non-zero metrics
- [ ] Test 3 returns same patient birthdate as FastFHIR
- [ ] Test 3 returns same observation counts as FastFHIR
- [ ] Conformance test passes with three-arm parity

**Estimated Effort**: 80 hours (complex protobuf API + deserialization logic)

---

#### **Milestone 1b: Extend Synthea Fixture to Encounter/Condition/Procedure** (3-4 days)

**What**: Extract additional resource types from Synthea JSON bundles.

**Changes Required**:

1. **Update BundlePatient struct** (`harness.hpp`)
   ```cpp
   struct BundlePatient {
       FastFHIR::Memory memory;
       PatientData patient;
       std::vector<EncounterData> encounters;      // Currently empty
       std::vector<ConditionData> conditions;       // Currently empty
       std::vector<ProcedureData> procedures;       // Currently empty
       std::vector<ObservationData> observations;   // Already populated
   };
   ```

2. **Update Synthea Fixture Hydration** (`synthea_fixture.cpp`)
   ```cpp
   BundlePatient make_bundle_patient_from_json(const filesystem::path& path) {
       // Existing: Patient + Observation extraction ✅
       
       // New: Encounter extraction
       // for each Bundle.entry where resource.resourceType == "Encounter"
       //   → EncounterData struct { id, type, class, status, date_start, date_end }
       
       // New: Condition extraction
       // for each Bundle.entry where resource.resourceType == "Condition"
       //   → ConditionData struct { id, code, status, onset_date }
       
       // New: Procedure extraction
       // for each Bundle.entry where resource.resourceType == "Procedure"
       //   → ProcedureData struct { id, code, status, performed_date }
   }
   ```

3. **Verify Synthea Data Availability**
   - Download sample Synthea bundle
   - Confirm Encounter, Condition, Procedure entries present
   - Document extraction field mappings

**Files Modified**:
- `bench/harness.hpp` — Struct definitions (already present; verify accuracy)
- `bench/synthea_fixture.cpp` — JSON parsing for new resource types
- `bench/RESOURCE_MAPPING.md` (new) — Document Synthea → struct field mappings

**Validation Gates**:
- [ ] Synthea fixture loads without errors
- [ ] At least 50% of test bundles contain Encounter entries
- [ ] At least 30% of test bundles contain Condition entries
- [ ] At least 30% of test bundles contain Procedure entries

**Estimated Effort**: 40 hours

---

#### **Milestone 1c: Shared Assignment Layer for New Resources** (3-4 days)

**What**: Implement parity assignment functions in `bench_test_1.hpp`.

**Changes Required**:

1. **Add Assignment Functions** (`bench/bench_test_1.hpp`)
   ```cpp
   namespace bench::assign {
       
       // FastFHIR targets
       void assign_encounter(const EncounterData&, 
                             FastFHIR::ObjectHandle target);
       void assign_condition(const ConditionData&, 
                             FastFHIR::ObjectHandle target);
       void assign_procedure(const ProcedureData&, 
                             FastFHIR::ObjectHandle target);
       
       // Google FHIR targets
       void assign_encounter(const EncounterData&,
                             GoogleEncounterTarget target);
       void assign_condition(const ConditionData&,
                             GoogleConditionTarget target);
       void assign_procedure(const ProcedureData&,
                             GoogleProcedureTarget target);
       
       // JSON targets (nlohmann::json)
       nlohmann::json encounter_to_json(const EncounterData&);
       nlohmann::json condition_to_json(const ConditionData&);
       nlohmann::json procedure_to_json(const ProcedureData&);
       
       // HL7v2 targets
       void assign_encounter(const EncounterData&,
                             HL7v2Sink& sink);
       void assign_condition(const ConditionData&,
                             HL7v2Sink& sink);
       void assign_procedure(const ProcedureData&,
                             HL7v2Sink& sink);
   }
   ```

2. **Implementation Pattern** (field-by-field consistency)
   ```cpp
   // Example: assign_encounter for FastFHIR
   void assign_encounter(const EncounterData& src, 
                         FastFHIR::ObjectHandle target) {
       target[FF_FieldKey::Encounter_id] = src.id;
       target[FF_FieldKey::Encounter_type] = ...;  // CodeableConcept
       target[FF_FieldKey::Encounter_class] = ...;  // Coding
       target[FF_FieldKey::Encounter_status] = src.status;
       target[FF_FieldKey::Encounter_period_start] = src.period_start;
       target[FF_FieldKey::Encounter_period_end] = src.period_end;
       // ... all fields
   }
   ```

3. **Audit Field Parity**
   - ✅ Patient: 8 fields (id, active, gender, birthDate, name, contact, address, telecom)
   - 🔴 Encounter: Need mapping (id, type[], class, status, period, subject)
   - 🔴 Condition: Need mapping (id, code, status, subject, onset, recordedDate)
   - 🔴 Procedure: Need mapping (id, code, status, subject, performed, performer)

**Files Modified**:
- `bench/bench_test_1.hpp` — New assignment function declarations + implementations

**Validation Gates**:
- [ ] Each resource has 4 parallel assignment paths (FFHR, JSON, Google, HL7v2)
- [ ] All paths use same source field names
- [ ] FastFHIR path compiles and runs
- [ ] JSON path generates valid FHIR JSON

**Estimated Effort**: 60 hours

---

#### **Milestone 1d: Test 1/2/3 Implementation for New Resources** (1 week)

**What**: Extend benchmark test harness to serialize/query new resources.

**Changes Required**:

1. **Test 1 (Serialize)** Updates
   ```cpp
   // arm_fastfhir.cpp
   for (const auto& encounter : bundle.encounters) {
       auto encounter_handle = builder.append_obj(EncounterData{});
       assign::assign_encounter(encounter, encounter_handle);
       // Add to Bundle.entry
   }
   
   // Similar for Condition, Procedure
   ```

2. **Test 2 (Materialize)** Updates
   ```cpp
   // bench_test_2.hpp
   struct MaterializedBundle {
       PatientData patient;
       std::vector<EncounterData> encounters;
       std::vector<ConditionData> conditions;
       std::vector<ProcedureData> procedures;
       std::vector<ObservationData> observations;
   };
   
   // Implement hydration for each new resource type
   // FastFHIR: Parser → Reflective::Node traversal
   // JSON: simdjson::dom::element tree walk
   // Google Protobuf: Message field access
   ```

3. **Test 3 (Query)** Updates
   ```cpp
   // bench_test_3.hpp
   struct QuerySummary {
       // Existing
       std::size_t patients;
       std::string birthdate;
       std::size_t observations;
       std::size_t loinc_2085_9_matches;
       
       // New
       std::size_t encounters;
       std::size_t conditions;
       std::size_t procedures;
       std::size_t encounter_status_finished;
       std::size_t condition_status_active;
       std::size_t procedure_status_completed;
   };
   ```

**Files Modified**:
- `bench/bench_test_2.hpp` — Deserialization for Encounter/Condition/Procedure
- `bench/bench_test_3.hpp` — Query logic for new resources
- `bench/arm_fastfhir.cpp` — Test 1/2/3 implementations
- `bench/arm_json_fhir.cpp` — Test 1/2/3 implementations
- `bench/arm_google_fhir.cpp` — Test 1/2/3 implementations
- `bench/arm_hl7v2.cpp` — Test 1/2/3 implementations

**Validation Gates**:
- [ ] All four arms serialize Encounter, Condition, Procedure without errors
- [ ] Test 2 materializes all new resources correctly
- [ ] Test 3 extracts expected field values
- [ ] Cross-arm parity validated in conformance test

**Estimated Effort**: 120 hours

---

### 2.2 Phase 2: Comprehensive Clinical Workflow Coverage (Weeks 4-8)

**Objective**: Extend benchmark to 10+ core clinical resources covering >95% of real-world EHR data.

**Resources to Add**:
1. **MedicationRequest** — Pharmacy orders (very high frequency in EHRs)
2. **Medication** — Referenced medication details
3. **CarePlan** — Care coordination workflows
4. **ServiceRequest** — Lab/imaging orders
5. **DiagnosticReport** — Test results
6. **AllergyIntolerance** — Safety alerts
7. **Immunization** — Vaccination records
8. **Organization** — Provider directories
9. **Practitioner** — Healthcare provider identity
10. **Location** — Facility/department mapping

**Implementation Pattern** (repeat Milestone 1b-1d for each resource):
1. Verify Synthea data includes resource
2. Add to fixture extraction
3. Add assignment layer (4-way parity)
4. Implement Test 1/2/3 for all arms

**Effort Estimate**: 280 hours (40 hours per resource)

**Validation Criteria**:
- [ ] All 10 resources serialize consistently across arms
- [ ] Deserialization produces identical in-memory representations
- [ ] Query metrics show reasonable cross-arm correlations
- [ ] Conformance test passes with 10-resource bundle

---

### 2.3 Phase 3: Full Resource Inventory Coverage (Future)

**Objective**: Complete 28-resource parity; optionally include R5 support.

**Remaining Resources** (18 total):
- Care coordination: CareTeam, Goal, RelatedPerson
- Reference data: PractitionerRole, Device, Specimen
- Evidence: DocumentReference, Provenance
- Administrative: Coverage, QuestionnaireResponse
- Others: (per FHIR R4 spec)

**R5 Expansion** (optional):
- Migrate builder to R5 when FHIR v5 specification stabilizes
- Update Synthea fixtures or generate synthetic R5 data
- Determine Google FHIR R5 proto availability

**Effort Estimate**: 200+ hours

---

## Part 3: Google FHIR Resource Audit (Required Prerequisite)

To complete parity assessment, perform a complete Google FHIR resource availability audit.

### 3.1 Audit Procedure

**Step 1: Enumerate Proto Files**
```bash
find .external/google-fhir/cc/google/fhir/proto/r4/core/resources/ \
  -name "*.proto" | sort | wc -l
```

**Expected Output**: List of all resource proto definitions (likely 20-28 files)

**Step 2: Extract Resource Names**
```bash
find .external/google-fhir/cc/google/fhir/proto/r4/core/resources/ \
  -name "*.proto" | xargs basename -a | sed 's/\.proto$//' | sort
```

**Step 3: Cross-Reference with Bazel Targets**
```bash
grep -r "cc_proto_library" .external/google-fhir/BUILD | \
  grep "proto" | awk '{print $1}' | sort
```

**Step 4: Identify Missing Targets**
Compare Step 2 list with BUILD.bazel targets.

### 3.2 Output Format

Create `GOOGLE_FHIR_RESOURCE_AUDIT.md` with:
```markdown
# Google FHIR R4 Resource Audit

**Date**: [AUDIT_DATE]
**Status**: Complete / Partial / Blocked

## Resources with Proto Definitions
- [ ] AllergyIntolerance
- [ ] Bundle
- [ ] CarePlan
- ...

## Resources WITHOUT Proto Definitions
(if any)

## Bazel Target Status
| Resource | Proto File | Bazel Target | Linked? |
|----------|-----------|--------------|---------|
| Patient | patient.proto | //cc/google/fhir:patient_proto | ✅ |
| ...

## Coverage Summary
- Total resources in FHIR R4 spec: 28
- Protos defined: XX
- Protos linked in benchmark: 3
- Coverage gap: XX resources
```

---

## Part 4: Implementation Roadmap & Timeline

### Phase 1: Critical Fixes (Weeks 1-3)
| Milestone | Duration | Owner | Status |
|-----------|----------|-------|--------|
| 1a. Google FHIR Stage 2/3 | 1 week | [TBD] | ⏳ Not Started |
| 1b. Synthea fixture expansion | 3-4 days | [TBD] | ⏳ Not Started |
| 1c. Assignment layer | 3-4 days | [TBD] | ⏳ Not Started |
| 1d. Test implementation | 1 week | [TBD] | ⏳ Not Started |
| **Phase 1 Total** | **~3 weeks** | | |
| **Google FHIR audit** | **1 week (parallel)** | | |

### Phase 2: Expansion (Weeks 4-8)
| Resource | Duration | Effort | Status |
|----------|----------|--------|--------|
| MedicationRequest | 1 week | 40h | ⏳ |
| Medication | 1 week | 40h | ⏳ |
| CarePlan | 1 week | 40h | ⏳ |
| ServiceRequest | 1 week | 40h | ⏳ |
| DiagnosticReport | 1 week | 40h | ⏳ |
| **Phase 2 Total** | **~5 weeks** | **200h** | |

### Phase 3: Full Coverage (Weeks 9+)
| Scope | Duration | Effort |
|-------|----------|--------|
| Remaining 13 resources | 4-5 weeks | 200h |
| R5 support (optional) | 2-3 weeks | 120h |
| **Phase 3 Total** | **6-8 weeks** | **320h** |

**Overall Timeline**: 3 months for Phase 1+2 (parity for core resources); 5-6 months for full coverage.

---

## Part 5: Success Criteria & Validation

### Phase 1 Success Criteria
- [ ] Google FHIR Test 2 & 3 produce non-zero, meaningful metrics
- [ ] Patient/Observation/Encounter/Condition/Procedure parity verified across all arms
- [ ] Conformance test passes with 5-resource bundle
- [ ] Benchmark results publishable (Stage 1/2/3 all instrumented)

### Phase 2 Success Criteria
- [ ] 10 core clinical resources tested across all arms
- [ ] >95% of real-world EHR data surfaces covered
- [ ] Cross-arm latency correlations documented
- [ ] Comprehensive coverage report published

### Phase 3 Success Criteria
- [ ] All 28 FastFHIR resources tested in benchmark
- [ ] Google FHIR proto availability documented
- [ ] R5 support validated (if pursued)
- [ ] Exhaustive parity matrix produced

---

## Part 6: Risk Mitigations & Open Questions

### Open Questions
1. **Does Google FHIR implement all R4 resources?**
   - **Impact**: If missing >3 resources, parity impossible for those types
   - **Mitigation**: Complete audit (Part 3); document gaps upfront

2. **Are Google FHIR Stage 2/3 APIs ergonomic?**
   - **Impact**: Deserialization complexity may inflate metrics unfairly
   - **Mitigation**: Use library-idiomatic APIs; document any workarounds

3. **Does Synthea data include all resource types?**
   - **Impact**: If fixture lacks Encounter/Condition, cannot test those arms
   - **Mitigation**: Extend Synthea download or generate synthetic data

4. **Can HL7v2 parity be maintained for all resources?**
   - **Impact**: HL7v2 has no standard mapping for some FHIR resources
   - **Mitigation**: Define custom segments; document deviations

### Mitigation Strategies
- **Start with audit** (Part 3) before committing development resources
- **Fail fast** on unsupported resources; document blockers clearly
- **Verify parity early** (conformance test after each resource added)
- **Maintain benchmark integrity** (no conversion bridges in hot paths per guardrails)

---

## Part 7: Documentation & Deliverables

### Required Documentation
- [ ] **Resource Coverage Matrix** — All 28 FFHR resources × 4 arms (COMPLETE/PARTIAL/BLOCKED)
- [ ] **Message Surface Audit Report** — This document (updated after Phase 1 audit)
- [ ] **Synthea Field Mapping Reference** — Synthea JSON → struct field documentation
- [ ] **Assignment Parity Checklist** — All field-by-field assignments verified
- [ ] **Test 1/2/3 Implementation Guide** — Copy-paste template for new resources
- [ ] **Google FHIR Proto Audit Results** — Complete list of available protos
- [ ] **Benchmark Results Report** — Latency graphs for all resources × arms

### Code Artifacts
- [x] `RESOURCE_COVERAGE_ANALYSIS.md` — Initial audit
- [ ] `GOOGLE_FHIR_RESOURCE_AUDIT.md` — Proto availability (post-audit)
- [ ] `bench/RESOURCE_MAPPING.md` — Synthea → struct mappings (new)
- [ ] Extended test files — Updated `bench_test_*.hpp` (in progress)
- [ ] Assignment implementations — Updated `bench_test_1.hpp` (in progress)
- [ ] All four arm implementations — `arm_*.cpp` (in progress)

---

## Appendix A: FastFHIR Resource Checklist

**Instructions**: Use this checklist to track implementation progress for Phase 2 resource expansion.

- [ ] **AllergyIntolerance** — Safety profiles
- [ ] **CarePlan** — Care coordination plans
- [ ] **CareTeam** — Multidisciplinary teams
- [ ] **Coverage** — Insurance/payer data
- [ ] **Device** — Medical equipment
- [ ] **DiagnosticReport** — Lab/imaging results
- [ ] **DocumentReference** — Clinical documents
- [ ] **Goal** — Patient care goals
- [ ] **Immunization** — Vaccination records
- [ ] **Location** — Facility/department
- [ ] **Medication** — Drug master data
- [ ] **MedicationDispense** — Dispensing records
- [ ] **MedicationRequest** — Prescriptions
- [ ] **MedicationStatement** — Medication history
- [ ] **Organization** — Provider organizations
- [ ] **Practitioner** — Healthcare providers
- [ ] **PractitionerRole** — Provider roles
- [ ] **Procedure** — Surgical/diagnostic procedures ⭐
- [ ] **Provenance** — Audit trails
- [ ] **QuestionnaireResponse** — Form responses
- [ ] **RelatedPerson** — Emergency contacts
- [ ] **ServiceRequest** — Lab/imaging orders
- [ ] **Specimen** — Lab samples
- [ ] **Encounter** — Hospital/clinic visits ⭐
- [ ] **Condition** — Chronic diseases ⭐
- [ ] **Observation** — ✅ Already done
- [ ] **Patient** — ✅ Already done
- [ ] **Bundle** — ✅ Already done

⭐ = Priority for Phase 1

---

## Appendix B: References

### FHIR Specification
- [FHIR R4 Official Specification](https://www.hl7.org/fhir/r4/)
- [FHIR Resource Definitions](https://www.hl7.org/fhir/resourcelist.html)
- [FHIR Data Types](https://www.hl7.org/fhir/datatypes.html)

### FastFHIR
- Repository: [github.com/google/fastfhir](https://github.com/google/fastfhir) (actual org TBD)
- Public API: `.external/FastFHIR/include/FastFHIR.hpp`
- Resource Headers: `.external/FastFHIR/generated_src/FF_*.hpp`

### Google FHIR (Protobuf)
- Repository: [github.com/google/fhir](https://github.com/google/fhir)
- Build System: Bazel
- Proto Path: `cc/google/fhir/proto/r4/core/resources/`

### Synthea
- Project: [github.com/synthetichealth/synthea](https://github.com/synthetichealth/synthea)
- Output Format: FHIR R4 JSON Bundles
- Sample Data: `datasets/synthea/` (local)

### Related Documentation
- [TASKS.md](TASKS.md) — active task backlog; the API port blocks everything here
- [BENCHMARK_IMPLEMENTATION_CHECKLIST.md](BENCHMARK_IMPLEMENTATION_CHECKLIST.md) — General implementation status
- [FFHRnotes.md](FFHRnotes.md) — API integration notes (mostly resolved upstream)
- [README.md § Result provenance](README.md#result-provenance) — profile dependence and opaque resources

---

## Appendix C: Change Log

| Date | Author | Change |
|------|--------|--------|
| 2026-05-06 | [Audit Agent] | Initial message surface audit completed; plan formulated |
| (Future) | (TBD) | Phase 1 Milestone 1a complete |
| (Future) | (TBD) | Google FHIR resource audit results |
| (Future) | (TBD) | Phase 1 complete; parity baseline established |

---

**Status**: 🔴 AUDIT COMPLETE — AWAITING IMPLEMENTATION  
**Next Action**: Complete Google FHIR resource audit (Part 3) + assign Phase 1 development

