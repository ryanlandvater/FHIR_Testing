#include "harness.hpp"

#include <algorithm>
#include <filesystem>
#include <simdjson.h>
#include <stdexcept>
#include <string>

#include <FF_Ingestor.hpp>

namespace bench {

BundlePatient make_bundle_patient_from_json(const std::filesystem::path& json_path) {
  simdjson::padded_string json_buffer;
  try {
    json_buffer = simdjson::padded_string::load(json_path.string());
  } catch (const std::exception& ex) {
    throw std::runtime_error("Failed to load JSON payload: " + std::string(ex.what()));
  }

  if (json_buffer.size() == 0) {
    throw std::runtime_error("Empty file: " + json_path.string());
  }

  const auto file_size = static_cast<std::size_t>(json_buffer.size());
  const std::string_view ingest_payload(json_buffer.data(), json_buffer.size());

  BundlePatient item{};
  const auto arena_size = std::max<std::size_t>(4096, file_size * static_cast<std::size_t>(2));
  item.memory = FastFHIR::Memory::create(arena_size);

  FastFHIR::Builder builder(item.memory);
  FastFHIR::Ingest::Ingestor ingestor;
  FastFHIR::Reflective::ObjectHandle root(&builder, FF_NULL_OFFSET);
  size_t parsed_count = 0;

  FF_Result result{FF_FAILURE};
  try {
    FastFHIR::Ingest::IngestRequest request{
        .builder = builder,
        .source_type = FastFHIR::Ingest::SourceType::FHIR_JSON,
        .json_string = ingest_payload,
    };
    result = ingestor.ingest(request, root, parsed_count);
  } catch (const std::exception& ex) {
    throw std::runtime_error("Ingestor exception for " + json_path.string() + ": " + ex.what());
  }

  if (!result) {
    throw std::runtime_error(
        "Ingestor failed for " + json_path.string() + ": code="
        + std::to_string(static_cast<int>(result.code)) + ", message=" + result.message);
  }
  if (!root) {
    throw std::runtime_error("Ingestor returned null root for " + json_path.string());
  }

  const auto root_node = root.as_node();
  detail::hydrate_bundle_resources(root_node, item);

  if (item.patient.id.empty()) {
    throw std::runtime_error("No Patient resource in ingested root for " + json_path.string());
  }

  builder.set_root(root);
  (void)builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t size) -> std::vector<BYTE> {
    // No-op checksum callback for benchmarking; real implementation would hash the data.
    return std::vector<BYTE>(32);
  });
  return item;
}

EnrichmentObservationFixture load_enrichment_observation_from_json(const std::filesystem::path& json_path) {
  simdjson::padded_string json_buffer;
  try {
    json_buffer = simdjson::padded_string::load(json_path.string());
  } catch (const std::exception& ex) {
    throw std::runtime_error("Failed to load JSON payload: " + std::string(ex.what()));
  }

  if (json_buffer.size() == 0) {
    throw std::runtime_error("Empty file: " + json_path.string());
  }

  const auto file_size = static_cast<std::size_t>(json_buffer.size());
  const std::string_view ingest_payload(json_buffer.data(), json_buffer.size());

    EnrichmentObservationFixture fixture{};
    fixture.memory = FastFHIR::Memory::create(
      std::max<std::size_t>(4096, file_size * static_cast<std::size_t>(2)));
    FastFHIR::Builder builder(fixture.memory);
  FastFHIR::Ingest::Ingestor ingestor;
  FastFHIR::Reflective::ObjectHandle root(&builder, FF_NULL_OFFSET);
  size_t parsed_count = 0;

  FF_Result result{FF_FAILURE};
  try {
    FastFHIR::Ingest::IngestRequest request{
        .builder = builder,
        .source_type = FastFHIR::Ingest::SourceType::FHIR_JSON,
        .json_string = ingest_payload,
    };
    result = ingestor.ingest(request, root, parsed_count);
  } catch (const std::exception& ex) {
    throw std::runtime_error("Ingestor exception for " + json_path.string() + ": " + ex.what());
  }

  if (!result || !root) {
    throw std::runtime_error(
        "Ingestor failed for " + json_path.string() + ": code="
        + std::to_string(static_cast<int>(result.code)) + ", message=" + result.message);
  }

  const auto root_node = root.as_node();

  if (!(root_node && root_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>())) {
    throw std::runtime_error("Expected Observation resource in " + json_path.string());
  }

  fixture.observation = root_node.as<ObservationData>();
  return fixture;
}

}  // namespace bench
