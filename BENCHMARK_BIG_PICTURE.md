# FHIR Benchmark Harness: Architecture & Implementation Guide

## Purpose
This repository benchmarks how the same clinical payload is serialized, deserialized, and queried across multiple technologies. To ensure absolute fairness, we simulate an Electronic Medical Record (EHR) system by holding clinical data in pure C++ POCOs (Plain Old C++ Objects). 

We will use FastFHIR's `*Data` structs as our POCOs because they strictly conform to the FHIR specification. 
```cpp
struct BundlePatient {
    PatientData patient;
    std::vector<ObservationData> observations;
    std::vector<EncounterData> encounters;
    std::vector<ProcedureData> procedures;
    std::vector<ConditionData> conditions;
    std::vector<MedicationData> medications;
    std::vector<MedicationRequestData> medicationrequests;
    std::vector<MedicationStatementData> medicationstatements;
    std::vector<ImmunizationData> immunizations;
    std::vector<AllergyIntoleranceData> allergies;
};
```

## The Four Benchmark Arms
1. **FastFHIR**# FHIR Benchmark Harness: Architecture & Implementation Guide

## Purpose
This repository benchmarks how the same clinical payload is serialized, deserialized, and queried across multiple technologies. To ensure absolute fairness, we simulate an Electronic Medical Record (EHR) system by holding clinical data in pure C++ POCOs (Plain Old C++ Objects) using FastFHIR's `*Data` structs. 

```cpp
#pragma once
#include "FF_Patient.hpp"
#include "FF_Encounter.hpp"
#include "FF_Observation.hpp"
#include "FF_Condition.hpp"
#include "FF_Procedure.hpp"
#include <vector>

namespace bench {
    struct BundlePatient {
        PatientData patient;
        std::vector<ObservationData> observations;
        std::vector<EncounterData> encounters;
        std::vector<ProcedureData> procedures;
        std::vector<ConditionData> conditions;
        // Include medications, immunizations, etc.
    };
}
```

## The Four Benchmark Arms
1. **FastFHIR**
2. **JSON** (nlohmann)
3. **Google FHIR** (Protobuf)
4. **HL7v2** (Custom ZPV flattening)

## The Core Benchmark Lifecycle (Strict Bottleneck)
Every arm must execute a strict `POCO -> Bytestream -> POCO` round-trip. Only the `serialize`, `deserialize`, and `query` steps are timed.

```cpp
namespace bench {
    // Serialization bottlenecks templated on the output container type 
    // (e.g., std::string for JSON/HL7, FastFHIR::Memory for FFHR)
    template <typename BufferType>
    struct {BufferType, run_metrics} serialize(const BundlePatient& source);

    template <typename BufferType, typename NativeDom>
    {NativeDom, run_metrics} deserialize(const BufferType& buffer);
    
    template <typename NativeDom>
    run_metrics query(const NativeDom& dom);
}
```

1. **Setup (Untimed):** Ingest a Synthea JSON file into the `BundlePatient` POCO struct.
2. **Serialize (Timed):** Converts the POCO into the library's native bytestream container. returns a string view of it. 
3. **Deserialize (Timed):** Parses the bytestream into the library's native memory model/DOM. Do *not* convert back to POCOs here. We are only timing the parse speed.
4. **Query (Timed):** Search the native DOM for the Patient's `BirthDate` and the LOINC code for Cholesterol (`2085-9`).
5. **Validate (Untimed):** Convert the `NativeDom` back into a new `BundlePatient` struct and verify it matches the original source data.

## The Shared Traversal Contract
To guarantee that no arm cheats by skipping data or altering the flow, **all serialization must use a single, shared assignment header** (`bench_assign.hpp`). 

### FFHR Assignment Rule (Mandatory)
For the FFHR arm, do not serialize by first cloning the full source POCO into a new `*Data` struct and then calling `append_obj(clone)`.

Required pattern:
1. Create a blank stream object first, e.g. `auto patient_stream = builder.append_obj(PatientData{});`
2. Assign scalar/code/string fields directly via field keys, e.g. `patient_stream[FastFHIR::Fields::PATIENT::ID] = src.id;`
3. For object arrays, write each derived object to the stream and then assign the resulting offset array through `MutableEntry`, or patch array entries via `MutableEntry` flow.
4. For block/object fields, assign typed child objects through `MutableEntry` assignment.

This rule keeps assignment semantics explicit and parity-auditable, and avoids hidden whole-object copy behavior in timed serialization paths.

You must use `#ifdef` macros to interleave the library-specific assignment code block by block. This ensures absolute, side-by-side visual parity. The build system will compile the harness multiple times with the respective `-DARM_FASTFHIR`, `-DARM_JSON`, `-DARM_PROTO`, or `-DARM_HL7V2` flags. See 

```cpp
#pragma once
#include "FF_Patient.hpp"
#include <string>

namespace bench {

template <typename TargetDom>
void assign_patient(const PatientData& source, TargetDom& target) {
    if (!source.id.empty()) {
#if defined(ARM_FASTFHIR)
        target.id = source.id; // Assigning to FastFHIR C++ POCO before builder.append_obj()
#elif defined(ARM_JSON)
        target["id"] = source.id;
#elif defined(ARM_PROTO)
        target.mutable_id()->set_value(std::string(source.id));
#elif defined(ARM_HL7V2)
        target.push_back({"id", std::string(source.id)}); // ZPV snapshot vector
#else
        static_assert(false, "Benchmark arm not defined");
#endif
    }

    if (!source.name.empty()) {
#if defined(ARM_FASTFHIR)
        target.name.reserve(source.name.size());
#elif defined(ARM_JSON)
        target["name"] = nlohmann::json::array();
#endif
        for (const auto& name_src : source.name) {
#if defined(ARM_FASTFHIR)
            HumanNameData name_dst{};
            if (!name_src.family.empty()) name_dst.family = name_src.family;
            target.name.push_back(std::move(name_dst));
#elif defined(ARM_JSON)
            nlohmann::json j_name;
            if (!name_src.family.empty()) j_name["family"] = name_src.family;
            target["name"].push_back(std::move(j_name));
#elif defined(ARM_PROTO)
            auto* p_name = target.add_name();
            if (!name_src.family.empty()) p_name->mutable_family()->set_value(std::string(name_src.family));
#elif defined(ARM_HL7V2)
            if (!name_src.family.empty()) target.push_back({"name.family", std::string(name_src.family)});
#endif
        }
    }
}

} // namespace bench
```


2. **JSON** (nlohmann)
3. **Google FHIR** (Protobuf)
4. **HL7v2** (Custom ZPV flattening)

## The Core Benchmark Lifecycle
Every arm must execute the following lifecycle. Only the `serialize`, `deserialize`, and `query` steps are timed. 
```cpp
namespace bench {
    // Serialization bottlenecks templated on the output container type
    template <typename BufferType>
    BufferType serialize(const BundlePatient& source);

    template <typename BufferType, typename NativeDom>
    NativeDom deserialize(const BufferType& buffer);
}
```

1. **Setup (Untimed):** Ingest a Synthea JSON file into the `BundlePatient` POCO struct.
2. **Serialize (Timed):** Converts the POCO into the library's native bytestream (e.g., `std::string` for JSON, `FastFHIR::Memory` for FFHR).
3. **Deserialize (Timed):** Parses the bytestream into the library's native memory model/DOM. Do *not* convert back to POCOs here. We are only timing the parse speed.
4. **Query (Timed):** Search the native DOM for the Patient's `BirthDate` and the LOINC code for Cholesterol (`2085-9`).
5. **Validate (Untimed):** Convert the `NativeDom` back into a new `BundlePatient` struct and verify it matches the original source data to ensure no shortcuts were taken.

## The Shared Traversal Contract (CRITICAL)
To guarantee that no arm cheats by skipping data, **all serialization must use a single, shared template traversal header** (`bench_traverse.hpp`). 

Do not use `#ifdef` macros to interleave library-specific code. Use a Template Writer pattern to cleanly separate the FHIR traversal logic from the underlying memory allocation mechanics. 

```cpp
namespace bench {
    template <typename Writer>
    void traverse_patient(Writer& w, const PatientData& p) {
        w.write_string("id", p.id);
        w.write_bool("active", p.active);
        
        w.begin_object_array("name");
        for (const auto& name : p.name) {
            w.begin_object();
            w.write_string("family", name.family);
            // ... traverse other name fields
            w.end_object();
        }
        w.end_object_array();
    }

    template <typename Writer>
    void traverse_bundle(Writer& w, const BundlePatient& bundle) {
        w.begin_bundle();
        
        w.begin_entry("Patient");
        traverse_patient(w, bundle.patient);
        w.end_entry();

        for (const auto& obs : bundle.observations) {
            w.begin_entry("Observation");
            // traverse_observation(w, obs);
            w.end_entry();
        }
        // ... traverse other resource vectors
        
        w.end_bundle();
    }
}
```

```cpp
// The Writer Concept - All arm-specific writers MUST implement exactly these methods, no more, no less.
struct WriterConcept {
    void begin_bundle();
    void end_bundle();
    void begin_entry(const char* resource_type);
    void end_entry();
    
    void begin_object();
    void end_object();
    void begin_object_array(const char* key);
    void end_object_array();
    
    void begin_string_array(const char* key);
    void write_array_string(std::string_view val);
    void end_string_array();
    
    void write_string(const char* key, std::string_view val);
    void write_bool(const char* key, bool val);
    void write_enum(const char* key, int val); // Or specific enum types
};
```
## Arm-Specific Writers
Each arm implements a Writer that conforms to the template interface, handling memory allocation in the way that is optimal for that specific technology.

* **FastFHIRWriter:** Records offsets and assigns them to the FastFHIR builder.
* **JsonWriter:** Maintains a pointer to the current `nlohmann::json` object and assigns fields directly.
* **ProtobufWriter:** Calls `add_...()` and `set_...()` mutators on the Google FHIR generated classes.
* **HL7v2Writer:** Concatenates strings into flattened ZPV segments.


## CRITICAL CONSTRAINTS:
I am building a strict C++ benchmark. I have a shared traversal template that relies on a Writer pattern. 
I need you to implement the `JsonWriter` [or `FastFHIRWriter`, etc.] that fulfills the `WriterConcept` defined above.
1. MINIMAL CODE: Implement the methods inline within the struct. Do not write any external helper classes, factories, or utilities.
2. FLAT HIERARCHY: Keep the code flow entirely linear and easy to trace from the entry point. 
3. NO DRIFT: Do not add any methods to the struct that are not explicitly listed in the `WriterConcept`.
4. If a method requires managing state (like a stack for nested JSON objects or FastFHIR offsets), use standard `std::vector` inside the struct.

Write ONLY the struct implementation.

## Non-Goals
This harness is not intended to be a full production ETL pipeline or complete interoperability validator. It is a controlled, high-performance benchmarking environment focused exclusively on comparative behavior under repeatable conditions.