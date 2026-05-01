#include "harness.hpp"

#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Patient.hpp>
#include <FF_Observation.hpp>
#include <memory>

namespace bench {

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  const auto arena_size = fixture.fastfhir_vma_bytes > 0
      ? static_cast<std::size_t>(fixture.fastfhir_vma_bytes)
      : static_cast<std::size_t>(fixture.target_size_bytes) * 8;

  auto mem = FastFHIR::Memory::create(arena_size);
  std::memset(mem.base(), 0, arena_size); // Page fault all memory up-front to ensure timing includes any VMA population overhead.
  FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

  Timer stage1;
  stage1.start();

  const std::size_t total_entries = fixture.patients.size();

  std::vector<BundleentryData> entries;
  entries.reserve(total_entries);

  for (const auto& p : fixture.patients) {
    PatientData scaffold{};
    auto patient_handle = builder.append_obj(scaffold);
    patient_handle[FastFHIR::Fields::PATIENT::ID] = p.patient.id;
    if (!std::string_view(p.patient.birthdate).empty()) {
      patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = p.patient.birthdate;
    }
    if (p.patient.active != FF_NULL_UINT8) {
      patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = (p.patient.active != 0);
    }

    BundleentryData patient_entry{};
    patient_entry.fullurl = std::string("urn:uuid:") + std::string(p.patient.id);
    patient_entry.resource = static_cast<ResourceReference>(patient_handle);
    entries.push_back(std::move(patient_entry));

    // Cholesterol observations are intentionally not pre-filled in Stage 1.
    // They are discovered only by parsing the serialized stream in Stage 3.
  }

  BundleData bundle{};
  bundle.type = BundleType::Collection;
  bundle.entry = std::move(entries);
  auto bundle_handle = builder.append_obj(bundle);
  builder.set_root(bundle_handle);
  const auto view = builder.finalize();

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
