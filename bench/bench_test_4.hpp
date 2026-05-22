#pragma once

#include "harness.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(ARM_FASTFHIR)
#include <FF_Bundle.hpp>
#define BENCH_TEST_4_ENRICH_FN enrich_fastfhir
#elif defined(ARM_JSON)
#include <nlohmann/json.hpp>
#define BENCH_TEST_4_ENRICH_FN enrich_json
#elif defined(ARM_HL7V2)
#include "hl7v2_message.hpp"
#define BENCH_TEST_4_ENRICH_FN enrich_hl7v2
#elif defined(ARM_GOOGLE_FHIR)
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#define BENCH_TEST_4_ENRICH_FN enrich_google_fhir
#endif

#include "bench_test_1.hpp"

namespace bench::test_4 {

struct EnrichMetricsSummary {
  std::size_t source_bytes = 0;
  std::size_t enriched_bytes = 0;
  std::size_t appended_observations = 0;
  std::int64_t duration_ns = 0;
};

template <typename StreamT>
struct EnrichResult {
  StreamT enriched_stream;
  EnrichMetricsSummary summary;
};

inline std::string format_enrich_summary(const EnrichMetricsSummary& summary) {
  return "source_bytes=" + std::to_string(summary.source_bytes) +
         " enriched_bytes=" + std::to_string(summary.enriched_bytes) +
         " appended_observations=" + std::to_string(summary.appended_observations) +
         " duration_ns=" + std::to_string(summary.duration_ns);
}

inline MetricEvent enrich_metric(std::string_view arm, std::int64_t duration_ns) {
  return MetricEvent{std::string(arm), Stage::Test4Enrich, duration_ns};
}

#if defined(ARM_FASTFHIR)

using StreamType = FastFHIR::Memory;

inline EnrichResult<StreamType> enrich_fastfhir(const StreamType& payload,
                                                const ObservationData& enrichment_observation) {
  StreamType enriched_stream = payload;
  FastFHIR::Builder builder(enriched_stream, FHIR_VERSION_R5);

  Timer timer;
  timer.start();

  auto root_handle = builder.root_handle();
  auto root_node = root_handle.as_node();
  if (!(root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>())) {
    throw std::runtime_error("test_4::enrich expected Bundle root in FastFHIR stream");
  }

  BundleData bundle = root_node.as<BundleData>();
  auto observation_handle = builder.append_obj(ObservationData{});
  assign::assign_observation(enrichment_observation, observation_handle);
  bundle.entry.push_back(BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)});

  auto new_root = builder.append_obj(bundle);
  builder.set_root(new_root);
  (void)builder.finalize(FF_CHECKSUM_SHA256,
                         [](const unsigned char*, size_t) -> std::vector<BYTE> {
                           return std::vector<BYTE>(32);
                         });

  EnrichMetricsSummary summary;
  summary.source_bytes = payload.view().size();
  summary.enriched_bytes = enriched_stream.view().size();
  summary.appended_observations = 1;
  summary.duration_ns = timer.stop_ns();
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#elif defined(ARM_JSON)

using StreamType = std::string;

inline EnrichResult<StreamType> enrich_json(const StreamType& payload,
                                            const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  nlohmann::json bundle = nlohmann::json::parse(payload);
  nlohmann::json observation;
  assign::assign_observation(enrichment_observation, observation);

  if (!bundle.contains("entry") || !bundle["entry"].is_array()) {
    bundle["entry"] = nlohmann::json::array();
  }
  bundle["entry"].push_back(nlohmann::json{{"resource", std::move(observation)}});

  StreamType enriched_stream = bundle.dump();

  EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = timer.stop_ns();
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#elif defined(ARM_HL7V2)

using StreamType = std::string;

inline EnrichResult<StreamType> enrich_hl7v2(const StreamType& payload,
                                             const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  hl7v2::OruR01Message message;
  assign::assign_observation(enrichment_observation, message);

  StreamType enriched_stream = payload;
  enriched_stream += message.dump();

  EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = timer.stop_ns();
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#elif defined(ARM_GOOGLE_FHIR)

using StreamType = std::string;

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

inline std::string first_patient_id(const std::string& payload) {
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

inline void append_record(std::string& payload, char record_type, const std::string& record_bytes) {
  payload.push_back(record_type);
  const uint32_t len = static_cast<uint32_t>(record_bytes.size());
  payload.push_back(static_cast<char>(len & 0xFFu));
  payload.push_back(static_cast<char>((len >> 8) & 0xFFu));
  payload.push_back(static_cast<char>((len >> 16) & 0xFFu));
  payload.push_back(static_cast<char>((len >> 24) & 0xFFu));
  payload.append(record_bytes);
}

inline EnrichResult<StreamType> enrich_google_fhir(const StreamType& payload,
                                                   const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  // Materialize all protobuf records first, then mutate and re-serialize the full stream.
  std::vector<google::fhir::r4::core::Patient> patients;
  std::vector<google::fhir::r4::core::Observation> observations;
  std::vector<std::pair<char, std::size_t>> record_order;

  patients.reserve(256);
  observations.reserve(1024);
  record_order.reserve(2048);

  std::size_t cursor = 0;
  while (cursor + 5 <= payload.size()) {
    const char record_type = payload[cursor];
    const uint32_t record_size = read_u32_le(payload, cursor + 1);
    cursor += 5;

    if (cursor + record_size > payload.size()) {
      throw std::runtime_error("test_4::enrich_google_fhir encountered truncated protobuf record");
    }

    const char* record_ptr = payload.data() + cursor;
    if (record_type == 'P') {
      google::fhir::r4::core::Patient patient;
      if (!patient.ParseFromArray(record_ptr, static_cast<int>(record_size))) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to parse Patient record");
      }
      patients.push_back(std::move(patient));
      record_order.push_back({'P', patients.size() - 1});
    } else if (record_type == 'O') {
      google::fhir::r4::core::Observation observation;
      if (!observation.ParseFromArray(record_ptr, static_cast<int>(record_size))) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to parse Observation record");
      }
      observations.push_back(std::move(observation));
      record_order.push_back({'O', observations.size() - 1});
    } else {
      throw std::runtime_error("test_4::enrich_google_fhir encountered unknown protobuf record type");
    }

    cursor += record_size;
  }

  google::fhir::r4::core::Observation observation;
  std::string patient_id;
  for (const auto& patient : patients) {
    if (patient.has_id()) {
      patient_id = patient.id().value();
      break;
    }
  }
  const std::string fallback_patient_id =
      patient_id.empty() ? std::string("benchmark-enrich-patient") : patient_id;
  assign::GoogleObservationTarget target{observation, fallback_patient_id};
  assign::assign_observation(enrichment_observation, target);

  observations.push_back(std::move(observation));
  record_order.push_back({'O', observations.size() - 1});

  StreamType enriched_stream;
  enriched_stream.reserve(payload.size() + 512);
  for (const auto& [record_type, idx] : record_order) {
    std::string record_bytes;
    if (record_type == 'P') {
      if (!patients[idx].SerializeToString(&record_bytes)) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to serialize Patient record");
      }
    } else {
      if (!observations[idx].SerializeToString(&record_bytes)) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to serialize Observation record");
      }
    }
    append_record(enriched_stream, record_type, record_bytes);
  }

  EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = timer.stop_ns();
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#endif

}  // namespace bench::test_4
