cmake_minimum_required(VERSION 3.20)
project(FHIRBenchmarkHarness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BENCH_ENABLE_TESTS "Enable benchmark smoke and conformance tests" ON)

add_subdirectory(bench)

if(BENCH_ENABLE_TESTS)
  include(CTest)
  enable_testing()
  add_test(NAME smoke_harness COMMAND bench_harness --smoke)
  set_tests_properties(smoke_harness PROPERTIES LABELS "smoke")

  add_test(NAME timing_conformance COMMAND bench_timing_conformance)
  set_tests_properties(timing_conformance PROPERTIES LABELS "timing")

  add_test(NAME schema_placeholder COMMAND bench_schema_validation)
  set_tests_properties(schema_placeholder PROPERTIES LABELS "schema")
endif()
