#include "harness.hpp"
#include "hl7v2_message.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#define ARM_HL7V2
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#undef ARM_HL7V2

namespace bench {
namespace {
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
  out.metrics.push_back(test_2::materialize_placeholder("hl7v2"));

  Timer test3_timer;
  test3_timer.start();
  const auto query_summary = test_3::query(payload);
  out.metrics.push_back({"hl7v2", Stage::Test3Query, test3_timer.stop_ns()});
  out.queried_value = test_3::format_query_summary(query_summary);
  out.reconstructed_bundle_json = payload;

  return out;
}

}  // namespace bench
