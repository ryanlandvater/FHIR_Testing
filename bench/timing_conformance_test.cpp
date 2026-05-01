#include "harness.hpp"

#include <iostream>

int main() {
  bench::BundlePatient bp{};
  bp.memory = FastFHIR::Memory::create(4096);
  FastFHIR::Builder builder(bp.memory, FHIR_VERSION_R5);

  PatientData patient{};
  patient.id = "patient-conformance";
  patient.birthdate = "1990-03-21";
  patient.gender = AdministrativeGender::Male;
  patient.active = 1;

  auto patient_handle = builder.append_obj(patient);
  builder.set_root(patient_handle);
  (void)builder.finalize();

  bp.patient.id = patient.id;
  bp.patient.birthdate = patient.birthdate;
  bp.patient.gender = patient.gender;
  bp.patient.active = patient.active;
  bp.patient.gender = AdministrativeGender::Male;
  bp.patient.active = 1;

  bench::BundleBenchFixture fixture{};
  fixture.bundle.push_back(std::move(bp));
  fixture.target_size_bytes = 1024;
  fixture.actual_ingested_bytes = 1024;

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
