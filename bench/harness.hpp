#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Patient.hpp>

namespace bench {

// Timed sections are declared here first for manual reviewability.
// Stage 1 start: immediately before first field write to destination representation.
// Stage 1 end: immediately after payload sealed and before transport preparation.
// Stage 2 start: immediately before send API call.
// Stage 2 end: on transport completion callback/confirmation.
// Stage 3/7.2 start: first parser/read call that consumes bytes for query.
// Stage 3/7.2 end: target value extracted into result variable.

enum class Stage {
  Stage1Serialize,
  Stage2Transport,
  Stage3Query,
  Stage3Materialize
};

struct MetricEvent {
  std::string arm;
  Stage stage;
  std::int64_t duration_us;
};

struct ArmRunResult {
  std::vector<MetricEvent> metrics;
  std::string queried_value;
};

class Timer {
 public:
  void start() { begin_ = std::chrono::steady_clock::now(); }
  std::int64_t stop_us() const {
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - begin_).count();
  }

 private:
  std::chrono::steady_clock::time_point begin_{};
};

inline std::string to_string(Stage s) {
  switch (s) {
    case Stage::Stage1Serialize:
      return "stage1_serialize";
    case Stage::Stage2Transport:
      return "stage2_transport";
    case Stage::Stage3Query:
      return "stage3_query";
    case Stage::Stage3Materialize:
      return "stage3_materialize";
  }
  return "unknown";
}

inline void print_metric(const MetricEvent& e) {
  std::cout << e.arm << "," << to_string(e.stage) << "," << e.duration_us << "\n" << std::flush;
}

inline constexpr std::string_view kPatientQueryField = "birthDate";
inline constexpr std::string_view kCholesterolLoincCode = "2085-9";
inline constexpr std::string_view kLoincSystem = "http://loinc.org";

// All string fields are owned values so SyntheaFixture is safely copyable.
struct SyntheaFixture {
  // Owned strings — PatientData string_views point into these.
  std::string patient_id_storage;
  std::string patient_birthdate_storage;

  // PatientData backed by the owned strings above.
  // NOTE: unique_ptr fields (meta, text, etc.) are left null — only
  // id, birthdate, gender, and active are used by the benchmark arms.
  PatientData patient;

  int64_t ffhr_size_bytes = 0;  // Size of the .ffhr file this was read from.
};

struct BundleBenchFixture {
  std::vector<SyntheaFixture> patients;
  int64_t target_size_bytes = 0;
  int64_t actual_ingested_bytes = 0;
  int64_t fastfhir_vma_bytes = 0;
};

// Load a pre-generated .ffhr file, parse it via FastFHIR::Parser, and return
// a SyntheaFixture with owned string copies of all extracted fields.
// Throws if the .ffhr file is missing or does not contain a valid Patient.
SyntheaFixture make_synthea_fixture(const std::filesystem::path& ffhr_path);

// PatientData contains unique_ptr members (move-only). Clone only the subset
// of fields that benchmark arms actually read.
inline SyntheaFixture clone_fixture(const SyntheaFixture& src) {
  SyntheaFixture dst{};
  dst.patient_id_storage        = src.patient_id_storage;
  dst.patient_birthdate_storage = src.patient_birthdate_storage;
  dst.patient.id                = dst.patient_id_storage;
  dst.patient.birthdate         = dst.patient_birthdate_storage;
  dst.patient.gender            = src.patient.gender;
  dst.patient.active            = src.patient.active;
  dst.ffhr_size_bytes           = src.ffhr_size_bytes;
  return dst;
}

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_json_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture);

}  // namespace bench
