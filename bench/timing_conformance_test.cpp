#include "harness.hpp"

#include <iostream>

int main() {
  // Build a minimal single-patient fixture without running the ingestor.
  bench::SyntheaFixture sf{};
  sf.patient_id_storage = "patient-conformance";
  sf.patient_birthdate_storage = "1990-03-21";
  sf.patient.id = sf.patient_id_storage;
  sf.patient.birthdate = sf.patient_birthdate_storage;
  sf.patient.gender = AdministrativeGender::Male;
  sf.patient.active = 1;

  bench::BundleBenchFixture fixture{};
  fixture.patients.push_back(std::move(sf));
  fixture.target_size_bytes = 1024;

  const auto fastfhir = bench::run_fastfhir_bundle(fixture);
  const auto json = bench::run_json_bundle(fixture);

  if (fastfhir.metrics.size() < 2 || json.metrics.size() < 2) {
    std::cerr << "timing conformance failed: expected stage1 + stage3 metrics from both arms\n";
    return 1;
  }

  for (const auto& metric : fastfhir.metrics) {
    if (metric.duration_us <= 0) {
      std::cerr << "timing conformance failed: FastFHIR duration must be positive\n";
      return 1;
    }
  }

  for (const auto& metric : json.metrics) {
    if (metric.duration_us <= 0) {
      std::cerr << "timing conformance failed: JSON duration must be positive\n";
      return 1;
    }
  }

  if (fastfhir.queried_value != json.queried_value) {
    std::cerr << "timing conformance failed: arm query results diverged\n"
              << "  fastfhir: " << fastfhir.queried_value << "\n"
              << "  json:     " << json.queried_value << "\n";
    return 1;
  }

  if (fastfhir.queried_value.find("patients=1") == std::string::npos) {
    std::cerr << "timing conformance failed: expected patients=1 in result, got: "
              << fastfhir.queried_value << "\n";
    return 1;
  }

  std::cout << "timing conformance passed\n";
  return 0;
}
