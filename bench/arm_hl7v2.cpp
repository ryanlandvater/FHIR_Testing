#include "harness.hpp"
#include "hl7v2_message.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#define ARM_HL7V2
#include "bench_assign.hpp"
#undef ARM_HL7V2

namespace bench {
namespace {

static inline std::string segment_field(std::string_view segment, std::size_t one_based_index) {
  if (one_based_index == 0) {
    return "";
  }
  std::size_t current = 1;
  std::size_t start = 0;
  while (start <= segment.size()) {
    const auto sep = segment.find('|', start);
    const auto end = sep == std::string_view::npos ? segment.size() : sep;
    if (current == one_based_index) {
      return std::string(segment.substr(start, end - start));
    }
    if (sep == std::string_view::npos) {
      break;
    }
    start = sep + 1;
    ++current;
  }
  return "";
}

}  // namespace

ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer stage1_timer;
  stage1_timer.start();

  std::string payload;
  payload.reserve(fixture.bundle.size() * 512);

  for (const auto& item : fixture.bundle) {
    hl7v2::OruR01Message message;
    assign::assign_patient(item.patient, message);

    for (const auto& observation : item.observations) {
      assign::assign_observation(observation, message);
    }

    payload += message.dump();
  }

  out.metrics.push_back({"hl7v2", Stage::Stage1Serialize, stage1_timer.stop_ns()});

  Timer stage3_timer;
  stage3_timer.start();

  std::size_t patient_count = 0;
  std::size_t observation_count = 0;
  std::size_t cholesterol_matches = 0;
  std::size_t value_present = 0;
  std::size_t value_quantity = 0;
  std::size_t value_codeableconcept = 0;
  std::size_t value_string = 0;
  std::size_t value_code = 0;
  std::string birthdate;

  std::size_t line_start = 0;
  while (line_start < payload.size()) {
    const auto line_end = payload.find('\r', line_start);
    const auto end = line_end == std::string::npos ? payload.size() : line_end;
    const std::string_view segment(payload.data() + line_start, end - line_start);

    if (segment.size() >= 4 && segment.substr(0, 4) == "PID|") {
      ++patient_count;
      if (birthdate.empty()) {
        birthdate = segment_field(segment, 8);
      }
    } else if (segment.size() >= 4 && segment.substr(0, 4) == "OBX|") {
      ++observation_count;
      const auto id = segment_field(segment, 4);
      const auto value_type = segment_field(segment, 3);
      const auto value = segment_field(segment, 6);
      if (id.rfind("2085-9", 0) == 0) {
        ++cholesterol_matches;
      }
      if (!value.empty()) {
        ++value_present;
      }
      if (value_type == "NM") {
        ++value_quantity;
      } else if (value_type == "CE") {
        ++value_codeableconcept;
      } else if (value_type == "ST") {
        ++value_string;
      } else if (value_type == "CWE") {
        ++value_code;
      }
    }

    if (line_end == std::string::npos) {
      break;
    }
    line_start = line_end + 1;
  }

  out.metrics.push_back({"hl7v2", Stage::Stage3Query, stage3_timer.stop_ns()});
  out.queried_value =
      "patients=" + std::to_string(patient_count) +
      " birthdate=" + birthdate +
      " observations=" + std::to_string(observation_count) +
      " loinc_2085_9_matches=" + std::to_string(cholesterol_matches) +
      " obs_value_present=" + std::to_string(value_present) +
      " obs_value_quantity=" + std::to_string(value_quantity) +
      " obs_value_codeableconcept=" + std::to_string(value_codeableconcept) +
      " obs_value_string=" + std::to_string(value_string) +
      " obs_value_code=" + std::to_string(value_code) +
      " obs_effective_datetime=0"
      " obs_effective_period=0"
      " obs_issued_present=0"
      " obs_component_value_present=0"
      " obs_component_value_quantity=0"
      " obs_component_value_codeableconcept=0"
      " obs_component_value_string=0"
        " obs_component_value_code=0";
  out.reconstructed_bundle_json = payload;

  return out;
}

}  // namespace bench
