#include "harness.hpp"

namespace bench {

MetricEvent run_json_fhir_smoke() {
  Timer timer;
  timer.start();
  volatile int accumulator = 0;
  for (int i = 0; i < 1100; ++i) {
    accumulator += i;
  }
  (void)accumulator;
  return MetricEvent{"json_fhir", Stage::Stage3Query, timer.stop_us()};
}

}  // namespace bench
