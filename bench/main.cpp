#include "harness.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv, argv + argc);
  const bool smoke = std::find(args.begin(), args.end(), "--smoke") != args.end();
  int iterations = 1;

  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--iterations" && i + 1 < args.size()) {
      iterations = std::max(1, std::atoi(args[i + 1].data()));
    }
  }

  if (!smoke) {
    std::cout << "bench_harness ready. Run with --smoke to emit single-patient FastFHIR and JSON/FHIR metrics.\n";
    return 0;
  }

  const auto patient = bench::make_single_patient_fixture();

  for (int i = 0; i < iterations; ++i) {
    const auto ff = bench::run_fastfhir_smoke(patient);
    const auto jf = bench::run_json_fhir_smoke(patient);

    for (const auto& metric : ff.metrics) {
      bench::print_metric(metric);
    }
    for (const auto& metric : jf.metrics) {
      bench::print_metric(metric);
    }
  }

  return 0;
}
