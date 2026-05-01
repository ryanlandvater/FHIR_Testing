#include "harness.hpp"

#include <nlohmann/json.hpp>

namespace bench {

ArmRunResult run_json_synthea_query(const SyntheaFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  Timer stage1;
  stage1.start();

  nlohmann::json bundle;
  bundle["resourceType"] = "Bundle";
  bundle["type"] = "collection";
  bundle["entry"] = nlohmann::json::array();

  for (const auto& obs : fixture.cholesterol_observations) {
    nlohmann::json resource;
    resource["resourceType"] = "Observation";
    resource["code"]["coding"] = nlohmann::json::array(
        {{{"system", obs.system}, {"code", obs.code}}});

    if (obs.has_value) {
      resource["valueQuantity"]["value"] = obs.value;
    }

    nlohmann::json entry;
    entry["resource"] = std::move(resource);
    bundle["entry"].push_back(std::move(entry));
  }

  const std::string payload = bundle.dump();
  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  Timer stage3;
  stage3.start();

  const auto parsed_bundle = nlohmann::json::parse(payload);
  if (parsed_bundle.contains("entry") && parsed_bundle["entry"].is_array()) {
    for (const auto& json_entry : parsed_bundle["entry"]) {
      if (!json_entry.contains("resource") || !json_entry["resource"].is_object()) {
        continue;
      }

      const auto& resource = json_entry["resource"];
      if (resource.value("resourceType", "") != "Observation") {
        continue;
      }

      if (!resource.contains("code") || !resource["code"].contains("coding") ||
          !resource["code"]["coding"].is_array()) {
        continue;
      }

      bool matches_loinc = false;
      for (const auto& coding : resource["code"]["coding"]) {
        if (!coding.is_object()) {
          continue;
        }
        if (coding.value("system", "") == kLoincSystem &&
            coding.value("code", "") == kCholesterolLoincCode) {
          matches_loinc = true;
          break;
        }
      }

      if (!matches_loinc) {
        continue;
      }

      if (resource.contains("valueQuantity") && resource["valueQuantity"].is_object() &&
          resource["valueQuantity"].contains("value")) {
        result.queried_value = resource["valueQuantity"]["value"].dump();
      } else {
        result.queried_value = "found";
      }
      break;
    }
  }

  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench
