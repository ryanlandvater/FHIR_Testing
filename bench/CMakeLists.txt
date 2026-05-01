add_library(bench_core INTERFACE)
target_include_directories(bench_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_harness
  main.cpp
  arm_fastfhir.cpp
  arm_json_fhir.cpp
)
target_link_libraries(bench_harness PRIVATE bench_core)

add_executable(bench_timing_conformance
  timing_conformance_test.cpp
  arm_fastfhir.cpp
)
target_link_libraries(bench_timing_conformance PRIVATE bench_core)

add_executable(bench_schema_validation schema_validation_test.cpp)
target_link_libraries(bench_schema_validation PRIVATE bench_core)
