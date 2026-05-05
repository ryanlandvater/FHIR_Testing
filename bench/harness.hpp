#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Condition.hpp>
#include <FF_Encounter.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Observation.hpp>
#include <FF_Patient.hpp>
#include <FF_Procedure.hpp>

namespace bench {

// Timed sections are declared here first for manual reviewability.
// Test 1 start: immediately before first field write to destination representation.
// Test 1 end: immediately after payload sealed.
// Test 2 start: immediately before deserialize/materialize work begins.
// Test 2 end: when the in-memory representation is available for query logic.
// Test 3 start: first parser/read call that consumes bytes or materialized nodes for query.
// Test 3 end: target value extracted into result variable.

enum class Stage {
  Test1Serialize,
  Test2Materialize,
  Test3Query,

  // Temporary compatibility aliases during the stage -> test migration.
  Stage1Serialize = Test1Serialize,
  Stage2Transport = Test2Materialize,
  Stage3Query = Test3Query,
  Stage3Materialize = Test2Materialize
};

struct MetricEvent {
  std::string arm;
  Stage stage;
  std::int64_t duration_ns;
};

struct ArmRunResult {
  std::vector<MetricEvent> metrics;
  std::string queried_value;
  std::string reconstructed_bundle_json;
};

class Timer {
 public:
  void start() { begin_ = std::chrono::steady_clock::now(); }
  std::int64_t stop_ns() const {
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin_).count();
    if (elapsed_ns <= 0) {
      return 1;
    }
    return elapsed_ns;
  }

 private:
  std::chrono::steady_clock::time_point begin_{};
};

inline std::string to_string(Stage s) {
  switch (s) {
    case Stage::Test1Serialize:
      return "test_1_serialize";
    case Stage::Test2Materialize:
      return "test_2_materialize";
    case Stage::Test3Query:
      return "test_3_query";
  }
  return "unknown";
}

inline void print_metric(const MetricEvent& e) {
  std::cout << e.arm << "," << to_string(e.stage) << "," << e.duration_ns << "\n" << std::flush;
}

inline constexpr std::string_view kPatientQueryField = "birthDate";
inline constexpr std::string_view kCholesterolLoincCode = "2085-9";
inline constexpr std::string_view kLoincSystem = "http://loinc.org";

// One ingested Synthea patient item kept in RAM.
// The FastFHIR::Memory arena contains the serialized FFHR patient binary.
struct BundlePatient {
  FastFHIR::Memory memory;
  PatientData patient;
  std::vector<EncounterData> encounters;
  std::vector<ConditionData> conditions;
  std::vector<ProcedureData> procedures;
  std::vector<ObservationData> observations;
};

struct BundleBenchFixture {
  std::vector<BundlePatient> bundle;
  int64_t target_size_bytes = 0;
  int64_t actual_ingested_bytes = 0;
  int64_t fastfhir_vma_bytes = 0;
};

namespace detail {
void hydrate_bundle_resources(const FastFHIR::Reflective::Node& root,
                              BundlePatient& bundle_patient);
}

BundlePatient make_bundle_patient_from_json(const std::filesystem::path& json_path);

inline BundlePatient clone_bundle_patient(const BundlePatient& src) {
  BundlePatient dst{};
  dst.memory = src.memory;

  // Re-hydrate the POCO bundle from copied FFHR memory so cloned fixtures keep
  // string-backed fields valid after arena duplication.
  if (dst.memory) {
    FastFHIR::Parser parser(dst.memory);
    detail::hydrate_bundle_resources(parser.root(), dst);
  }

  // Fallback for defensive compatibility if re-hydration cannot locate a patient.
  if (dst.patient.id.empty() && !src.patient.id.empty()) {
    dst.patient.id = src.patient.id;
  }
  if (dst.patient.birthdate.empty() && !src.patient.birthdate.empty()) {
    dst.patient.birthdate = src.patient.birthdate;
  }
  dst.patient.gender = src.patient.gender;
  dst.patient.active = src.patient.active;
  return dst;
}

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_json_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture);

// ---------------------------------------------------------------------------
// Cross-arm result validation
// ---------------------------------------------------------------------------

namespace detail {

inline bool reference_matches_patient_id(const ReferenceData& reference,
                                         std::string_view patient_id) {
  if (patient_id.empty() || reference.reference.empty()) {
    return false;
  }

  if (reference.reference == patient_id) {
    return true;
  }

  constexpr std::string_view kPatientPrefix = "Patient/";
  return reference.reference.size() == kPatientPrefix.size() + patient_id.size() &&
         reference.reference.substr(0, kPatientPrefix.size()) == kPatientPrefix &&
         reference.reference.substr(kPatientPrefix.size()) == patient_id;
}

template <typename Resource>
inline void copy_resources_for_patient(const std::vector<FastFHIR::Reflective::Node>& resource_nodes,
                                       std::string_view patient_id,
                                       std::vector<Resource>& out) {
  out.clear();
  out.reserve(resource_nodes.size());

  for (const auto& resource_node : resource_nodes) {
    auto resource = resource_node.as<Resource>();
    if (!patient_id.empty() && resource.subject) {
      if (!reference_matches_patient_id(*resource.subject, patient_id)) {
        continue;
      }
    }
    out.push_back(std::move(resource));
  }
}

inline void hydrate_bundle_resources(const FastFHIR::Reflective::Node& root,
                                     BundlePatient& bundle_patient) {
  bundle_patient.patient = PatientData{};
  bundle_patient.encounters.clear();
  bundle_patient.conditions.clear();
  bundle_patient.procedures.clear();
  bundle_patient.observations.clear();

  FastFHIR::Reflective::Node patient_node;
  std::vector<FastFHIR::Reflective::Node> encounter_nodes;
  std::vector<FastFHIR::Reflective::Node> condition_nodes;
  std::vector<FastFHIR::Reflective::Node> procedure_nodes;
  std::vector<FastFHIR::Reflective::Node> observation_nodes;

  if (root && root.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
    patient_node = root;
  } else if (root && root.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
    if (auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY]) {
      for (auto& entry : entries.entries()) {
        auto resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource) {
          continue;
        }

        auto resource_node = resource.as_node();
        if (!resource_node) {
          continue;
        }

        if (!patient_node && resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
          patient_node = resource_node;
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::ENCOUNTER>()) {
          encounter_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::CONDITION>()) {
          condition_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::PROCEDURE>()) {
          procedure_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
          observation_nodes.push_back(resource_node);
        }
      }
    }
  }

  if (patient_node && patient_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
    bundle_patient.patient = patient_node.as<PatientData>();
  }

  copy_resources_for_patient(encounter_nodes, bundle_patient.patient.id, bundle_patient.encounters);
  copy_resources_for_patient(condition_nodes, bundle_patient.patient.id, bundle_patient.conditions);
  copy_resources_for_patient(procedure_nodes, bundle_patient.patient.id, bundle_patient.procedures);
  copy_resources_for_patient(observation_nodes, bundle_patient.patient.id, bundle_patient.observations);
}

// Extract a key=value field from a queried_value string.
inline std::string qv_field(const std::string& qv, const std::string& key) {
  const std::string needle = key + "=";
  const auto pos = qv.find(needle);
  if (pos == std::string::npos) return "";
  const auto start = pos + needle.size();
  const auto end = qv.find(' ', start);
  return end == std::string::npos ? qv.substr(start) : qv.substr(start, end - start);
}

// Reduce a date string to its digit characters for format-agnostic comparison.
inline std::string digits_only(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) if (std::isdigit(c)) out += static_cast<char>(c);
  return out;
}

} // namespace detail

// Compare the query results across FFHR and JSON arms and print a warning to stderr
// for each discrepancy.  Returns true only if every check passes.
//
// Checks performed:
//   1. patients=N   — both arms must agree
//   2. birthdate    — normalized to digits; both arms must agree
//   3. encounters   — when present in both arms, values must agree
//   4. conditions   — when present in both arms, values must agree
//   5. observations — required in both arms and values must agree
//   6. loinc_2085_9_matches — required in both arms and values must agree
//   7. Observation value/effective/component semantic counters — required parity
inline bool validate_results(const ArmRunResult& ff, const ArmRunResult& jf) {
  using detail::qv_field;
  using detail::digits_only;
  bool ok = true;

  auto compare_required_field = [&](const char* key) {
    const auto ff_value = qv_field(ff.queried_value, key);
    const auto jf_value = qv_field(jf.queried_value, key);
    if (ff_value.empty() || jf_value.empty()) {
      std::cerr << "[validate] MISSING required field " << key << ":"
                << " fastfhir=" << (ff_value.empty() ? "<missing>" : ff_value)
                << " json=" << (jf_value.empty() ? "<missing>" : jf_value) << "\n";
      ok = false;
      return;
    }
    if (ff_value != jf_value) {
      std::cerr << "[validate] MISMATCH " << key << ":"
                << " fastfhir=" << ff_value
                << " json=" << jf_value << "\n";
      ok = false;
    }
  };

  // 1. Patient count
  const auto ff_pat = qv_field(ff.queried_value, "patients");
  const auto jf_pat = qv_field(jf.queried_value, "patients");
  if (ff_pat != jf_pat) {
    std::cerr << "[validate] MISMATCH patients:"
              << " fastfhir=" << ff_pat
              << " json="     << jf_pat << "\n";
    ok = false;
  }

  // 2. Birthdate (spot-check first patient; format-normalised)
  const auto ff_bd = digits_only(qv_field(ff.queried_value, "birthdate"));
  const auto jf_bd = digits_only(qv_field(jf.queried_value, "birthdate"));
  if (!ff_bd.empty() && ff_bd != jf_bd) {
    std::cerr << "[validate] MISMATCH birthdate:"
              << " fastfhir=" << ff_bd
              << " json="     << jf_bd << "\n";
    ok = false;
  }

  // 3. Encounter count (optional until all arms emit it)
  const auto ff_enc = qv_field(ff.queried_value, "encounters");
  const auto jf_enc = qv_field(jf.queried_value, "encounters");
  if (!ff_enc.empty() && !jf_enc.empty()) {
    if (ff_enc != jf_enc) {
      std::cerr << "[validate] MISMATCH encounters:"
                << " fastfhir=" << ff_enc
                << " json="     << jf_enc << "\n";
      ok = false;
    }
  }

  // 4. Condition count (optional until all arms emit it)
  const auto ff_cond = qv_field(ff.queried_value, "conditions");
  const auto jf_cond = qv_field(jf.queried_value, "conditions");
  if (!ff_cond.empty() && !jf_cond.empty()) {
    if (ff_cond != jf_cond) {
      std::cerr << "[validate] MISMATCH conditions:"
                << " fastfhir=" << ff_cond
                << " json="     << jf_cond << "\n";
      ok = false;
    }
  }

  compare_required_field("observations");
  compare_required_field("loinc_2085_9_matches");
  compare_required_field("obs_value_present");
  compare_required_field("obs_value_quantity");
  compare_required_field("obs_value_codeableconcept");
  compare_required_field("obs_value_string");
  compare_required_field("obs_value_code");
  compare_required_field("obs_effective_datetime");
  compare_required_field("obs_effective_period");
  compare_required_field("obs_issued_present");
  compare_required_field("obs_component_value_present");
  compare_required_field("obs_component_value_quantity");
  compare_required_field("obs_component_value_codeableconcept");
  compare_required_field("obs_component_value_string");
  compare_required_field("obs_component_value_code");

  // 6. Reconstructed fixture parity (both arms must reconstruct identical bundle JSON)
  if (!ff.reconstructed_bundle_json.empty() && !jf.reconstructed_bundle_json.empty()) {
    if (ff.reconstructed_bundle_json != jf.reconstructed_bundle_json) {
      std::cerr << "[validate] MISMATCH reconstructed_bundle_json:"
                << " fastfhir=" << ff.reconstructed_bundle_json.size()
                << " json=" << jf.reconstructed_bundle_json.size() << "\n";
      ok = false;
    }
  }

  return ok;
}

// Compare HL7v2 query outputs against FFHR/JSON baseline for the subset of
// fields that HL7v2 currently models directly.
inline bool validate_hl7_results(const ArmRunResult& ff,
                                 const ArmRunResult& jf,
                                 const ArmRunResult& h2) {
  using detail::digits_only;
  using detail::qv_field;

  bool ok = true;
  auto compare_required_field = [&](const char* key) {
    const auto baseline_value = qv_field(jf.queried_value, key);
    const auto hl7_value = qv_field(h2.queried_value, key);
    if (baseline_value.empty() || hl7_value.empty()) {
      std::cerr << "[validate] MISSING hl7 required field " << key << ":"
                << " baseline=" << (baseline_value.empty() ? "<missing>" : baseline_value)
                << " hl7=" << (hl7_value.empty() ? "<missing>" : hl7_value) << "\n";
      ok = false;
      return;
    }
    if (baseline_value != hl7_value) {
      std::cerr << "[validate] MISMATCH hl7 " << key << ":"
                << " baseline=" << baseline_value
                << " hl7=" << hl7_value << "\n";
      ok = false;
    }
  };

  compare_required_field("patients");

  const auto baseline_birthdate = digits_only(qv_field(jf.queried_value, "birthdate"));
  const auto hl7_birthdate = digits_only(qv_field(h2.queried_value, "birthdate"));
  if (baseline_birthdate.empty() || hl7_birthdate.empty()) {
    std::cerr << "[validate] MISSING hl7 required field birthdate:"
              << " baseline=" << (baseline_birthdate.empty() ? "<missing>" : baseline_birthdate)
              << " hl7=" << (hl7_birthdate.empty() ? "<missing>" : hl7_birthdate) << "\n";
    ok = false;
  } else if (baseline_birthdate != hl7_birthdate) {
    std::cerr << "[validate] MISMATCH hl7 birthdate:"
              << " baseline=" << baseline_birthdate
              << " hl7=" << hl7_birthdate << "\n";
    ok = false;
  }

  compare_required_field("observations");
  compare_required_field("loinc_2085_9_matches");
  compare_required_field("obs_value_present");
  compare_required_field("obs_value_quantity");
  compare_required_field("obs_value_codeableconcept");
  compare_required_field("obs_value_string");
  compare_required_field("obs_value_code");
  compare_required_field("obs_effective_datetime");
  compare_required_field("obs_effective_period");
  compare_required_field("obs_issued_present");
  compare_required_field("obs_component_value_present");
  compare_required_field("obs_component_value_quantity");
  compare_required_field("obs_component_value_codeableconcept");
  compare_required_field("obs_component_value_string");
  compare_required_field("obs_component_value_code");

  // Keep FFHR input referenced to make baseline dependency explicit and avoid
  // accidental drift where only one arm is checked.
  (void)ff;
  return ok;
}

}  // namespace bench
