#include "harness.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#define ARM_JSON
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
#undef ARM_JSON

namespace bench {
namespace {

const ObservationData& enrichment_observation_fixture() {
  static const EnrichmentObservationFixture fixture =
      load_enrichment_observation_from_json("bench/enrich.json");
  return fixture.observation;
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

  Timer test1_timer;
  test1_timer.start();

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
  out.metrics.push_back({"json_fhir", Stage::Test1Serialize, test1_timer.stop_ns()});

  Timer test2_timer;
  test2_timer.start();
  const auto materialized = test_2::materialize(payload);
  out.metrics.push_back(test_2::materialize_metric("json_fhir", test2_timer.stop_ns()));
  (void)materialized;

  Timer test3_timer;
  test3_timer.start();
  const auto query_summary = test_3::query(payload);
  out.metrics.push_back({"json_fhir", Stage::Test3Query, test3_timer.stop_ns()});
  out.queried_value = test_3::format_query_summary(query_summary);
  out.reconstructed_bundle_json = payload;

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("json_fhir", enrich_result.summary.duration_ns));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);
  return out;
}

}  // namespace bench
