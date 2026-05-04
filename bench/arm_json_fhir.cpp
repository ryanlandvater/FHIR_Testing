#include "harness.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#define ARM_JSON
#include "bench_assign.hpp"
#undef ARM_JSON

namespace bench {
namespace {
struct ObservationSemanticCounts {
  std::size_t observations = 0;
  std::size_t value_present = 0;
  std::size_t value_quantity = 0;
  std::size_t value_codeableconcept = 0;
  std::size_t value_string = 0;
  std::size_t value_code = 0;
  std::size_t effective_datetime = 0;
  std::size_t effective_period = 0;
  std::size_t issued_present = 0;
  std::size_t component_value_present = 0;
  std::size_t component_value_quantity = 0;
  std::size_t component_value_codeableconcept = 0;
  std::size_t component_value_string = 0;
  std::size_t component_value_code = 0;
};

struct ResourceSemanticStats {
  std::size_t patient_count = 0;
  std::size_t cholesterol_matches = 0;
  ObservationSemanticCounts observation_counts{};
};

static inline void merge_observation_counts(ObservationSemanticCounts& dst,
                                            const ObservationSemanticCounts& src) {
  dst.observations += src.observations;
  dst.value_present += src.value_present;
  dst.value_quantity += src.value_quantity;
  dst.value_codeableconcept += src.value_codeableconcept;
  dst.value_string += src.value_string;
  dst.value_code += src.value_code;
  dst.effective_datetime += src.effective_datetime;
  dst.effective_period += src.effective_period;
  dst.issued_present += src.issued_present;
  dst.component_value_present += src.component_value_present;
  dst.component_value_quantity += src.component_value_quantity;
  dst.component_value_codeableconcept += src.component_value_codeableconcept;
  dst.component_value_string += src.component_value_string;
  dst.component_value_code += src.component_value_code;
}

static inline void accumulate_json_choice_value_semantics(const nlohmann::json& object,
                                                          std::size_t& present,
                                                          std::size_t& quantity,
                                                          std::size_t& codeable_concept,
                                                          std::size_t& string,
                                                          std::size_t& code) {
  const auto value_quantity_it = object.find("valueQuantity");
  if (value_quantity_it != object.end() && value_quantity_it->is_object()) {
    ++present;
    ++quantity;
    return;
  }

  const auto value_codeable_concept_it = object.find("valueCodeableConcept");
  if (value_codeable_concept_it != object.end() && value_codeable_concept_it->is_object()) {
    ++present;
    ++codeable_concept;
    return;
  }

  const auto value_string_it = object.find("valueString");
  if (value_string_it != object.end() && value_string_it->is_string()) {
    ++present;
    ++string;
    return;
  }

  const auto value_code_it = object.find("valueCode");
  if (value_code_it != object.end() && value_code_it->is_string()) {
    ++present;
    ++code;
  }
}

static inline void accumulate_observation_semantics(const nlohmann::json& resource,
                                                    ObservationSemanticCounts& counts) {
  ++counts.observations;
  accumulate_json_choice_value_semantics(resource,
                                         counts.value_present,
                                         counts.value_quantity,
                                         counts.value_codeableconcept,
                                         counts.value_string,
                                         counts.value_code);

  const auto issued_it = resource.find("issued");
  if (issued_it != resource.end() && issued_it->is_string() && !issued_it->get_ref<const std::string&>().empty()) {
    ++counts.issued_present;
  }

  const auto effective_datetime_it = resource.find("effectiveDateTime");
  if (effective_datetime_it != resource.end() && effective_datetime_it->is_string()) {
    ++counts.effective_datetime;
  }

  const auto effective_period_it = resource.find("effectivePeriod");
  if (effective_period_it != resource.end() && effective_period_it->is_object()) {
    ++counts.effective_period;
  }

  const auto component_it = resource.find("component");
  if (component_it != resource.end() && component_it->is_array()) {
    for (const auto& component : *component_it) {
      if (!component.is_object()) {
        continue;
      }
      accumulate_json_choice_value_semantics(component,
                                             counts.component_value_present,
                                             counts.component_value_quantity,
                                             counts.component_value_codeableconcept,
                                             counts.component_value_string,
                                             counts.component_value_code);
    }
  }
}

static inline bool observation_has_cholesterol_loinc(const nlohmann::json& resource) {
  const auto code_it = resource.find("code");
  if (code_it == resource.end() || !code_it->is_object()) {
    return false;
  }
  const auto coding_it = code_it->find("coding");
  if (coding_it == code_it->end() || !coding_it->is_array()) {
    return false;
  }

  for (const auto& coding : *coding_it) {
    if (!coding.is_object()) {
      continue;
    }
    const auto code_value_it = coding.find("code");
    if (code_value_it == coding.end() || !code_value_it->is_string()) {
      continue;
    }
    if (code_value_it->get<std::string_view>() != kCholesterolLoincCode) {
      continue;
    }
    const auto system_it = coding.find("system");
    if (system_it == coding.end() || !system_it->is_string() ||
        system_it->get<std::string_view>() == kLoincSystem) {
      return true;
    }
  }

  return false;
}

static inline ResourceSemanticStats analyze_resource(const nlohmann::json& resource) {
  ResourceSemanticStats stats{};
  const auto type_it = resource.find("resourceType");
  if (type_it == resource.end() || !type_it->is_string()) {
    return stats;
  }

  const std::string_view resource_type = type_it->get<std::string_view>();
  if (resource_type == "Patient") {
    stats.patient_count = 1;
    return stats;
  }
  if (resource_type == "Observation") {
    if (observation_has_cholesterol_loinc(resource)) {
      stats.cholesterol_matches = 1;
    }
    accumulate_observation_semantics(resource, stats.observation_counts);
  }
  return stats;
}

#if defined(__APPLE__)
struct JsonPatientBuildContext {
  const std::vector<BundlePatient>* bundle;
  std::vector<nlohmann::json>* entries;
};

struct JsonObservationBuildContext {
  const std::vector<const ObservationData*>* observations;
  std::vector<nlohmann::json>* entries;
};

static inline void build_patient_entry(void* raw_context, std::size_t idx) {
  auto* context = static_cast<JsonPatientBuildContext*>(raw_context);
  nlohmann::json patient_json;
  assign::assign_patient((*context->bundle)[idx].patient, patient_json);
  (*context->entries)[idx] = nlohmann::json{{"resource", std::move(patient_json)}};
}

static inline void build_observation_entry(void* raw_context, std::size_t idx) {
  auto* context = static_cast<JsonObservationBuildContext*>(raw_context);
  nlohmann::json observation_json;
  assign::assign_observation(*(*context->observations)[idx], observation_json);
  (*context->entries)[idx] = nlohmann::json{{"resource", std::move(observation_json)}};
}
#endif

}  // namespace

ArmRunResult run_json_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer stage1_timer;
  stage1_timer.start();

  nlohmann::json bundle;
  bundle["resourceType"] = "Bundle";
  bundle["type"] = "collection";
  bundle["entry"] = nlohmann::json::array();

  std::vector<nlohmann::json> patient_entries(fixture.bundle.size());
#if defined(__APPLE__)
  JsonPatientBuildContext patient_context{&fixture.bundle, &patient_entries};
  dispatch_apply_f(patient_entries.size(),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                   &patient_context,
                   build_patient_entry);
#else
  std::transform(
      fixture.bundle.begin(),
      fixture.bundle.end(),
      patient_entries.begin(),
      [](const BundlePatient& item) -> nlohmann::json {
        nlohmann::json patient_json;
        assign::assign_patient(item.patient, patient_json);
        return nlohmann::json{{"resource", std::move(patient_json)}};
      });
#endif

  std::size_t total_observations = 0;
  for (const auto& item : fixture.bundle) {
    total_observations += item.observations.size();
  }

  std::vector<const ObservationData*> observation_ptrs;
  observation_ptrs.reserve(total_observations);
  for (const auto& item : fixture.bundle) {
    for (const auto& observation : item.observations) {
      observation_ptrs.push_back(&observation);
    }
  }

  std::vector<nlohmann::json> observation_entries(observation_ptrs.size());
#if defined(__APPLE__)
  JsonObservationBuildContext observation_context{&observation_ptrs, &observation_entries};
  dispatch_apply_f(observation_entries.size(),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                   &observation_context,
                   build_observation_entry);
#else
  std::transform(
      observation_ptrs.begin(),
      observation_ptrs.end(),
      observation_entries.begin(),
      [](const ObservationData* observation) -> nlohmann::json {
        nlohmann::json observation_json;
        assign::assign_observation(*observation, observation_json);
        return nlohmann::json{{"resource", std::move(observation_json)}};
      });
#endif

  nlohmann::json::array_t all_entries;
  all_entries.reserve(patient_entries.size() + observation_entries.size());
  for (auto& entry : patient_entries) {
    all_entries.push_back(std::move(entry));
  }
  for (auto& entry : observation_entries) {
    all_entries.push_back(std::move(entry));
  }
  bundle["entry"] = std::move(all_entries);

  const std::string payload = bundle.dump();
  out.metrics.push_back({"json_fhir", Stage::Stage1Serialize, stage1_timer.stop_ns()});

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
  ObservationSemanticCounts observation_counts{};

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
      const auto stats = analyze_resource(resource);
      patient_count += stats.patient_count;
      cholesterol_matches += stats.cholesterol_matches;
      merge_observation_counts(observation_counts, stats.observation_counts);

      if (birthdate.empty()) {
        const auto type_it = resource.find("resourceType");
        if (type_it != resource.end() && type_it->is_string() &&
            type_it->get<std::string_view>() == "Patient") {
          const auto bd_it = resource.find("birthDate");
          if (bd_it != resource.end() && bd_it->is_string()) {
            birthdate = bd_it->get<std::string>();
          }
        }
      }
    }
  }

  out.metrics.push_back({"json_fhir", Stage::Stage3Query, stage3_timer.stop_ns()});
  out.queried_value =
      "patients=" + std::to_string(patient_count) +
      " birthdate=" + birthdate +
      " observations=" + std::to_string(observation_counts.observations) +
      " loinc_2085_9_matches=" + std::to_string(cholesterol_matches) +
      " obs_value_present=" + std::to_string(observation_counts.value_present) +
      " obs_value_quantity=" + std::to_string(observation_counts.value_quantity) +
      " obs_value_codeableconcept=" + std::to_string(observation_counts.value_codeableconcept) +
      " obs_value_string=" + std::to_string(observation_counts.value_string) +
      " obs_value_code=" + std::to_string(observation_counts.value_code) +
      " obs_effective_datetime=" + std::to_string(observation_counts.effective_datetime) +
      " obs_effective_period=" + std::to_string(observation_counts.effective_period) +
      " obs_issued_present=" + std::to_string(observation_counts.issued_present) +
      " obs_component_value_present=" + std::to_string(observation_counts.component_value_present) +
      " obs_component_value_quantity=" + std::to_string(observation_counts.component_value_quantity) +
      " obs_component_value_codeableconcept=" + std::to_string(observation_counts.component_value_codeableconcept) +
      " obs_component_value_string=" + std::to_string(observation_counts.component_value_string) +
      " obs_component_value_code=" + std::to_string(observation_counts.component_value_code);
  out.reconstructed_bundle_json = payload;
  return out;
}

}  // namespace bench
