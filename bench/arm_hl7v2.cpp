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

  const std::int64_t test1_ns = test1_timer.stop_ns();
  // Wire size is read AFTER the clock stops (notes.md section 6).
  const std::int64_t test1_bytes = static_cast<std::int64_t>(payload.size());
  out.metrics.push_back({"hl7v2", Stage::Test1Serialize, test1_ns, 0, test1_bytes});

  Timer test3_timer;
  test3_timer.start();
  const auto query_summary = test_3::query(payload);
  out.metrics.push_back({"hl7v2", Stage::Test3Query, test3_timer.stop_ns()});
  out.queried_value = test_3::format_query_summary(query_summary);
  out.reconstructed_bundle_json = payload;

  // Test 2 -- random access (IN-B / WF-1.1). Out-of-order reads, navigating
  // from the root each time; the retired materialize walk read in layout
  // order, and the two disagree by three orders of magnitude.
  //
  // MUST run BEFORE Test 4. FastFHIR::Memory is a shared_ptr handle, so the
  // enrich appends into this very arena (PA-9) -- running Test 2 afterwards
  // had the FastFHIR arm reading 1,474 entries while the other arms read
  // 1,473, and the cross-arm byte gate caught it.
  {
    const auto ra = test_2::random_access(payload);
    out.metrics.push_back(test_2::random_access_metric("hl7v2", ra));
    out.random_access_summary = test_2::format_random_access_summary(ra);
  }

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("hl7v2", enrich_result.summary));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

  return out;
}

}  // namespace bench
