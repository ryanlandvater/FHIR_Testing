#include <openssl/sha.h>
#include "harness.hpp"

#include <memory>

namespace bench {

ArmRunResult run_fastfhir_smoke(const PatientData& patient) {
  ArmRunResult result;
  result.metrics.reserve(2);

  // Create a Memory arena for the FastFHIR payload.
  auto mem = FastFHIR::Memory::create(64 * 1024 * 1024);
  FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

  // Stage 1 start: immediately before the first field write (during ingest/builder operations).
  Timer stage1;
  stage1.start();

  // Build a Patient resource from in-memory PatientData using assignment operators.
  PatientData scaffold{};
  auto patient_handle = builder.append_obj(scaffold);
  patient_handle[FastFHIR::Fields::PATIENT::ID] = patient.id;
  patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = patient.active;

  const std::string gender_literal = FF_AdministrativeGenderToString(patient.gender);
  patient_handle[FastFHIR::Fields::PATIENT::GENDER] = FF_GetDictionaryCode(gender_literal, FHIR_VERSION_R5);
  patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = patient.birthdate;

  // Set as root and finalize to seal the payload.
  builder.set_root(patient_handle);
  auto view = builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t len) -> std::vector<BYTE> {
    return std::vector<uint8_t> (SHA256_DIGEST_LENGTH);
});

  // Stage 1 end: payload is sealed and ready for read-side operations.
  result.metrics.push_back(
      MetricEvent{"fastfhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  // Stage 3 start: first parser/read call that consumes bytes for query.
  Timer stage3;
  stage3.start();

  // Parse the sealed FFHR payload.
  FastFHIR::Parser parser(view.data(), view.size());
  auto root = parser.root();

  if (root) {
    // Query the birthDate field using the typed field key.
    auto birthdate_entry = root[FastFHIR::Fields::PATIENT::BIRTH_DATE];
    if (birthdate_entry) {
      result.queried_value = std::string(birthdate_entry.as<std::string_view>());
    }
  }

  // Stage 3 end: target value extracted into result variable.
  result.metrics.push_back(
      MetricEvent{"fastfhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  if (result.queried_value.empty()) {
    result.metrics.back().duration_us = std::max<std::int64_t>(result.metrics.back().duration_us, 1);
  }

  return result;
}

}  // namespace bench
