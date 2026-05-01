#include "harness.hpp"

#include <iostream>

int main() {
  const auto patient = bench::make_single_patient_fixture();
  const auto fastfhir = bench::run_fastfhir_smoke(patient);
  const auto json = bench::run_json_fhir_smoke(patient);

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

  if (fastfhir.queried_value != "1990-03-21" || json.queried_value != "1990-03-21") {
    std::cerr << "timing conformance failed: queried value did not match fixture birthDate (expected 1990-03-21)\n";
    return 1;
  }

  if (fastfhir.queried_value != json.queried_value) {
    std::cerr << "timing conformance failed: arm query results diverged\n";
    return 1;
  }

  std::cout << "timing conformance passed\n";
  return 0;
}
