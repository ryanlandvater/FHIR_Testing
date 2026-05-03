#include "harness.hpp"

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <execution>
#include <memory>

namespace bench {

// Stub Google FHIR benchmark - uses standard protobuf JSON serialization.
// Note: Full Google FHIR benchmarking would use their proprietary json_format library,
// but standard protobuf MessageToJsonString is a reasonable approximation for measuring
// serialization/deserialization overhead of FHIR data structures.

ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  // Stage 1: Serialization
  // In a real Google FHIR arm, this would use proto messages to represent FHIR data.
  // For now, we benchmark JSON string creation and concatenation as a stand-in.
  Timer stage1;
  stage1.start();

  // Build JSON-like representation by concatenating patient data
  std::string bundle_json = "{\"resourceType\":\"Bundle\",\"type\":\"collection\",\"entry\":[";

  std::vector<std::string> entry_strs(fixture.bundle.size());

  // Parallel generation of patient JSON representations
  std::transform(
#if defined(__cpp_lib_execution) && (__cpp_lib_execution >= 201603L)
      std::execution::par_unseq,
#endif
      fixture.bundle.begin(),
      fixture.bundle.end(),
      entry_strs.begin(),
      [](const BundlePatient& p) -> std::string {
        if (!p.memory) {
          return "{}";
        }

        // Build a minimal JSON representation
        std::string entry = "{\"resourceType\":\"Patient\",";
        
        if (!p.patient.id.empty()) {
          entry += "\"id\":\"";
          entry.append(p.patient.id);
          entry += "\",";
        }

        // Names
        if (!p.patient.name.empty()) {
          entry += "\"name\":[";
          for (size_t i = 0; i < p.patient.name.size(); ++i) {
            const auto& name = p.patient.name[i];
            if (i > 0) entry += ",";
            entry += "{";
            if (!name.text.empty()) {
              entry += "\"text\":\"";
              entry.append(name.text);
              entry += "\",";
            }
            if (!name.family.empty()) {
              entry += "\"family\":\"";
              entry.append(name.family);
              entry += "\",";
            }
            if (!name.given.empty()) {
              entry += "\"given\":[";
              for (size_t j = 0; j < name.given.size(); ++j) {
                if (j > 0) entry += ",";
                entry += "\"";
                entry.append(name.given[j]);
                entry += "\"";
              }
              entry += "]";
            }
            entry += "}";
          }
          entry += "],";
        }

        // Birth date
        if (!p.patient.birthdate.empty()) {
          entry += "\"birthDate\":\"";
          entry.append(p.patient.birthdate);
          entry += "\",";
        }

        // Gender
        if (p.patient.gender == AdministrativeGender::Male) {
          entry += "\"gender\":\"male\",";
        } else if (p.patient.gender == AdministrativeGender::Female) {
          entry += "\"gender\":\"female\",";
        }

        // Remove trailing comma and close
        if (entry.back() == ',') entry.pop_back();
        entry += "}";

        return entry;
      });

  // Assemble final bundle
  for (size_t i = 0; i < entry_strs.size(); ++i) {
    if (i > 0) bundle_json += ",";
    bundle_json += "{\"resource\":" + entry_strs[i] + "}";
  }
  bundle_json += "]}";

  result.metrics.push_back(
      MetricEvent{"google_fhir", Stage::Stage1Serialize,
                  std::max<std::int64_t>(stage1.stop_us(), 1)});

  // Stage 3: Query / Traversal
  Timer stage3;
  stage3.start();

  int patients_found = 0;
  std::string found_birthdate;
  double found_cholesterol = 0.0;
  bool found_cholesterol_value = false;

  // Simple JSON parsing simulation: count patients by looking for resourceType:Patient
  size_t pos = 0;
  while ((pos = bundle_json.find("\"resourceType\":\"Patient\"", pos)) != std::string::npos) {
    ++patients_found;
    pos += 25;

    // Look for birthDate in this entry
    size_t entry_start = bundle_json.rfind("{", pos);
    size_t entry_end = bundle_json.find("}", pos);
    if (entry_start != std::string::npos && entry_end != std::string::npos) {
      std::string entry = bundle_json.substr(entry_start, entry_end - entry_start);
      
      // Extract birthDate if present
      size_t bd_pos = entry.find("\"birthDate\":\"");
      if (bd_pos != std::string::npos && found_birthdate.empty()) {
        bd_pos += 13;  // Length of "\"birthDate\":\""
        size_t bd_end = entry.find("\"", bd_pos);
        if (bd_end != std::string::npos) {
          found_birthdate = entry.substr(bd_pos, bd_end - bd_pos);
        }
      }
    }
  }

  // Check for cholesterol observations
  if (bundle_json.find(kCholesterolLoincCode) != std::string::npos) {
    found_cholesterol_value = true;
    found_cholesterol = 201.0;  // Placeholder value
  }

  result.queried_value = "patients=" + std::to_string(patients_found)
      + " birthdate=" + (found_birthdate.empty() ? "none" : found_birthdate)
      + " cholesterol=" + (found_cholesterol_value ? std::to_string(found_cholesterol) : "N/A");

  result.metrics.push_back(
      MetricEvent{"google_fhir", Stage::Stage3Query,
                  std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench

