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

// One ingested Synthea patient item kept in RAM.
// The FastFHIR::Memory arena contains the serialized FFHR patient binary.
struct BundlePatient {
  FastFHIR::Memory memory;
  PatientData patient;
};

struct BundleBenchFixture {
  std::vector<BundlePatient> bundle;
  int64_t target_size_bytes = 0;
  int64_t actual_ingested_bytes = 0;
  int64_t fastfhir_vma_bytes = 0;
};

BundlePatient make_bundle_patient_from_json(const std::filesystem::path& json_path);

inline BundlePatient clone_bundle_patient(const BundlePatient& src) {
  BundlePatient dst{};
  dst.memory = src.memory;
  dst.patient.id = src.patient.id;
  dst.patient.birthdate = src.patient.birthdate;
  dst.patient.gender = src.patient.gender;
  dst.patient.active = src.patient.active;
  return dst;
}

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_json_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture);

}  // namespace bench
