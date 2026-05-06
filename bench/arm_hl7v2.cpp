#include "harness.hpp"
#include "hl7v2_message.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#define ARM_HL7V2
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
#undef ARM_HL7V2

namespace bench {
namespace {

const ObservationData& enrichment_observation_fixture() {
  static const EnrichmentObservationFixture fixture =
      load_enrichment_observation_from_json("bench/enrich.json");
  return fixture.observation;
}

test_4::EnrichResult<std::string> enrich_hl7v2(const std::string& payload,
                                               const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  hl7v2::OruR01Message message;
  assign::assign_observation(enrichment_observation, message);

  std::string enriched_stream = payload;
  enriched_stream += message.dump();

  test_4::EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = timer.stop_ns();
  return test_4::EnrichResult<std::string>{std::move(enriched_stream), summary};
}

}  // namespace

ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer test1_timer;
  test1_timer.start();

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

  out.metrics.push_back({"hl7v2", Stage::Test1Serialize, test1_timer.stop_ns()});

  Timer test2_timer;
  test2_timer.start();
  const auto materialized = test_2::materialize(payload);
  out.metrics.push_back(test_2::materialize_metric("hl7v2", test2_timer.stop_ns()));
  (void)materialized;

  Timer test3_timer;
  test3_timer.start();
  const auto query_summary = test_3::query(payload);
  out.metrics.push_back({"hl7v2", Stage::Test3Query, test3_timer.stop_ns()});
  out.queried_value = test_3::format_query_summary(query_summary);
  out.reconstructed_bundle_json = payload;

  auto enrich_result = enrich_hl7v2(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("hl7v2", enrich_result.summary.duration_ns));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

  return out;
}

}  // namespace bench
