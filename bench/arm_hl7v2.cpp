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
  // Observe the walk result. Without this the compiler is free to elide
  // touch_tree() entirely -- the FastFHIR arm reported 83 ns for a 317-entry
  // bundle before this check existed, which was a dead-code artefact rather
  // than a zero-copy result. See notes.md section 2.
  if (materialized.touched_nodes == 0 && materialized.ok) {
    std::cerr << "[warn] materialize touched 0 nodes\n";
  }
  if (std::getenv("BENCH_TOUCHED")) {
    std::cerr << "[touched] " << out.metrics.back().arm << " nodes="
              << materialized.touched_nodes << " ok=" << materialized.ok << "\n";
  }

  Timer test3_timer;
  test3_timer.start();
  const auto query_summary = test_3::query(payload);
  out.metrics.push_back({"hl7v2", Stage::Test3Query, test3_timer.stop_ns()});
  out.queried_value = test_3::format_query_summary(query_summary);
  out.reconstructed_bundle_json = payload;

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("hl7v2", enrich_result.summary.duration_ns));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

  return out;
}

}  // namespace bench
