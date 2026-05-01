#include "harness.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace bench {

ArmRunResult run_json_fhir_smoke(const PatientData& patient) {
  ArmRunResult result;
  result.metrics.reserve(2);

  // Stage 1 start: immediately before the first JSON field write.
  Timer stage1;
  stage1.start();
  
  // Build canonical FHIR JSON from the same in-memory PatientData ground truth.
  nlohmann::json json;
  json["resourceType"] = "Patient";
  json["id"] = std::string(patient.id);
  if (patient.active != FF_NULL_UINT8) {
    json["active"] = (patient.active != 0);
  }
  json["gender"] = FF_AdministrativeGenderToString(patient.gender);
  json["birthDate"] = std::string(patient.birthdate);

  if (!patient.name.empty()) {
    const auto& name = patient.name.front();
    nlohmann::json name_json;
    name_json["family"] = std::string(name.family);
    name_json["given"] = nlohmann::json::array();
    for (const auto given : name.given) {
      name_json["given"].push_back(std::string(given));
    }
    json["name"] = nlohmann::json::array({name_json});
  }

  const std::string payload = json.dump();
  
  // Stage 1 end: complete UTF-8 JSON text is available.
  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  // Stage 3 start: first read/traversal operation on received representation.
  Timer stage3;
  stage3.start();
  const auto parsed = nlohmann::json::parse(payload);
  result.queried_value = parsed.at(kPatientQueryField).get<std::string>();
  // Stage 3 end: target value extracted into result variable.
  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  if (result.queried_value.empty()) {
    result.metrics.back().duration_us = std::max<std::int64_t>(result.metrics.back().duration_us, 1);
  }

  return result;
}

}  // namespace bench
