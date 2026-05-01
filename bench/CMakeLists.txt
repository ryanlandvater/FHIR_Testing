include(FetchContent)

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(FASTFHIR_ROOT "" CACHE PATH "Path to an external FastFHIR checkout with submodules")
if(NOT FASTFHIR_ROOT)
  message(FATAL_ERROR "FASTFHIR_ROOT must be set to a valid FastFHIR checkout directory (e.g., .external/FastFHIR) with include/FastFHIR.hpp present")
endif()
if(NOT EXISTS "${FASTFHIR_ROOT}/include/FastFHIR.hpp")
  message(FATAL_ERROR "FASTFHIR_ROOT=${FASTFHIR_ROOT} does not contain include/FastFHIR.hpp. Clone FastFHIR with submodules and provide the path.")
endif()

# Consume FastFHIR from an installed prefix to avoid referencing checkout internals.
set(FASTFHIR_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/build" CACHE PATH "FastFHIR install prefix (contains include/ and lib/)")

if(NOT EXISTS "${FASTFHIR_INSTALL_PREFIX}/include/FastFHIR.hpp")
  message(FATAL_ERROR "FastFHIR public headers not found at ${FASTFHIR_INSTALL_PREFIX}/include. Run generate_repo.sh to build and install FastFHIR.")
endif()

find_library(FASTFHIR_LIBRARY
  NAMES fastfhir
  PATHS "${FASTFHIR_INSTALL_PREFIX}/lib"
  NO_DEFAULT_PATH
  REQUIRED
)

add_library(bench_core INTERFACE)
target_include_directories(bench_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(bench_core INTERFACE ${FASTFHIR_INSTALL_PREFIX}/include)
target_link_libraries(bench_core INTERFACE nlohmann_json::nlohmann_json ${FASTFHIR_LIBRARY})

add_executable(bench_harness
  main.cpp
  arm_fastfhir.cpp
  arm_json_fhir.cpp
  arm_fastfhir_synthea.cpp
  arm_json_synthea.cpp
  synthea_fixture.cpp
)
target_link_libraries(bench_harness PRIVATE bench_core)

add_executable(bench_timing_conformance
  timing_conformance_test.cpp
  arm_fastfhir.cpp
  arm_json_fhir.cpp
)
target_link_libraries(bench_timing_conformance PRIVATE bench_core)

add_executable(bench_schema_validation schema_validation_test.cpp)
target_link_libraries(bench_schema_validation PRIVATE bench_core)

# Ensure runtime linker can locate libfastfhir from the install prefix.
set_target_properties(bench_harness PROPERTIES BUILD_RPATH "${FASTFHIR_INSTALL_PREFIX}/lib")
set_target_properties(bench_timing_conformance PROPERTIES BUILD_RPATH "${FASTFHIR_INSTALL_PREFIX}/lib")
set_target_properties(bench_schema_validation PROPERTIES BUILD_RPATH "${FASTFHIR_INSTALL_PREFIX}/lib")
