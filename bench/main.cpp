#include "harness.hpp"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv, argv + argc);
  const bool smoke = std::find(args.begin(), args.end(), "--smoke") != args.end();

  if (!smoke) {
    std::cout << "bench_harness skeleton ready. Run with --smoke for baseline checks.\n";
    return 0;
  }

  const auto ff = bench::run_fastfhir_smoke();
  const auto jf = bench::run_json_fhir_smoke();
  const auto gf = bench::run_google_fhir_smoke();
  const auto hl = bench::run_hl7v2_smoke();

  bench::print_metric(ff);
  bench::print_metric(jf);
  bench::print_metric(gf);
  bench::print_metric(hl);

  return 0;
}
