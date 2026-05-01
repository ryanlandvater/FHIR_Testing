#include "harness.hpp"

#include <iostream>

int main() {
  // Placeholder conformance check. Expand with static checks or instrumentation
  // that verifies no synchronous SQL/database calls in timed code paths.
  const auto metrics = bench::run_fastfhir_smoke();
  if (metrics.size() < 2) {
    std::cerr << "timing conformance failed: expected stage1 + stage3 metrics\n";
    return 1;
  }

  for (const auto& metric : metrics) {
    if (metric.duration_us <= 0) {
      std::cerr << "timing conformance failed: non-positive duration\n";
      return 1;
    }
  }

  std::cout << "timing conformance placeholder passed\n";
  return 0;
}
