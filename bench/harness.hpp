#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <FastFHIR.hpp>
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
  std::cout << e.arm << "," << to_string(e.stage) << "," << e.duration_us << "\n";
}

inline constexpr std::string_view kPatientQueryField = "birthDate";

inline PatientData make_single_patient_fixture() {
  // Canonical in-memory FHIR R5 PatientData fixture used by all arms.
  PatientData patient{};
  patient.id = "patient-1";
  patient.active = 1;
  patient.gender = AdministrativeGender::Male;
  patient.birthdate = "1990-03-21";

  HumanNameData name{};
  name.family = "Landvater";
  name.given = {"Ryan", "Eric"};
  patient.name.push_back(std::move(name));

  return patient;
}

ArmRunResult run_fastfhir_smoke(const PatientData& patient);
ArmRunResult run_json_fhir_smoke(const PatientData& patient);

}  // namespace bench
