#include "harness.hpp"

#include <memory>

namespace bench {

ArmRunResult run_fastfhir_synthea_query(const SyntheaFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  auto mem = FastFHIR::Memory::create(64 * 1024 * 1024);
  FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

  Timer stage1;
  stage1.start();

  std::vector<BundleentryData> entries;
  entries.reserve(fixture.cholesterol_observations.size());

  for (std::size_t i = 0; i < fixture.cholesterol_observations.size(); ++i) {
    const auto& sample = fixture.cholesterol_observations[i];

    ObservationData observation{};
    observation.id = "cholesterol";
    observation.status = ObservationStatus::Final;

    auto code = std::make_unique<CodeableConceptData>();
    CodingData coding{};
    coding.system = sample.system;
    coding.code = sample.code;
    code->coding.push_back(std::move(coding));
    observation.code = std::move(code);

    auto obs_handle = builder.append_obj(observation);

    BundleentryData entry{};
    entry.fullurl = "urn:uuid:cholesterol-entry";
    entry.resource = static_cast<ResourceReference>(obs_handle);
    entries.push_back(std::move(entry));
  }

  BundleData bundle{};
  bundle.type = BundleType::Collection;
  bundle.entry = std::move(entries);

  auto bundle_handle = builder.append_obj(bundle);
  builder.set_root(bundle_handle);
  const auto view = builder.finalize();

  result.metrics.push_back(
      MetricEvent{"fastfhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  Timer stage3;
  stage3.start();

  FastFHIR::Parser parser(view.data(), view.size());
  auto root = parser.root();

  if (root && root.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
    auto entries_node = root[FastFHIR::Fields::BUNDLE::ENTRY];
    if (entries_node) {
      for (auto& entry_node : entries_node.entries()) {
        auto resource_entry = entry_node[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource_entry) {
          continue;
        }

        auto resource_node = resource_entry.as_node();
        if (!resource_node || !resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
          continue;
        }

        auto code_block = resource_node[FastFHIR::Fields::OBSERVATION::CODE];
        if (!code_block) {
          continue;
        }

        auto coding_array = code_block[FastFHIR::Fields::CODEABLECONCEPT::CODING];
        if (!coding_array) {
          continue;
        }

        bool matches_loinc = false;
        for (auto& coding_node : coding_array.entries()) {
          const std::string_view system = coding_node[FastFHIR::Fields::CODING::SYSTEM];
          const std::string_view code = coding_node[FastFHIR::Fields::CODING::CODE];
          if (system == kLoincSystem && code == kCholesterolLoincCode) {
            matches_loinc = true;
            break;
          }
        }

        if (matches_loinc) {
          result.queried_value = "found";
          break;
        }
      }
    }
  }

  result.metrics.push_back(
      MetricEvent{"fastfhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench
