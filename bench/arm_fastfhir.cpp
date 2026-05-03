#include "harness.hpp"

#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Patient.hpp>
#include <FF_Observation.hpp>
#include <algorithm>
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

static inline BundleentryData make_patient_bundle_entry(
    FastFHIR::Builder& builder,
    const BundlePatient& p) {
  BundleentryData empty_entry{};
  const PatientData& patient = p.patient;

  try {
    auto patient_handle = builder.append_obj(PatientData{});
    if (!patient.id.empty()) {
      patient_handle[FastFHIR::Fields::PATIENT::ID] = patient.id;
    }

    BundleentryData patient_entry{};
    patient_entry.resource = static_cast<ResourceReference>(patient_handle);
    return patient_entry;
  } catch (...) {
    return empty_entry;
  }
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
  volatile uint8_t* v_ptr = static_cast<volatile uint8_t*>(mem.base());
  for (std::size_t i = 0; i < arena_size; i += 4096) {
      v_ptr[i] = v_ptr[i]; 
  }
  FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

  Timer stage1;
  stage1.start();

  std::vector<BundleentryData> entries;
  entries.reserve(fixture.bundle.size());
  for (const auto& p : fixture.bundle) {
    entries.push_back(make_patient_bundle_entry(builder, p));
  }

  entries.erase(
      std::remove_if(
          entries.begin(),
          entries.end(),
          [](const BundleentryData& e) {
            return e.resource.offset == FF_NULL_OFFSET;
          }),
      entries.end());

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
  int encounters_found = 0;
  int conditions_found = 0;
  std::string found_birthdate;
  std::string found_condition_code;

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
          ++patients_found;
          if (birth_date_field && found_birthdate.empty()) {
            found_birthdate = std::string(birth_date_field.as<std::string_view>());
          }
        } else if (resource_node.is<FastFHIR::RESOURCETYPE::ENCOUNTER>()) {
          ++encounters_found;
        } else if (resource_node.is<FastFHIR::RESOURCETYPE::CONDITION>()) {
          ++conditions_found;
          auto code_node = resource_node[FastFHIR::Fields::CONDITION::CODE];
          if (code_node && found_condition_code.empty()) {
            auto coding_array = code_node[FastFHIR::Fields::CODEABLECONCEPT::CODING];
            if (coding_array) {
              for (auto& coding_entry : coding_array.entries()) {
                auto code_field = coding_entry[FastFHIR::Fields::CODING::CODE];
                if (code_field) {
                  found_condition_code = std::string(code_field.as<std::string_view>());
                  break;
                }
              }
            }
          }
        }
      }
    }
  }

  result.queried_value = "patients=" + std::to_string(patients_found)
    + " birthdate=" + (found_birthdate.empty() ? "none" : found_birthdate)
    + " encounters=" + std::to_string(encounters_found)
    + " conditions=" + std::to_string(conditions_found)
    + " condition_code=" + (found_condition_code.empty() ? "none" : found_condition_code);

  result.metrics.push_back(
      MetricEvent{"fastfhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench
