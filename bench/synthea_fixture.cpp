#include "harness.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace bench {

SyntheaFixture make_synthea_fixture(const std::string& json_payload) {
  const auto root = nlohmann::json::parse(json_payload);
  if (!root.is_object()) {
    throw std::runtime_error("Synthea payload root is not a JSON object");
  }

  SyntheaFixture fixture{};
  if (!root.contains("entry") || !root["entry"].is_array()) {
    return fixture;
  }

  for (const auto& bundle_entry : root["entry"]) {
    if (!bundle_entry.is_object() || !bundle_entry.contains("resource") ||
        !bundle_entry["resource"].is_object()) {
      continue;
    }

    const auto& resource = bundle_entry["resource"];
    if (resource.value("resourceType", "") != "Observation") {
      continue;
    }

    if (!resource.contains("code") || !resource["code"].is_object() ||
        !resource["code"].contains("coding") || !resource["code"]["coding"].is_array()) {
      continue;
    }

    bool loinc_match = false;
    for (const auto& coding : resource["code"]["coding"]) {
      if (!coding.is_object()) {
        continue;
      }
      if (coding.value("system", "") == kLoincSystem &&
          coding.value("code", "") == kCholesterolLoincCode) {
        loinc_match = true;
        break;
      }
    }

    if (!loinc_match) {
      continue;
    }

    CholesterolObservation obs{};
    obs.system = std::string(kLoincSystem);
    obs.code = std::string(kCholesterolLoincCode);

    if (resource.contains("valueQuantity") && resource["valueQuantity"].is_object() &&
        resource["valueQuantity"].contains("value") &&
        resource["valueQuantity"]["value"].is_number()) {
      obs.value = resource["valueQuantity"]["value"].get<double>();
      obs.has_value = true;
    }

    fixture.cholesterol_observations.push_back(std::move(obs));
  }

  return fixture;
}

}  // namespace bench
