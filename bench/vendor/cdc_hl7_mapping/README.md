# Vendored FHIR -> HL7 v2 mapping (CDC PRIME)

Source: https://github.com/CDCgov/prime-fhir-converter (Apache-2.0), the CDC
Pandemic-Ready Interoperability Modernization Effort's FHIR-to-v2 converter --
the library ReportStream uses to deliver ELR to public health agencies that
cannot ingest FHIR. It is the authoritative artifact for this direction: nearly
every other open-source converter (LinuxForHealth, Microsoft FHIR-Converter,
HL7's own v2-to-FHIR IG) runs v2 -> FHIR, because FHIR is the superset and that
direction is the one with a normative mapping.

Files, taken verbatim from `src/main/resources/metadata/hl7_mapping/`:

| file | upstream path | governs |
|---|---|---|
| `OBX.yml`      | `resources/Observation/OBX.yml`      | Observation -> OBX field assignment |
| `OBXValue.yml` | `resources/Observation/OBXValue.yml` | `Observation.value[x]` -> OBX-5, by datatype |
| `CWE.yml`      | `datatypes/codeableConcept/CWE.yml`  | CodeableConcept -> CWE component order |

These are **reference, not build inputs** -- nothing compiles them. They are
here so the v2 arm's field mapping can be checked against a published authority
instead of against whatever its author assumed, and so a reviewer can see the
provenance of each OBX field the arm writes.

## Why the converter itself is not linked in

Two reasons, and the second is the substantive one.

1. It is Kotlin/JVM. This benchmark is a single C++ process built with Bazel at
   `--compilation_mode=opt`; hosting a JVM inside one arm would measure the JVM.

2. **It cannot convert native FHIR.** Every one of the 18 conditions in
   `OBXValue.yml` selects the OBX-5 datatype by reading an extension
   (`rsext-obx-observation`, `OBX.2`) that carries the ORIGINAL HL7 v2 value
   type:

       condition: '%context.extension(%`rsext-obx-observation`)
                    .extension.where(url = "OBX.2").value = "NM"'

   So the converter is built for round-tripping data that came FROM v2 and is
   going BACK to v2 -- "incremental modernization", which is what it is named
   for. Given FHIR that was never v2 (Synthea, and this corpus), those
   extensions are absent, every condition is false, and OBX-5 comes out empty.

   That is not a defect in the converter; it is the shape of the problem. There
   is no complete FHIR -> v2 mapping, because v2 has no home for most of FHIR.
   Anything claiming to be one is either inferring the v2 datatype from the
   FHIR datatype (what this arm now does, following the correspondence these
   files encode) or dropping what does not fit.

## What the arm takes from them

The datatype -> OBX-2 correspondence and the component orders, applied by
inferring OBX-2 from the FHIR datatype rather than reading it from an
extension. `assign_observation_value` in `bench/bench_test_1.hpp` cites the
specific rule it follows for each branch.
