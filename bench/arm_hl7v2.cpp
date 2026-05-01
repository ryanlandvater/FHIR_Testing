#include "harness.hpp"

namespace bench {

MetricEvent run_hl7v2_smoke() {
  Timer timer;
  timer.start();
  volatile int accumulator = 0;
  for (int i = 0; i < 1300; ++i) {
    accumulator += i;
  }
  (void)accumulator;
  return MetricEvent{"hl7v2", Stage::Stage3Query, timer.stop_us()};
}

}  // namespace bench
