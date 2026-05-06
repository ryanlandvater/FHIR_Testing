include(FetchContent)
include(CheckLinkerFlag)

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

set(GOOGLE_FHIR_LINK_MODE "DYNAMIC_BUNDLED" CACHE STRING "Google FHIR link strategy: STATIC_FASTBUILD or DYNAMIC_BUNDLED")
set_property(CACHE GOOGLE_FHIR_LINK_MODE PROPERTY STRINGS STATIC_FASTBUILD DYNAMIC_BUNDLED)

set(GOOGLE_FHIR_BAZEL_BIN_FASTBUILD
  "${CMAKE_SOURCE_DIR}/.external/google-fhir-build/output-base/execroot/com_google_fhir/bazel-out/darwin_arm64-fastbuild/bin"
  CACHE PATH "Google FHIR Bazel fastbuild bin path")
set(GOOGLE_FHIR_BAZEL_BIN_EXTERNAL
  "${CMAKE_SOURCE_DIR}/.external/google-fhir-build/output-base/execroot/com_google_fhir/bazel-out/darwin_arm64-opt-exec-ST-a828a81199fe/bin"
  CACHE PATH "Google FHIR Bazel external deps bin path (protobuf/absl)")
set(GOOGLE_FHIR_BUNDLED_DYLIB "" CACHE FILEPATH "Absolute path to libgoogle_fhir_bundled dynamic library artifact")

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

set(GOOGLE_FHIR_OUTPUT_BASE_ROOT
  "${CMAKE_SOURCE_DIR}/.external/google-fhir-build/output-base"
  CACHE PATH "Google FHIR Bazel output-base root")
set(GOOGLE_FHIR_PROTOBUF_HEADERS_DIR
  "${GOOGLE_FHIR_OUTPUT_BASE_ROOT}/external/com_google_protobuf/src"
  CACHE PATH "Google FHIR protobuf headers root")
set(GOOGLE_FHIR_ABSL_HEADERS_DIR
  "${GOOGLE_FHIR_OUTPUT_BASE_ROOT}/external/com_google_absl"
  CACHE PATH "Google FHIR absl headers root")

add_executable(bench_harness
  main.cpp
  arm_fastfhir.cpp
  arm_json_fhir.cpp
  arm_hl7v2.cpp
  arm_google_fhir.cpp
  synthea_fixture.cpp
)

target_include_directories(bench_harness PRIVATE
  ${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}
  ${GOOGLE_FHIR_PROTOBUF_HEADERS_DIR}
  ${GOOGLE_FHIR_ABSL_HEADERS_DIR}
)

target_link_libraries(bench_harness PRIVATE bench_core)
if(GOOGLE_FHIR_LINK_MODE STREQUAL "STATIC_FASTBUILD")
  function(_bench_filter_existing_libs out_var)
    set(_filtered_libs)
    foreach(_lib IN LISTS ARGN)
      if(EXISTS "${_lib}")
        list(APPEND _filtered_libs "${_lib}")
      else()
        message(WARNING "Google FHIR static lib missing, skipping: ${_lib}")
      endif()
    endforeach()
    set(${out_var} "${_filtered_libs}" PARENT_SCOPE)
  endfunction()

  set(GF_FHIR_STATIC_LIBS
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/cc/google/fhir/libjson_format.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/cc/google/fhir/r4/libjson_format.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/cc/google/fhir/json/libfhir_json.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/cc/google/fhir/liberror_reporter.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/cc/google/fhir/libannotations.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/proto/google/fhir/proto/libannotations_proto.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/proto/google/fhir/proto/r4/core/libdatatypes_proto.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/proto/google/fhir/proto/r4/core/libcodes_proto.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/proto/google/fhir/proto/r4/core/libvaluesets_proto.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/proto/google/fhir/proto/r4/core/resources/libbundle_and_contained_resource_proto.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/proto/google/fhir/proto/r4/core/resources/libpatient_proto.a"
    "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/proto/google/fhir/proto/r4/core/resources/libobservation_proto.a"
  )

  set(GF_PROTOBUF_STATIC_LIBS
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libprotobuf.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libprotobuf_lite.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libprotobuf_nowkt.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libarena.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libarena_align.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libgenerated_message_tctable_lite.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libgenerated_message_tctable_gen.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libport.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/libport_def.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/liblite.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/io/libio.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/io/libtokenizer.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/io/libzero_copy_sink.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_protobuf/src/google/protobuf/io/libgzip_stream.a"
  )

  set(GF_ABSL_LOG_STATIC_LIBS
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/libglobals.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/liblog_entry.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/libcheck_op.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/libconditions.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/libformat.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/libglobals.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/liblog_message.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/liblog_sink_set.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/libnullguard.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/log/internal/libproto.a"
  )

  set(GF_ABSL_SUPPORT_STATIC_LIBS
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/libbase.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/libcore_headers.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/libmalloc_internal.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/libraw_logging_internal.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/libspinlock_wait.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/libstrerror.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/libthrow_delegate.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/base/liblog_severity.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/strings/libstrings.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/strings/libstring_view.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/strings/libstr_format_internal.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/synchronization/libsynchronization.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/synchronization/libkernel_timeout_internal.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/synchronization/libgraphcycles_internal.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/time/libtime.a"
    "${GOOGLE_FHIR_BAZEL_BIN_EXTERNAL}/external/com_google_absl/absl/time/libcivil_time.a"
  )

  _bench_filter_existing_libs(GF_FHIR_STATIC_LIBS_EXISTING ${GF_FHIR_STATIC_LIBS})
  _bench_filter_existing_libs(GF_PROTOBUF_STATIC_LIBS_EXISTING ${GF_PROTOBUF_STATIC_LIBS})
  _bench_filter_existing_libs(GF_ABSL_LOG_STATIC_LIBS_EXISTING ${GF_ABSL_LOG_STATIC_LIBS})
  _bench_filter_existing_libs(GF_ABSL_SUPPORT_STATIC_LIBS_EXISTING ${GF_ABSL_SUPPORT_STATIC_LIBS})

  target_link_libraries(bench_harness PRIVATE
    ${GF_FHIR_STATIC_LIBS_EXISTING}
    ${GF_PROTOBUF_STATIC_LIBS_EXISTING}
    ${GF_ABSL_LOG_STATIC_LIBS_EXISTING}
    ${GF_ABSL_SUPPORT_STATIC_LIBS_EXISTING}
    "-framework CoreFoundation"
  )
  check_linker_flag(CXX "-Wl,-no_warn_build_version" BENCH_LD_SUPPORTS_NO_WARN_BUILD_VERSION)
  if(BENCH_LD_SUPPORTS_NO_WARN_BUILD_VERSION)
    target_link_options(bench_harness PRIVATE -Wl,-no_warn_build_version)
  else()
    message(WARNING "Linker does not support -Wl,-no_warn_build_version; continuing without this warning suppression.")
  endif()
elseif(GOOGLE_FHIR_LINK_MODE STREQUAL "DYNAMIC_BUNDLED")
  if(NOT GOOGLE_FHIR_BUNDLED_DYLIB)
    set(_google_fhir_bundled_candidates
      "${CMAKE_SOURCE_DIR}/.external/google-fhir/bazel-bin/cc/google/fhir/libgoogle_fhir_bundled.dylib"
      "${CMAKE_SOURCE_DIR}/.external/google-fhir/bazel-bin/cc/google/fhir/liblibgoogle_fhir_bundled.dylib"
      "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/cc/google/fhir/libgoogle_fhir_bundled.dylib"
      "${GOOGLE_FHIR_BAZEL_BIN_FASTBUILD}/cc/google/fhir/liblibgoogle_fhir_bundled.dylib"
    )
    foreach(_candidate IN LISTS _google_fhir_bundled_candidates)
      if(EXISTS "${_candidate}")
        set(GOOGLE_FHIR_BUNDLED_DYLIB "${_candidate}")
        break()
      endif()
    endforeach()
  endif()

  if(NOT GOOGLE_FHIR_BUNDLED_DYLIB)
    message(FATAL_ERROR "GOOGLE_FHIR_LINK_MODE is DYNAMIC_BUNDLED but GOOGLE_FHIR_BUNDLED_DYLIB was not found. Build //cc/google/fhir:libgoogle_fhir_bundled with Bazel and set GOOGLE_FHIR_BUNDLED_DYLIB.")
  endif()

  add_library(google_fhir_bundled SHARED IMPORTED GLOBAL)
  set_target_properties(google_fhir_bundled PROPERTIES IMPORTED_LOCATION "${GOOGLE_FHIR_BUNDLED_DYLIB}")
  target_link_libraries(bench_harness PRIVATE google_fhir_bundled "-framework CoreFoundation")

  get_filename_component(_google_fhir_bundled_dir "${GOOGLE_FHIR_BUNDLED_DYLIB}" DIRECTORY)
  get_filename_component(_google_fhir_bundled_name "${GOOGLE_FHIR_BUNDLED_DYLIB}" NAME)
  execute_process(
    COMMAND otool -D "${GOOGLE_FHIR_BUNDLED_DYLIB}"
    OUTPUT_VARIABLE _google_fhir_bundled_otool
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  string(REPLACE "\n" ";" _google_fhir_bundled_otool_lines "${_google_fhir_bundled_otool}")
  list(LENGTH _google_fhir_bundled_otool_lines _google_fhir_bundled_otool_len)
  if(_google_fhir_bundled_otool_len GREATER 1)
    list(GET _google_fhir_bundled_otool_lines 1 _google_fhir_bundled_install_name)
  else()
    set(_google_fhir_bundled_install_name "")
  endif()

  add_custom_command(TARGET bench_harness POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${GOOGLE_FHIR_BUNDLED_DYLIB}"
      "$<TARGET_FILE_DIR:bench_harness>/${_google_fhir_bundled_name}"
    VERBATIM
  )

  if(_google_fhir_bundled_install_name)
    add_custom_command(TARGET bench_harness POST_BUILD
      COMMAND install_name_tool -change
        "${_google_fhir_bundled_install_name}"
        "@rpath/${_google_fhir_bundled_name}"
        "$<TARGET_FILE:bench_harness>"
      VERBATIM
    )
  endif()

  set_property(TARGET bench_harness APPEND PROPERTY BUILD_RPATH "${_google_fhir_bundled_dir}")
  set_property(TARGET bench_harness APPEND PROPERTY BUILD_RPATH "@loader_path")
else()
  message(FATAL_ERROR "Unsupported GOOGLE_FHIR_LINK_MODE='${GOOGLE_FHIR_LINK_MODE}'. Expected STATIC_FASTBUILD or DYNAMIC_BUNDLED.")
endif()

if(BENCH_ASSIGN_FORCE_FULL_CAST)
  target_compile_definitions(bench_harness PRIVATE BENCH_ASSIGN_FORCE_FULL_CAST=1)
endif()

add_executable(bench_timing_conformance
  timing_conformance_test.cpp
  arm_fastfhir.cpp
  arm_json_fhir.cpp
  synthea_fixture.cpp
)
target_link_libraries(bench_timing_conformance PRIVATE bench_core)
if(BENCH_ASSIGN_FORCE_FULL_CAST)
  target_compile_definitions(bench_timing_conformance PRIVATE BENCH_ASSIGN_FORCE_FULL_CAST=1)
endif()

# Ensure runtime linker can locate libfastfhir from the install prefix.
set_property(TARGET bench_harness APPEND PROPERTY BUILD_RPATH "${FASTFHIR_INSTALL_PREFIX}/lib")
set_property(TARGET bench_timing_conformance APPEND PROPERTY BUILD_RPATH "${FASTFHIR_INSTALL_PREFIX}/lib")
