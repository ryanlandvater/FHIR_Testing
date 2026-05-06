#pragma once

#include "harness.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(ARM_FASTFHIR)
#include <FF_Bundle.hpp>
#elif defined(ARM_JSON)
#include <nlohmann/json.hpp>
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

inline EnrichResult<StreamType> enrich(const StreamType& payload,
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

inline EnrichResult<StreamType> enrich(const StreamType& payload,
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

#endif

}  // namespace bench::test_4
