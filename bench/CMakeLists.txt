include(FetchContent)

option(BENCH_ASSIGN_FORCE_FULL_CAST "Force full parsed PatientData cast in stage3 for crash forensics" OFF)

find_package(TBB QUIET CONFIG)
if(TBB_FOUND)
  message(STATUS "TBB found via package config")
  set(BENCH_TBB_TARGET TBB::tbb)
else()
  find_library(TBB_LIBRARY NAMES tbb)
  if(TBB_LIBRARY)
    add_library(bench_tbb UNKNOWN IMPORTED)
    set_target_properties(bench_tbb PROPERTIES IMPORTED_LOCATION "${TBB_LIBRARY}")
    set(BENCH_TBB_TARGET bench_tbb)
    message(STATUS "TBB found via find_library: ${TBB_LIBRARY}")
  else()
    message(WARNING "TBB not found. Parallel STL backend may fall back to implementation defaults.")
  endif()
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

message(STATUS "Benchmark configured for FFHR + JSON + HL7v2 arms.")

# Optional: PostgreSQL libpq for database result writing
find_package(PostgreSQL QUIET)
set(BENCH_LIBPQ_FOUND OFF)
if(PostgreSQL_FOUND)
  add_compile_definitions(HAVE_LIBPQ)
  set(BENCH_LIBPQ_FOUND ON)
  message(STATUS "PostgreSQL found: ${PostgreSQL_LIBRARIES}")
else()
  find_library(BENCH_LIBPQ_LIBRARY
    NAMES pq libpq
    PATHS /opt/homebrew/lib /usr/local/lib /opt/anaconda3/lib)
  find_path(BENCH_LIBPQ_INCLUDE_DIR
    NAMES libpq-fe.h
    PATHS /opt/homebrew/include /usr/local/include /opt/anaconda3/include)
  if(BENCH_LIBPQ_LIBRARY AND BENCH_LIBPQ_INCLUDE_DIR)
    set(BENCH_LIBPQ_FOUND ON)
    add_compile_definitions(HAVE_LIBPQ)
    message(STATUS "PostgreSQL found via fallback: ${BENCH_LIBPQ_LIBRARY}")
  else()
    message(STATUS "PostgreSQL not found - database writing disabled")
  endif()
endif()

set(FASTFHIR_ROOT "" CACHE PATH "Path to an external FastFHIR checkout with submodules")
if(NOT FASTFHIR_ROOT)
  message(FATAL_ERROR "FASTFHIR_ROOT must be set to a valid FastFHIR checkout directory (e.g., .external/FastFHIR) with include/FastFHIR.hpp present")
endif()
if(NOT EXISTS "${FASTFHIR_ROOT}/include/FastFHIR.hpp")
  message(FATAL_ERROR "FASTFHIR_ROOT=${FASTFHIR_ROOT} does not contain include/FastFHIR.hpp. Clone FastFHIR with submodules and provide the path.")
endif()

# Consume FastFHIR from an installed prefix to avoid referencing checkout internals.
set(FASTFHIR_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/local" CACHE PATH "FastFHIR install prefix (contains include/ and lib/)")
set(FASTFHIR_BUILD_DIR "${CMAKE_SOURCE_DIR}/.external/FastFHIR-build" CACHE PATH "FastFHIR build directory (for generated headers and bundled simdjson headers)")

if(NOT EXISTS "${FASTFHIR_INSTALL_PREFIX}/include/FastFHIR.hpp")
  message(FATAL_ERROR "FastFHIR public headers not found at ${FASTFHIR_INSTALL_PREFIX}/include. Run ./generate_repo.sh to build and install FastFHIR.")
endif()

set(FASTFHIR_INCLUDE_DIR "${FASTFHIR_ROOT}/include")
set(FASTFHIR_GENERATED_INCLUDE_DIR "${FASTFHIR_ROOT}/generated_src")
set(FASTFHIR_SIMDJSON_INCLUDE_DIR "${FASTFHIR_BUILD_DIR}/_deps/simdjson-src/include")

if(NOT EXISTS "${FASTFHIR_INCLUDE_DIR}/FastFHIR.hpp")
  message(FATAL_ERROR "FastFHIR source headers not found at ${FASTFHIR_INCLUDE_DIR}. Ensure FASTFHIR_ROOT points to the active FastFHIR checkout.")
endif()
if(NOT EXISTS "${FASTFHIR_GENERATED_INCLUDE_DIR}")
  message(FATAL_ERROR "FastFHIR generated headers not found at ${FASTFHIR_GENERATED_INCLUDE_DIR}. Build FastFHIR first so generated_src is available.")
endif()
if(NOT EXISTS "${FASTFHIR_SIMDJSON_INCLUDE_DIR}/simdjson.h")
  message(FATAL_ERROR "Bundled simdjson headers not found at ${FASTFHIR_SIMDJSON_INCLUDE_DIR}. Ensure FASTFHIR_BUILD_DIR points to the FastFHIR build tree.")
endif()

find_library(FASTFHIR_LIBRARY
  NAMES fastfhir
  PATHS "${FASTFHIR_INSTALL_PREFIX}/lib"
  NO_DEFAULT_PATH
  REQUIRED
)

find_library(FASTFHIR_INGESTOR_LIBRARY
  NAMES fastfhir_ingestor
  PATHS "${FASTFHIR_INSTALL_PREFIX}/lib"
  NO_DEFAULT_PATH
  REQUIRED
)

add_library(bench_core INTERFACE)
target_include_directories(bench_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(bench_core INTERFACE ${FASTFHIR_INCLUDE_DIR})
target_include_directories(bench_core INTERFACE ${FASTFHIR_GENERATED_INCLUDE_DIR})
target_include_directories(bench_core INTERFACE ${FASTFHIR_SIMDJSON_INCLUDE_DIR})
target_link_libraries(bench_core INTERFACE nlohmann_json::nlohmann_json ${FASTFHIR_LIBRARY} ${FASTFHIR_INGESTOR_LIBRARY})

if(BENCH_TBB_TARGET)
  target_link_libraries(bench_core INTERFACE ${BENCH_TBB_TARGET})
endif()

if(PostgreSQL_FOUND)
  target_include_directories(bench_core INTERFACE ${PostgreSQL_INCLUDE_DIRS})
  target_link_libraries(bench_core INTERFACE ${PostgreSQL_LIBRARIES})
elseif(BENCH_LIBPQ_FOUND)
  target_include_directories(bench_core INTERFACE ${BENCH_LIBPQ_INCLUDE_DIR})
  target_link_libraries(bench_core INTERFACE ${BENCH_LIBPQ_LIBRARY})
endif()

add_executable(bench_harness
  main.cpp
  arm_fastfhir.cpp
  arm_json_fhir.cpp
  arm_hl7v2.cpp
  synthea_fixture.cpp
)

target_link_libraries(bench_harness PRIVATE bench_core)
if(BENCH_ASSIGN_FORCE_FULL_CAST)
  target_compile_definitions(bench_harness PRIVATE BENCH_ASSIGN_FORCE_FULL_CAST=1)
endif()

add_executable(bench_timing_conformance
  timing_conformance_test.cpp
  arm_fastfhir.cpp
  arm_json_fhir.cpp
)
target_link_libraries(bench_timing_conformance PRIVATE bench_core)
if(BENCH_ASSIGN_FORCE_FULL_CAST)
  target_compile_definitions(bench_timing_conformance PRIVATE BENCH_ASSIGN_FORCE_FULL_CAST=1)
endif()

# Ensure runtime linker can locate libfastfhir from the install prefix.
set_target_properties(bench_harness PROPERTIES BUILD_RPATH "${FASTFHIR_INSTALL_PREFIX}/lib")
set_target_properties(bench_timing_conformance PROPERTIES BUILD_RPATH "${FASTFHIR_INSTALL_PREFIX}/lib")
