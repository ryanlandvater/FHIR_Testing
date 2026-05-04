#include "harness.hpp"

#include <nlohmann/json.hpp>

#define ARM_JSON
#include "bench_assign.hpp"
#undef ARM_JSON

namespace bench {
namespace {

}  // namespace

ArmRunResult run_json_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer stage1_timer;
  stage1_timer.start();

  nlohmann::json bundle;
  bundle["resourceType"] = "Bundle";
  bundle["type"] = "collection";
  bundle["entry"] = nlohmann::json::array();

  for (const auto& item : fixture.bundle) {
    nlohmann::json patient_json;
    assign::assign_patient(item.patient, patient_json);
    bundle["entry"].push_back({{"resource", std::move(patient_json)}});
  }

  const std::string payload = bundle.dump();
  out.metrics.push_back({"json_fhir", Stage::Stage1Serialize, stage1_timer.stop_us()});

  Timer stage3_timer;
  stage3_timer.start();
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(payload);
  } catch (...) {
    parsed = nlohmann::json::object();
  }

  std::string birthdate;
  std::size_t patient_count = 0;
  std::size_t cholesterol_matches = 0;

  if (parsed.contains("entry") && parsed["entry"].is_array()) {
    for (const auto& entry : parsed["entry"]) {
      if (!entry.is_object()) {
        continue;
      }
      auto resource_it = entry.find("resource");
      if (resource_it == entry.end() || !resource_it->is_object()) {
        continue;
      }

      const auto& resource = *resource_it;
      const auto type_it = resource.find("resourceType");
      if (type_it == resource.end() || !type_it->is_string()) {
        continue;
      }

      const std::string resource_type = type_it->get<std::string>();
      if (resource_type == "Patient") {
        ++patient_count;
        const auto bd_it = resource.find("birthDate");
        if (birthdate.empty() && bd_it != resource.end() && bd_it->is_string()) {
          birthdate = bd_it->get<std::string>();
        }
      }
    }
  }

  out.metrics.push_back({"json_fhir", Stage::Stage3Query, stage3_timer.stop_us()});
  out.queried_value =
      "patients=" + std::to_string(patient_count) +
      " birthdate=" + birthdate +
      " loinc_2085_9_matches=" + std::to_string(cholesterol_matches);
  out.reconstructed_bundle_json = payload;
  return out;
}

}  // namespace bench
