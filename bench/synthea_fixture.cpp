#include "harness.hpp"

#include <algorithm>
#include <filesystem>
#include <simdjson.h>
#include <cstdio>
#include <cstdlib>
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

  const FastFHIR::FF_Stream stream = make_stream(item.memory);
  FastFHIR::Ingest::Ingestor ingestor;
  FastFHIR::Reflective::ObjectHandle root(stream.get(), FF_NULL_OFFSET);
  size_t parsed_count = 0;

  FF_Result result{FF_FAILURE};
  try {
    FastFHIR::Ingest::IngestRequest request{
        .builder = *stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_ALL_KNOWN,
        .json_string = ingest_payload,
        // padded_string guarantees SIMDJSON_PADDING slack past size(), so the
        // ingestor can parse in place instead of making a padded copy of the
        // whole document.
        .payload_capacity = json_buffer.size() + simdjson::SIMDJSON_PADDING,
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

  (void)seal_stream(stream, root, json_path.string());
  if (std::getenv("BENCH_VALIDATE_INGEST")) {
    FastFHIR::Parser check(item.memory);
    const FF_Result vr = check.validate_FFHR_stream();
    std::fprintf(stderr, "[validate-ingest] %s: code=%d %s\n",
                 json_path.filename().string().c_str(), (int)vr.code, vr.message.c_str());
  }
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
  const FastFHIR::FF_Stream stream = make_stream(fixture.memory);
  FastFHIR::Ingest::Ingestor ingestor;
  FastFHIR::Reflective::ObjectHandle root(stream.get(), FF_NULL_OFFSET);
  size_t parsed_count = 0;

  FF_Result result{FF_FAILURE};
  try {
    FastFHIR::Ingest::IngestRequest request{
        .builder = *stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_ALL_KNOWN,
        .json_string = ingest_payload,
        .payload_capacity = json_buffer.size() + simdjson::SIMDJSON_PADDING,
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
