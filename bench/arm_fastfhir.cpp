#include "harness.hpp"

namespace bench {

MetricEvent run_fastfhir_smoke() {
  // Stage 3 query timing placeholder; replace with real parser path.
  Timer timer;
  timer.start();
  volatile int accumulator = 0;
  for (int i = 0; i < 1000; ++i) {
    accumulator += i;
  }
  (void)accumulator;
  return MetricEvent{"fastfhir", Stage::Stage3Query, timer.stop_us()};
}

}  // namespace bench
