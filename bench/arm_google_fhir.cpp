#include "harness.hpp"

namespace bench {

MetricEvent run_google_fhir_smoke() {
  Timer timer;
  timer.start();
  volatile int accumulator = 0;
  for (int i = 0; i < 1200; ++i) {
    accumulator += i;
  }
  (void)accumulator;
  return MetricEvent{"google_fhir_proto", Stage::Stage3Query, timer.stop_us()};
}

}  // namespace bench
