#include "harness.hpp"

#include <FF_Bundle.hpp>

#include <algorithm>
#include <execution>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#define ARM_FASTFHIR
#include "bench_assign.hpp"
#undef ARM_FASTFHIR

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

static inline void accumulate_choice_value_semantics(const ChoiceEntry& choice,
                                                     std::size_t& present,
                                                     std::size_t& quantity,
                                                     std::size_t& codeable_concept,
                                                     std::size_t& string,
                                                     std::size_t& code) {
  if (choice.is_empty()) {
    return;
  }

  ++present;
  switch (choice.tag) {
    case RECOVER_FF_QUANTITY:
      ++quantity;
      break;
    case RECOVER_FF_CODEABLECONCEPT:
      ++codeable_concept;
      break;
    case RECOVER_FF_STRING:
      ++string;
      break;
    case RECOVER_FF_CODE:
      ++code;
      break;
    default:
      break;
  }
}

static inline void accumulate_observation_semantics(const ObservationData& observation,
                                                    ObservationSemanticCounts& counts) {
  ++counts.observations;
  accumulate_choice_value_semantics(observation.value,
                                    counts.value_present,
                                    counts.value_quantity,
                                    counts.value_codeableconcept,
                                    counts.value_string,
                                    counts.value_code);

  if (!observation.issued.empty()) {
    ++counts.issued_present;
  }

  switch (observation.effective.tag) {
    case RECOVER_FF_DATETIME:
      ++counts.effective_datetime;
      break;
    case RECOVER_FF_PERIOD:
      ++counts.effective_period;
      break;
    default:
      break;
  }

  for (const auto& component : observation.component) {
    accumulate_choice_value_semantics(component.value,
                                      counts.component_value_present,
                                      counts.component_value_quantity,
                                      counts.component_value_codeableconcept,
                                      counts.component_value_string,
                                      counts.component_value_code);
  }
}

#if defined(__APPLE__)
struct EntryBuildContext {
  FastFHIR::Builder* builder;
  const std::vector<BundlePatient>* bundle;
  std::vector<BundleentryData>* entries;
};

struct ObservationBuildContext {
  FastFHIR::Builder* builder;
  const std::vector<const ObservationData*>* observations;
  std::vector<BundleentryData>* entries;
};

static inline void build_bundle_entry(void* raw_context, std::size_t idx) {
  auto* context = static_cast<EntryBuildContext*>(raw_context);
  const auto& item = (*context->bundle)[idx];
  auto patient_handle = context->builder->append_obj(PatientData{});
  assign::assign_patient(item.patient, patient_handle);
  (*context->entries)[idx] = BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
}

static inline void build_observation_entry(void* raw_context, std::size_t idx) {
  auto* context = static_cast<ObservationBuildContext*>(raw_context);
  const auto& observation = *(*context->observations)[idx];
  auto observation_handle = context->builder->append_obj(ObservationData{});
  assign::assign_observation(observation, observation_handle);
  (*context->entries)[idx] = BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)};
}
#endif

}  // namespace

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;


  std::size_t arena_hint = 4096;
  std::size_t total_observation_count = 0;
  for (const auto& p : fixture.bundle) {
    arena_hint += p.memory.size();
    total_observation_count += p.observations.size();
  }
  FastFHIR::Memory payload_memory = FastFHIR::Memory::create(arena_hint);
  FastFHIR::Builder builder(payload_memory, FHIR_VERSION_R5);

  Timer stage1_timer;
  stage1_timer.start();

  BundleData bundle{};
  bundle.type = BundleType::Collection;

  std::vector<BundleentryData> entries(fixture.bundle.size());
// #if defined(__APPLE__)
//   EntryBuildContext context{&builder, &fixture.bundle, &entries};
//   dispatch_apply_f(entries.size(), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &context,
//                    build_bundle_entry);
// #elif defined(__cpp_lib_execution) && (__cpp_lib_execution >= 201603L)
//   std::transform(
//       std::execution::par_unseq,
//       fixture.bundle.begin(),
//       fixture.bundle.end(),
//       entries.begin(),
//       [&builder](const BundlePatient& item) -> BundleentryData {
//         auto patient_handle = builder.append_obj(PatientData{});
//         assign::assign_patient(item.patient, patient_handle);
//         return BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
//       });
// #else
  std::transform(
      fixture.bundle.begin(),
      fixture.bundle.end(),
      entries.begin(),
      [&builder](const BundlePatient& item) -> BundleentryData {
        auto patient_handle = builder.append_obj(PatientData{});
        assign::assign_patient(item.patient, patient_handle);
        return BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
      });
// #endif
  std::vector<const ObservationData*> observation_ptrs;
  observation_ptrs.reserve(total_observation_count);
  for (const auto& item : fixture.bundle) {
    for (const auto& observation : item.observations) {
      observation_ptrs.push_back(&observation);
    }
  }

  std::vector<BundleentryData> observation_entries(observation_ptrs.size());
// #if defined(__APPLE__)
//   ObservationBuildContext observation_context{&builder, &observation_ptrs, &observation_entries};
//   dispatch_apply_f(observation_entries.size(),
//                    dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
//                    &observation_context,
//                    build_observation_entry);
// #elif defined(__cpp_lib_execution) && (__cpp_lib_execution >= 201603L)
//   std::transform(
//       std::execution::par_unseq,
//       observation_ptrs.begin(),
//       observation_ptrs.end(),
//       observation_entries.begin(),
//       [&builder](const ObservationData* observation) -> BundleentryData {
//         auto observation_handle = builder.append_obj(ObservationData{});
//         assign::assign_observation(*observation, observation_handle);
//         return BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)};
//       });
// #else
  std::transform(
      observation_ptrs.begin(),
      observation_ptrs.end(),
      observation_entries.begin(),
      [&builder](const ObservationData* observation) -> BundleentryData {
        auto observation_handle = builder.append_obj(ObservationData{});
        assign::assign_observation(*observation, observation_handle);
        return BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)};
      });
// #endif

  bundle.entry = std::move(entries);
  if (!observation_entries.empty()) {
    bundle.entry.insert(bundle.entry.end(),
                        std::make_move_iterator(observation_entries.begin()),
                        std::make_move_iterator(observation_entries.end()));
  }

  const auto root = builder.append_obj(bundle);
  builder.set_root(root);
  (void)builder.finalize(FF_CHECKSUM_SHA256,
                         [](const unsigned char*, size_t) -> std::vector<BYTE> {
                           return std::vector<BYTE>(32);
                         });
  out.metrics.push_back({"fastfhir", Stage::Stage1Serialize, stage1_timer.stop_ns()});

  Timer stage3_timer;
  stage3_timer.start();
  std::string birthdate;
  std::size_t cholesterol_matches = 0;
  std::size_t patient_count = 0;
  ObservationSemanticCounts observation_counts{};

  if (payload_memory) {
    FastFHIR::Parser parser(payload_memory);
    const auto root_node = parser.root();
    if (root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
      if (auto entries = root_node[FastFHIR::Fields::BUNDLE::ENTRY]) {
        for (auto& entry : entries.entries()) {
          auto resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
          if (!resource) {
            continue;
          }
          auto resource_node = resource.as_node();
          if (!resource_node) {
            continue;
          }
          if (resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
            ++patient_count;
            if (birthdate.empty()) {
#if defined(BENCH_ASSIGN_FORCE_FULL_CAST)
              const auto patient = resource_node.as<PatientData>();
              birthdate = std::string(patient.birthdate);
#else
              auto birth_date_entry = resource_node[FastFHIR::Fields::PATIENT::BIRTH_DATE];
              if (birth_date_entry) {
                birthdate = std::string(birth_date_entry.as<std::string_view>());
              }
#endif
            }
          } else if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
            auto code_entry = resource_node[FastFHIR::Fields::OBSERVATION::CODE];
            if (!code_entry) {
              const auto observation = resource_node.as<ObservationData>();
              accumulate_observation_semantics(observation, observation_counts);
              continue;
            }
            auto coding_entries = code_entry[FastFHIR::Fields::CODEABLECONCEPT::CODING];
            if (!coding_entries) {
              const auto observation = resource_node.as<ObservationData>();
              accumulate_observation_semantics(observation, observation_counts);
              continue;
            }
            bool is_cholesterol = false;
            for (auto& coding : coding_entries.entries()) {
              auto system_entry = coding[FastFHIR::Fields::CODING::SYSTEM];
              auto code_value_entry = coding[FastFHIR::Fields::CODING::CODE];
              if (!code_value_entry) {
                continue;
              }
              const auto code_value = code_value_entry.as<std::string_view>();
              if (code_value != kCholesterolLoincCode) {
                continue;
              }
              if (!system_entry || system_entry.as<std::string_view>() == kLoincSystem) {
                is_cholesterol = true;
                break;
              }
            }
            if (is_cholesterol) {
              ++cholesterol_matches;
            }

            const auto observation = resource_node.as<ObservationData>();
            accumulate_observation_semantics(observation, observation_counts);
          }
        }
      }
    }
  }

  out.metrics.push_back({"fastfhir", Stage::Stage3Query, stage3_timer.stop_ns()});
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
  return out;
}

}  // namespace bench
