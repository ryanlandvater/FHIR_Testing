#include "harness.hpp"

#include "proto/google/fhir/proto/r4/core/codes.pb.h"
#include "proto/google/fhir/proto/r4/core/datatypes.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"

#include <cstdint>
#include <string>

#define ARM_GOOGLE_FHIR
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#undef ARM_GOOGLE_FHIR

namespace bench {

namespace {

inline void append_u32_le(std::string& out, uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

inline void append_record(std::string& payload, char record_type, const std::string& record_bytes) {
  payload.push_back(record_type);
  append_u32_le(payload, static_cast<uint32_t>(record_bytes.size()));
  payload.append(record_bytes);
}

}  // namespace

ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer test1_timer;
  test1_timer.start();

  std::string payload;
  payload.reserve(fixture.bundle.size() * 320);

  for (const auto& item : fixture.bundle) {
    google::fhir::r4::core::Patient patient;
    assign::GooglePatientTarget patient_target{patient};
    assign::assign_patient(item.patient, patient_target);

    std::string patient_bytes;
    if (patient.SerializeToString(&patient_bytes)) {
      append_record(payload, 'P', patient_bytes);
    }

    for (const auto& observation : item.observations) {
      google::fhir::r4::core::Observation obs;
      assign::GoogleObservationTarget observation_target{obs, item.patient.id};
      assign::assign_observation(observation, observation_target);

      std::string obs_bytes;
      if (obs.SerializeToString(&obs_bytes)) {
        append_record(payload, 'O', obs_bytes);
      }
    }
  }

  out.metrics.push_back({"google_fhir", Stage::Test1Serialize, test1_timer.stop_ns()});

  Timer test2_timer;
  test2_timer.start();
  const auto materialized = test_2::materialize(payload);
  out.metrics.push_back(test_2::materialize_metric("google_fhir", test2_timer.stop_ns()));
  (void)materialized;

  Timer test3_timer;
  test3_timer.start();
  const auto summary = test_3::query(payload);
  out.metrics.push_back({"google_fhir", Stage::Test3Query, test3_timer.stop_ns()});

  out.queried_value = test_3::format_query_summary(summary);
  out.reconstructed_bundle_json = "protobuf_payload_bytes=" + std::to_string(payload.size());
  return out;
}

}  // namespace bench
