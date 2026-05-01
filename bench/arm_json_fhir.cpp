#include "harness.hpp"

#include <nlohmann/json.hpp>

namespace bench {

ArmRunResult run_json_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  Timer stage1;
  stage1.start();

  nlohmann::json bundle;
  bundle["resourceType"] = "Bundle";
  bundle["type"] = "collection";
  bundle["entry"] = nlohmann::json::array();

  for (const auto& p : fixture.patients) {
    nlohmann::json patient_resource;
    patient_resource["resourceType"] = "Patient";
    patient_resource["id"] = std::string(p.patient.id);
    if (!std::string_view(p.patient.birthdate).empty()) {
      patient_resource["birthDate"] = std::string(p.patient.birthdate);
    }
    if (p.patient.active != FF_NULL_UINT8) {
      patient_resource["active"] = (p.patient.active != 0);
    }
    nlohmann::json patient_entry;
    patient_entry["resource"] = std::move(patient_resource);
    bundle["entry"].push_back(std::move(patient_entry));
  }

  const std::string payload = bundle.dump();

  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  Timer stage3;
  stage3.start();

  const auto parsed = nlohmann::json::parse(payload);
  int patients_found = 0;
  std::string found_birthdate;
  double found_cholesterol = 0.0;
  bool found_cholesterol_value = false;

  if (parsed.contains("entry") && parsed["entry"].is_array()) {
    for (const auto& json_entry : parsed["entry"]) {
      if (!json_entry.contains("resource") || !json_entry["resource"].is_object()) continue;
      const auto& resource = json_entry["resource"];
      const auto resource_type = resource.value("resourceType", "");

      if (resource_type == "Patient") {
        if (resource.contains("birthDate")) {
          ++patients_found;
          if (found_birthdate.empty())
            found_birthdate = resource["birthDate"].get<std::string>();
        }
      } else if (resource_type == "Observation") {
        if (resource.contains("code") && resource["code"].contains("coding")
            && resource["code"]["coding"].is_array()) {
          for (const auto& coding : resource["code"]["coding"]) {
            if (coding.value("code", "") == std::string(kCholesterolLoincCode)) {
              if (resource.contains("valueQuantity")
                  && resource["valueQuantity"].contains("value")) {
                found_cholesterol = resource["valueQuantity"]["value"].get<double>();
                found_cholesterol_value = true;
              }
              break;
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
      MetricEvent{"json_fhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench
