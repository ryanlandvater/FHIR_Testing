#pragma once

#include <cctype>
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
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin_).count();
    if (elapsed_ns <= 0) {
      return 1;
    }
    // Round up sub-microsecond samples so recorded metrics are never zero.
    return (elapsed_ns + 999) / 1000;
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

// ---------------------------------------------------------------------------
// Cross-arm result validation
// ---------------------------------------------------------------------------

namespace detail {

// Extract a key=value field from a queried_value string.
inline std::string qv_field(const std::string& qv, const std::string& key) {
  const std::string needle = key + "=";
  const auto pos = qv.find(needle);
  if (pos == std::string::npos) return "";
  const auto start = pos + needle.size();
  const auto end = qv.find(' ', start);
  return end == std::string::npos ? qv.substr(start) : qv.substr(start, end - start);
}

// Reduce a date string to its digit characters for format-agnostic comparison.
inline std::string digits_only(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) if (std::isdigit(c)) out += static_cast<char>(c);
  return out;
}

} // namespace detail

// Compare the query results across all four arms and print a warning to stderr
// for each discrepancy.  Returns true only if every check passes.
//
// Checks performed:
//   1. patients=N   — all four arms must agree
//   2. birthdate    — normalized to digits; all four arms must agree
//   3. cholesterol  — the three FHIR arms (fastfhir / json / google) must agree
//                     (hl7v2 does not encode Observation resources)
//   4. hl7v2 parity — numerator must equal denominator (all ZPV snapshots matched)
inline bool validate_results(const ArmRunResult& ff, const ArmRunResult& jf,
                              const ArmRunResult& gf, const ArmRunResult& h2) {
  using detail::qv_field;
  using detail::digits_only;
  bool ok = true;

  // 1. Patient count
  const auto ff_pat = qv_field(ff.queried_value, "patients");
  const auto jf_pat = qv_field(jf.queried_value, "patients");
  const auto gf_pat = qv_field(gf.queried_value, "patients");
  const auto h2_pat = qv_field(h2.queried_value, "patients");
  if (ff_pat != jf_pat || ff_pat != gf_pat || ff_pat != h2_pat) {
    std::cerr << "[validate] MISMATCH patients:"
              << " fastfhir=" << ff_pat
              << " json="     << jf_pat
              << " google="   << gf_pat
              << " hl7v2="    << h2_pat << "\n";
    ok = false;
  }

  // 2. Birthdate (spot-check first patient; format-normalised)
  const auto ff_bd = digits_only(qv_field(ff.queried_value, "birthdate"));
  const auto jf_bd = digits_only(qv_field(jf.queried_value, "birthdate"));
  const auto gf_bd = digits_only(qv_field(gf.queried_value, "birthdate"));
  const auto h2_bd = digits_only(qv_field(h2.queried_value, "birthdate"));
  if (!ff_bd.empty() && (ff_bd != jf_bd || ff_bd != gf_bd || ff_bd != h2_bd)) {
    std::cerr << "[validate] MISMATCH birthdate:"
              << " fastfhir=" << ff_bd
              << " json="     << jf_bd
              << " google="   << gf_bd
              << " hl7v2="    << h2_bd << "\n";
    ok = false;
  }

  // 3. Cholesterol — FHIR arms only
  const auto ff_cho = qv_field(ff.queried_value, "cholesterol");
  const auto jf_cho = qv_field(jf.queried_value, "cholesterol");
  const auto gf_cho = qv_field(gf.queried_value, "cholesterol");
  if (ff_cho != jf_cho || ff_cho != gf_cho) {
    std::cerr << "[validate] MISMATCH cholesterol (FHIR arms):"
              << " fastfhir=" << ff_cho
              << " json="     << jf_cho
              << " google="   << gf_cho << "\n";
    ok = false;
  }

  // 4. HL7v2 ZPV parity — numerator must equal denominator
  const auto parity = qv_field(h2.queried_value, "parity");
  if (!parity.empty()) {
    const auto slash = parity.find('/');
    if (slash != std::string::npos &&
        parity.substr(0, slash) != parity.substr(slash + 1)) {
      std::cerr << "[validate] hl7v2 ZPV parity mismatch: parity=" << parity << "\n";
      ok = false;
    }
  }

  return ok;
}

}  // namespace bench
