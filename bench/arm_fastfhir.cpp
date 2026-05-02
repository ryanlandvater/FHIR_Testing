#include "harness.hpp"

#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Patient.hpp>
#include <FF_Observation.hpp>
#include <memory>
#include <variant>

namespace bench {

namespace {

template <typename T>
void assign_offset_array_if_present(
    FastFHIR::Builder& builder,
    const FastFHIR::Reflective::ObjectHandle& parent,
    FF_FieldKey field,
    const std::vector<T>& values) {
  if (values.empty()) {
    return;
  }

  std::vector<Offset> offsets;
  offsets.reserve(values.size());
  for (const auto& value : values) {
    offsets.push_back(builder.append(value));
  }

  parent[field] = offsets;
}

}  // namespace

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  // Keep arena modest by default: approximately 2x ingested source bytes.
  const auto arena_size = fixture.fastfhir_vma_bytes > 0
      ? static_cast<std::size_t>(fixture.fastfhir_vma_bytes)
      : static_cast<std::size_t>(std::max<int64_t>(4096, fixture.actual_ingested_bytes * 2));

  auto mem = FastFHIR::Memory::create(arena_size);
  std::memset(mem.base(), 0, arena_size); // Page fault all memory up-front to ensure timing includes any VMA population overhead.
  FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

  Timer stage1;
  stage1.start();

  std::vector<BundleentryData> entries;
  entries.reserve(fixture.bundle.size());

  // Deserialize each pre-ingested patient from Memory and add to bundle.
  for (const auto& p : fixture.bundle) {
    if (!p.memory) {
      continue; // Skip if memory is empty
    }

    FastFHIR::Parser patient_parser(p.memory);
    auto patient_node = patient_parser.root();
    if (!patient_node || !patient_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
      continue; // Skip if parse fails
    }

    const PatientData patient = patient_node.as<PatientData>();
    auto patient_handle = builder.append_obj(PatientData{});

    if (!patient.id.empty()) patient_handle[FastFHIR::Fields::PATIENT::ID] = patient.id;
    if (patient.meta) patient_handle[FastFHIR::Fields::PATIENT::META] = *patient.meta;
    if (!patient.implicitrules.empty()) {
      patient_handle[FastFHIR::Fields::PATIENT::IMPLICIT_RULES] = patient.implicitrules;
    }
    if (!patient.language.empty()) {
      patient_handle[FastFHIR::Fields::PATIENT::LANGUAGE] = patient.language;
    }
    if (patient.text) patient_handle[FastFHIR::Fields::PATIENT::TEXT] = *patient.text;
    if (!patient.contained.empty()) {
      patient_handle[FastFHIR::Fields::PATIENT::CONTAINED] = patient.contained;
    }
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::EXTENSION,
        patient.extension);
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::MODIFIER_EXTENSION,
        patient.modifierextension);
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::IDENTIFIER,
        patient.identifier);

    if (patient.active != FF_NULL_UINT8) {
      patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = (patient.active != 0);
    }
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::NAME,
        patient.name);
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::TELECOM,
        patient.telecom);
    if (patient.gender == AdministrativeGender::Male) {
      patient_handle[FastFHIR::Fields::PATIENT::GENDER] = std::string_view{"male"};
    } else if (patient.gender == AdministrativeGender::Female) {
      patient_handle[FastFHIR::Fields::PATIENT::GENDER] = std::string_view{"female"};
    } else if (patient.gender == AdministrativeGender::Other) {
      patient_handle[FastFHIR::Fields::PATIENT::GENDER] = std::string_view{"other"};
    } else if (patient.gender == AdministrativeGender::Unknown) {
      patient_handle[FastFHIR::Fields::PATIENT::GENDER] = std::string_view{"unknown"};
    }
    if (!patient.birthdate.empty()) {
      patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = patient.birthdate;
    }
    if (!patient.deceased.is_empty()) {
      if (const auto* b = std::get_if<bool>(&patient.deceased.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::DECEASED] = *b;
      } else if (const auto* i32 = std::get_if<int32_t>(&patient.deceased.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::DECEASED] = *i32;
      } else if (const auto* u32 = std::get_if<uint32_t>(&patient.deceased.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::DECEASED] = *u32;
      } else if (const auto* i64 = std::get_if<int64_t>(&patient.deceased.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::DECEASED] = *i64;
      } else if (const auto* u64 = std::get_if<uint64_t>(&patient.deceased.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::DECEASED] = *u64;
      } else if (const auto* f64 = std::get_if<double>(&patient.deceased.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::DECEASED] = *f64;
      } else if (const auto* s = std::get_if<std::string_view>(&patient.deceased.value)) {
        if (!s->empty()) {
          patient_handle[FastFHIR::Fields::PATIENT::DECEASED] = *s;
        }
      }
    }
    if (!patient.multiplebirth.is_empty()) {
      if (const auto* b = std::get_if<bool>(&patient.multiplebirth.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH] = *b;
      } else if (const auto* i32 = std::get_if<int32_t>(&patient.multiplebirth.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH] = *i32;
      } else if (const auto* u32 = std::get_if<uint32_t>(&patient.multiplebirth.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH] = *u32;
      } else if (const auto* i64 = std::get_if<int64_t>(&patient.multiplebirth.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH] = *i64;
      } else if (const auto* u64 = std::get_if<uint64_t>(&patient.multiplebirth.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH] = *u64;
      } else if (const auto* f64 = std::get_if<double>(&patient.multiplebirth.value)) {
        patient_handle[FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH] = *f64;
      }
    }

    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::ADDRESS,
        patient.address);
    if (patient.maritalstatus) {
      patient_handle[FastFHIR::Fields::PATIENT::MARITAL_STATUS] = *patient.maritalstatus;
    }
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::PHOTO,
        patient.photo);
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::CONTACT,
        patient.contact);
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::COMMUNICATION,
        patient.communication);
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::GENERAL_PRACTITIONER,
        patient.generalpractitioner);
    if (patient.managingorganization) {
      patient_handle[FastFHIR::Fields::PATIENT::MANAGING_ORGANIZATION] = *patient.managingorganization;
    }
    assign_offset_array_if_present(
        builder,
        patient_handle,
        FastFHIR::Fields::PATIENT::LINK,
        patient.link);

    // Add to bundle entries
    BundleentryData patient_entry{};
    patient_entry.resource = static_cast<ResourceReference>(patient_handle);
    entries.push_back(std::move(patient_entry));
  }

  // Build and serialize the bundle
  BundleData bundle{};
  bundle.type = BundleType::Collection;
  bundle.entry = std::move(entries);
  auto bundle_handle = builder.append_obj(bundle);
  builder.set_root(bundle_handle);
  const auto view = builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t len) -> std::vector<uint8_t> {
    return std::vector<uint8_t>(32);
});

  const auto stage1_us = std::max<std::int64_t>(stage1.stop_us(), 1);
  result.metrics.push_back(MetricEvent{"fastfhir", Stage::Stage1Serialize, stage1_us});

  Timer stage3;
  stage3.start();

  FastFHIR::Parser parser(view.data(), view.size());
  auto root = parser.root();

  int patients_found = 0;
  std::string found_birthdate;
  double found_cholesterol = 0.0;
  bool found_cholesterol_value = false;

  if (root && root.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
    auto entries_node = root[FastFHIR::Fields::BUNDLE::ENTRY];
    if (entries_node) {
      for (auto& entry_node : entries_node.entries()) {
        auto resource_entry = entry_node[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource_entry) continue;

        auto resource_node = resource_entry.as_node();
        if (!resource_node) continue;

        if (resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
          auto birth_date_field = resource_node[FastFHIR::Fields::PATIENT::BIRTH_DATE];
          if (birth_date_field) {
            ++patients_found;
            if (found_birthdate.empty())
              found_birthdate = std::string(birth_date_field.as<std::string_view>());
          }
        } else if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
          auto code_node = resource_node[FastFHIR::Fields::OBSERVATION::CODE];
          if (code_node) {
            auto coding_array = code_node[FastFHIR::Fields::CODEABLECONCEPT::CODING];
            if (coding_array) {
              for (auto& coding_entry : coding_array.entries()) {
                auto code_field = coding_entry[FastFHIR::Fields::CODING::CODE];
                if (code_field && code_field.as<std::string_view>() == kCholesterolLoincCode) {
                  auto value_entry = resource_node[FastFHIR::Fields::OBSERVATION::VALUE];
                  if (value_entry) {
                    try {
                      auto qty = value_entry.as<QuantityData>();
                      found_cholesterol = qty.value;
                      found_cholesterol_value = true;
                    } catch (...) {
                      // Keep benchmark running if this Observation uses a non-quantity value choice.
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  result.queried_value = "patients=" + std::to_string(patients_found)
      + " birthdate=" + found_birthdate
      + " cholesterol=" + (found_cholesterol_value ? std::to_string(found_cholesterol) : "N/A");

  result.metrics.push_back(
      MetricEvent{"fastfhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench
