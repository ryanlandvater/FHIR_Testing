#pragma once

#include "bench_test_2.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#if defined(ARM_FASTFHIR)
#include <FF_Bundle.hpp>
#include <FF_Observation.hpp>
#include <FastFHIR.hpp>
#elif defined(ARM_JSON)
#include <simdjson.h>
#elif defined(ARM_GOOGLE_FHIR)
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#endif

namespace bench::test_3 {

struct QuerySummary {
  std::size_t patients = 0;
  std::string birthdate;
  std::size_t observations = 0;
  std::size_t loinc_2085_9_matches = 0;
  std::size_t obs_value_present = 0;
  std::size_t obs_value_quantity = 0;
  std::size_t obs_value_codeableconcept = 0;
  std::size_t obs_value_string = 0;
  std::size_t obs_value_code = 0;
  std::size_t obs_effective_datetime = 0;
  std::size_t obs_effective_period = 0;
  std::size_t obs_issued_present = 0;
  std::size_t obs_component_value_present = 0;
  std::size_t obs_component_value_quantity = 0;
  std::size_t obs_component_value_codeableconcept = 0;
  std::size_t obs_component_value_string = 0;
  std::size_t obs_component_value_code = 0;
};

inline std::string format_query_summary(const QuerySummary& summary) {
  return "patients=" + std::to_string(summary.patients) +
         " birthdate=" + summary.birthdate +
         " observations=" + std::to_string(summary.observations) +
         " loinc_2085_9_matches=" + std::to_string(summary.loinc_2085_9_matches) +
         " obs_value_present=" + std::to_string(summary.obs_value_present) +
         " obs_value_quantity=" + std::to_string(summary.obs_value_quantity) +
         " obs_value_codeableconcept=" + std::to_string(summary.obs_value_codeableconcept) +
         " obs_value_string=" + std::to_string(summary.obs_value_string) +
         " obs_value_code=" + std::to_string(summary.obs_value_code) +
         " obs_effective_datetime=" + std::to_string(summary.obs_effective_datetime) +
         " obs_effective_period=" + std::to_string(summary.obs_effective_period) +
         " obs_issued_present=" + std::to_string(summary.obs_issued_present) +
         " obs_component_value_present=" + std::to_string(summary.obs_component_value_present) +
         " obs_component_value_quantity=" + std::to_string(summary.obs_component_value_quantity) +
         " obs_component_value_codeableconcept=" + std::to_string(summary.obs_component_value_codeableconcept) +
         " obs_component_value_string=" + std::to_string(summary.obs_component_value_string) +
         " obs_component_value_code=" + std::to_string(summary.obs_component_value_code);
}

constexpr std::string_view kCholesterolLoincCode = "2085-9";
constexpr std::string_view kLoincSystem = "http://loinc.org";

namespace detail {

enum class ValueKind {
  Quantity,
  CodeableConcept,
  String,
  Code,
};

struct QueryAccumulator {
  std::size_t patients = 0;
  std::string birthdate;
  std::size_t observations = 0;
  std::size_t loinc_2085_9_matches = 0;
  std::size_t obs_value_present = 0;
  std::size_t obs_value_quantity = 0;
  std::size_t obs_value_codeableconcept = 0;
  std::size_t obs_value_string = 0;
  std::size_t obs_value_code = 0;
  std::size_t obs_effective_datetime = 0;
  std::size_t obs_effective_period = 0;
  std::size_t obs_issued_present = 0;
  std::size_t obs_component_value_present = 0;
  std::size_t obs_component_value_quantity = 0;
  std::size_t obs_component_value_codeableconcept = 0;
  std::size_t obs_component_value_string = 0;
  std::size_t obs_component_value_code = 0;

  void note_patient(std::string_view candidate_birthdate) {
    ++patients;
    if (birthdate.empty() && !candidate_birthdate.empty()) {
      birthdate.assign(candidate_birthdate.begin(), candidate_birthdate.end());
    }
  }

  void note_observation() { ++observations; }

  void note_loinc_2085_9() { ++loinc_2085_9_matches; }

  void note_observation_value(ValueKind kind) {
    ++obs_value_present;
    switch (kind) {
      case ValueKind::Quantity:
        ++obs_value_quantity;
        break;
      case ValueKind::CodeableConcept:
        ++obs_value_codeableconcept;
        break;
      case ValueKind::String:
        ++obs_value_string;
        break;
      case ValueKind::Code:
        ++obs_value_code;
        break;
    }
  }

  void note_component_value(ValueKind kind) {
    ++obs_component_value_present;
    switch (kind) {
      case ValueKind::Quantity:
        ++obs_component_value_quantity;
        break;
      case ValueKind::CodeableConcept:
        ++obs_component_value_codeableconcept;
        break;
      case ValueKind::String:
        ++obs_component_value_string;
        break;
      case ValueKind::Code:
        ++obs_component_value_code;
        break;
    }
  }

  void note_effective_datetime() { ++obs_effective_datetime; }

  void note_effective_period() { ++obs_effective_period; }

  void note_issued() { ++obs_issued_present; }

  QuerySummary finalize() const {
    QuerySummary summary;
    summary.patients = patients;
    summary.birthdate = birthdate;
    summary.observations = observations;
    summary.loinc_2085_9_matches = loinc_2085_9_matches;
    summary.obs_value_present = obs_value_present;
    summary.obs_value_quantity = obs_value_quantity;
    summary.obs_value_codeableconcept = obs_value_codeableconcept;
    summary.obs_value_string = obs_value_string;
    summary.obs_value_code = obs_value_code;
    summary.obs_effective_datetime = obs_effective_datetime;
    summary.obs_effective_period = obs_effective_period;
    summary.obs_issued_present = obs_issued_present;
    summary.obs_component_value_present = obs_component_value_present;
    summary.obs_component_value_quantity = obs_component_value_quantity;
    summary.obs_component_value_codeableconcept = obs_component_value_codeableconcept;
    summary.obs_component_value_string = obs_component_value_string;
    summary.obs_component_value_code = obs_component_value_code;
    return summary;
  }
};

inline bool is_loinc_match(std::string_view system, std::string_view code) {
  return code == kCholesterolLoincCode && (system.empty() || system == kLoincSystem);
}

inline std::string segment_field(std::string_view segment, std::size_t one_based_index) {
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

}  // namespace detail

#if defined(ARM_FASTFHIR)

#define TEST3_FF_BUNDLE_ENTRY FastFHIR::Fields::BUNDLE::ENTRY
#define TEST3_FF_ENTRY_RESOURCE FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE

inline std::optional<detail::ValueKind> value_kind_from_choice_tag(RECOVERY_TAG tag) {
  switch (tag) {
    case RECOVER_FF_QUANTITY:
      return detail::ValueKind::Quantity;
    case RECOVER_FF_CODEABLECONCEPT:
      return detail::ValueKind::CodeableConcept;
    case RECOVER_FF_STRING:
      return detail::ValueKind::String;
    case RECOVER_FF_CODE:
      return detail::ValueKind::Code;
    default:
      return std::nullopt;
  }
}

static inline QuerySummary query(std::string_view payload) {
  detail::QueryAccumulator acc;

  // Keep parser function-local in the query path to avoid persistent heap state.
  FastFHIR::Parser parser(payload.data(), payload.size());
  const auto root_node = parser.root();
  if (!(root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>())) {
    return acc.finalize();
  }

  auto entries = root_node[TEST3_FF_BUNDLE_ENTRY];
  if (!entries) {
    return acc.finalize();
  }

  for (auto& entry : entries.entries()) {
    auto resource = entry[TEST3_FF_ENTRY_RESOURCE];
    if (!resource) {
      continue;
    }
    auto resource_node = resource.as_node();
    if (!resource_node) {
      continue;
    }

    if (resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
      std::string_view birthdate;
      auto birth_date_entry = resource_node[FastFHIR::Fields::PATIENT::BIRTH_DATE];
      if (birth_date_entry) {
        birthdate = birth_date_entry.as<std::string_view>();
      }
      acc.note_patient(birthdate);
      continue;
    }

    if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
      acc.note_observation();

      const auto observation = resource_node.as<ObservationData>();
      if (observation.code) {
        bool matched = false;
        for (const auto& coding : observation.code->coding) {
          if (detail::is_loinc_match(coding.system, coding.code)) {
            matched = true;
            break;
          }
        }
        if (matched) {
          acc.note_loinc_2085_9();
        }
      }

      if (const auto kind = value_kind_from_choice_tag(observation.value.tag)) {
        acc.note_observation_value(*kind);
      }

      if (observation.effective.tag == RECOVER_FF_PERIOD) {
        acc.note_effective_period();
      } else if (!observation.effective.is_empty()) {
        acc.note_effective_datetime();
      }

      if (!observation.issued.empty()) {
        acc.note_issued();
      }

      for (const auto& component : observation.component) {
        if (const auto kind = value_kind_from_choice_tag(component.value.tag)) {
          acc.note_component_value(*kind);
        }
      }
    }
  }

  return acc.finalize();
}

#undef TEST3_FF_BUNDLE_ENTRY
#undef TEST3_FF_ENTRY_RESOURCE

#elif defined(ARM_JSON)

#define TEST3_JSON_KEY_ENTRY "entry"
#define TEST3_JSON_KEY_RESOURCE "resource"
#define TEST3_JSON_KEY_RESOURCE_TYPE "resourceType"
#define TEST3_JSON_KEY_PATIENT "Patient"
#define TEST3_JSON_KEY_OBSERVATION "Observation"

static inline QuerySummary query(const std::string& payload) {
  detail::QueryAccumulator acc;

  simdjson::dom::parser json_parser;
  auto doc = json_parser.parse(payload);
  if (doc.error()) {
    return acc.finalize();
  }

  auto entries = doc[TEST3_JSON_KEY_ENTRY];
  if (!entries.is_array()) {
    return acc.finalize();
  }

  for (auto entry : entries) {
    if (!entry.is_object()) {
      continue;
    }

    auto resource = entry[TEST3_JSON_KEY_RESOURCE];
    if (!resource.is_object()) {
      continue;
    }

    auto res_type = resource[TEST3_JSON_KEY_RESOURCE_TYPE];
    if (!res_type.is_string()) {
      continue;
    }

    const std::string_view type_str = res_type.get_c_str().value_unsafe();

    if (type_str == TEST3_JSON_KEY_PATIENT) {
      auto bd = resource["birthDate"];
      if (bd.is_string()) {
        acc.note_patient(bd.get_c_str().value_unsafe());
      } else {
        acc.note_patient({});
      }
      continue;
    }

    if (type_str == TEST3_JSON_KEY_OBSERVATION) {
      acc.note_observation();

      auto code = resource["code"];
      if (code.is_object()) {
        auto coding = code["coding"];
        if (coding.is_array()) {
          bool matched = false;
          for (auto c : coding) {
            if (!c.is_object()) {
              continue;
            }
            auto code_val = c["code"];
            if (!code_val.is_string()) {
              continue;
            }
            std::string_view code_sv = code_val.get_c_str().value_unsafe();
            std::string_view system_sv;
            auto system_val = c["system"];
            if (system_val.is_string()) {
              system_sv = system_val.get_c_str().value_unsafe();
            }
            if (detail::is_loinc_match(system_sv, code_sv)) {
              matched = true;
              break;
            }
          }
          if (matched) {
            acc.note_loinc_2085_9();
          }
        }
      }

      if (resource["valueQuantity"].is_object()) {
        acc.note_observation_value(detail::ValueKind::Quantity);
      } else if (resource["valueCodeableConcept"].is_object()) {
        acc.note_observation_value(detail::ValueKind::CodeableConcept);
      } else if (resource["valueString"].is_string()) {
        acc.note_observation_value(detail::ValueKind::String);
      } else if (resource["valueCode"].is_string()) {
        acc.note_observation_value(detail::ValueKind::Code);
      }

      if (resource["effectiveDateTime"].is_string()) {
        acc.note_effective_datetime();
      }
      if (resource["effectivePeriod"].is_object()) {
        acc.note_effective_period();
      }
      if (resource["issued"].is_string()) {
        acc.note_issued();
      }

      auto components = resource["component"];
      if (components.is_array()) {
        for (auto comp : components) {
          if (!comp.is_object()) {
            continue;
          }
          if (comp["valueQuantity"].is_object()) {
            acc.note_component_value(detail::ValueKind::Quantity);
          } else if (comp["valueCodeableConcept"].is_object()) {
            acc.note_component_value(detail::ValueKind::CodeableConcept);
          } else if (comp["valueString"].is_string()) {
            acc.note_component_value(detail::ValueKind::String);
          } else if (comp["valueCode"].is_string()) {
            acc.note_component_value(detail::ValueKind::Code);
          }
        }
      }
    }
  }

  return acc.finalize();
}

#undef TEST3_JSON_KEY_ENTRY
#undef TEST3_JSON_KEY_RESOURCE
#undef TEST3_JSON_KEY_RESOURCE_TYPE
#undef TEST3_JSON_KEY_PATIENT
#undef TEST3_JSON_KEY_OBSERVATION

#elif defined(ARM_GOOGLE_FHIR)

inline uint32_t decode_u32_le_t3(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

static inline QuerySummary query(const std::string& payload) {
  detail::QueryAccumulator acc;

  // Hoist outside the loop for memory reuse.
  google::fhir::r4::core::Patient patient;
  google::fhir::r4::core::Observation observation;

  std::size_t pos = 0;
  while (pos + 5 <= payload.size()) {
    const char record_type = payload[pos];
    const uint32_t record_len = decode_u32_le_t3(payload.data() + pos + 1);
    pos += 5;

    if (pos + record_len > payload.size()) {
      break;
    }

    const char* record_data = payload.data() + pos;
    pos += record_len;

    if (record_type == 'P') {
      patient.Clear();
      if (patient.ParseFromArray(record_data, static_cast<int>(record_len))) {
        std::string bd;
        if (patient.has_birth_date()) {
          // Google FHIR stores dates as epoch microseconds.
          // Using to_string to mimic extraction cost without full ISO8601 formatting overhead.
          bd = std::to_string(patient.birth_date().value_us());
        }
        acc.note_patient(bd);
      }
    } else if (record_type == 'O') {
      observation.Clear();
      if (observation.ParseFromArray(record_data, static_cast<int>(record_len))) {
        acc.note_observation();

        if (observation.has_code()) {
          bool matched = false;
          for (const auto& coding : observation.code().coding()) {
            if (detail::is_loinc_match(coding.system().value(), coding.code().value())) {
              matched = true;
              break;
            }
          }
          if (matched) acc.note_loinc_2085_9();
        }

        if (observation.has_value()) {
          const auto& val = observation.value();
          if (val.has_quantity()) {
            acc.note_observation_value(detail::ValueKind::Quantity);
          } else if (val.has_codeable_concept()) {
            acc.note_observation_value(detail::ValueKind::CodeableConcept);
          } else if (val.has_string_value()) {
            acc.note_observation_value(detail::ValueKind::String);
          }
          // Note: FHIR R4 Observation.value[x] does not natively use "valueCode"
          // so there is no .has_code() equivalent here in Google FHIR.
        }

        if (observation.has_effective()) {
          const auto& eff = observation.effective();
          if (eff.has_date_time()) {
            acc.note_effective_datetime();
          } else if (eff.has_period()) {
            acc.note_effective_period();
          }
        }

        if (observation.has_issued()) {
          acc.note_issued();
        }

        for (const auto& comp : observation.component()) {
          if (comp.has_value()) {
            const auto& cval = comp.value();
            if (cval.has_quantity()) {
              acc.note_component_value(detail::ValueKind::Quantity);
            } else if (cval.has_codeable_concept()) {
              acc.note_component_value(detail::ValueKind::CodeableConcept);
            } else if (cval.has_string_value()) {
              acc.note_component_value(detail::ValueKind::String);
            }
          }
        }
      }
    }
  }

  return acc.finalize();
}

#elif defined(ARM_HL7V2)

#define TEST3_HL7_PATIENT_SEG "PID|"
#define TEST3_HL7_OBS_SEG "OBX|"
#define TEST3_HL7_FIELD_VALUE_TYPE 3
#define TEST3_HL7_FIELD_CODE 4
#define TEST3_HL7_FIELD_VALUE 6
#define TEST3_HL7_FIELD_BIRTHDATE 8

// Returns the one_based_index-th pipe-delimited field of a raw HL7 segment line
// (1 = segment ID, 2 = first data field, etc.).
inline std::string_view segment_field(std::string_view seg, std::size_t one_based_index) {
  std::size_t field = 1;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= seg.size(); ++i) {
    if (i == seg.size() || seg[i] == '|') {
      if (field == one_based_index) {
        return seg.substr(start, i - start);
      }
      ++field;
      start = i + 1;
      if (field > one_based_index) {
        break;
      }
    }
  }
  return {};
}

static inline QuerySummary query(const std::string& payload) {
  QuerySummary summary;

  // Query directly from raw HL7 segment lines to keep this arm allocation-light.

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
    const std::string_view seg(payload.data() + line_start, end - line_start);

    if (seg.size() >= 4 && seg.substr(0, 4) == TEST3_HL7_PATIENT_SEG) {
      ++patient_count;
      if (birthdate.empty()) {
        const auto bd = segment_field(seg, TEST3_HL7_FIELD_BIRTHDATE);
        birthdate.assign(bd.data(), bd.size());
      }
    } else if (seg.size() >= 4 && seg.substr(0, 4) == TEST3_HL7_OBS_SEG) {
      ++observation_count;
      const auto id         = segment_field(seg, TEST3_HL7_FIELD_CODE);
      const auto value_type = segment_field(seg, TEST3_HL7_FIELD_VALUE_TYPE);
      const auto value      = segment_field(seg, TEST3_HL7_FIELD_VALUE);
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

  summary.patients = patient_count;
  summary.birthdate = std::move(birthdate);
  summary.observations = observation_count;
  summary.loinc_2085_9_matches = cholesterol_matches;
  summary.obs_value_present = value_present;
  summary.obs_value_quantity = value_quantity;
  summary.obs_value_codeableconcept = value_codeableconcept;
  summary.obs_value_string = value_string;
  summary.obs_value_code = value_code;
  return summary;
}

#undef TEST3_HL7_PATIENT_SEG
#undef TEST3_HL7_OBS_SEG
#undef TEST3_HL7_FIELD_VALUE_TYPE
#undef TEST3_HL7_FIELD_CODE
#undef TEST3_HL7_FIELD_VALUE
#undef TEST3_HL7_FIELD_BIRTHDATE

#endif

}  // namespace bench::test_3
