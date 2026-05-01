#include "harness.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv, argv + argc);
  const bool smoke = std::find(args.begin(), args.end(), "--smoke") != args.end();
  const bool synthea = std::find(args.begin(), args.end(), "--synthea") != args.end();
  int iterations = 1;

  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--iterations" && i + 1 < args.size()) {
      iterations = std::max(1, std::atoi(args[i + 1].data()));
    }
  }

  if (synthea) {
    const fs::path primary_dir = "datasets/synthea/fhir";
    const fs::path fallback_dir = "datasets/synthea";
    const fs::path synthea_dir = fs::exists(primary_dir) ? primary_dir : fallback_dir;

    if (!fs::exists(synthea_dir)) {
      std::cerr << "Synthea data not found at " << primary_dir << " or " << fallback_dir
                << ". Please run the download script.\n";
      return 1;
    }

    for (const auto& entry : fs::directory_iterator(synthea_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            std::ifstream file(entry.path());
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            
            auto bundle = bench::make_synthea_fixture(content);

            for (int i = 0; i < iterations; ++i) {
                const auto ff = bench::run_fastfhir_synthea_query(bundle);
                const auto jf = bench::run_json_synthea_query(bundle);

                for (const auto& metric : ff.metrics) {
                    bench::print_metric(metric);
                }
                for (const auto& metric : jf.metrics) {
                    bench::print_metric(metric);
                }
            }
        }
    }
    return 0;
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
