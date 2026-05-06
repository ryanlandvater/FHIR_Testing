#include "harness.hpp"

#include "proto/google/fhir/proto/r4/core/codes.pb.h"
#include "proto/google/fhir/proto/r4/core/datatypes.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"

#include <cstdint>
#include <stdexcept>
#include <string>

#define ARM_GOOGLE_FHIR
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
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

inline uint32_t read_u32_le(const std::string& in, std::size_t offset) {
  const auto b0 = static_cast<uint8_t>(in[offset + 0]);
  const auto b1 = static_cast<uint8_t>(in[offset + 1]);
  const auto b2 = static_cast<uint8_t>(in[offset + 2]);
  const auto b3 = static_cast<uint8_t>(in[offset + 3]);
  return static_cast<uint32_t>(b0) |
         (static_cast<uint32_t>(b1) << 8) |
         (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

std::string first_patient_id(const std::string& payload) {
  std::size_t cursor = 0;
  while (cursor + 5 <= payload.size()) {
    const char record_type = payload[cursor];
    const uint32_t record_size = read_u32_le(payload, cursor + 1);
    cursor += 5;

    if (cursor + record_size > payload.size()) {
      break;
    }

    if (record_type == 'P') {
      google::fhir::r4::core::Patient patient;
      if (patient.ParseFromArray(payload.data() + cursor, static_cast<int>(record_size)) &&
          patient.has_id()) {
        return patient.id().value();
      }
    }

    cursor += record_size;
  }
  return {};
}

const ObservationData& enrichment_observation_fixture() {
  static const EnrichmentObservationFixture fixture =
      load_enrichment_observation_from_json("bench/enrich.json");
  return fixture.observation;
}

test_4::EnrichResult<std::string> enrich_google_payload(
    const std::string& payload,
    const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  google::fhir::r4::core::Observation obs;
  const auto patient_id = first_patient_id(payload);
  const std::string fallback_patient_id =
      patient_id.empty() ? std::string("benchmark-enrich-patient") : patient_id;
  assign::GoogleObservationTarget observation_target{obs, fallback_patient_id};
  assign::assign_observation(enrichment_observation, observation_target);

  std::string obs_bytes;
  if (!obs.SerializeToString(&obs_bytes)) {
    throw std::runtime_error("Google FHIR enrich failed to serialize Observation");
  }

  std::string enriched_stream = payload;
  append_record(enriched_stream, 'O', obs_bytes);

  test_4::EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = timer.stop_ns();
  return test_4::EnrichResult<std::string>{std::move(enriched_stream), summary};
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

  auto enrich_result = enrich_google_payload(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("google_fhir", enrich_result.summary.duration_ns));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

  return out;
}

}  // namespace bench
