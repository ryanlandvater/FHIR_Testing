#pragma once

#include "harness.hpp"

#include <string_view>

namespace bench::test_2 {

inline MetricEvent materialize_placeholder(std::string_view arm) {
  return MetricEvent{std::string(arm), Stage::Test2Materialize, 0};
}

}  // namespace bench::test_2