cmake_minimum_required(VERSION 3.20)
project(FHIRBenchmarkHarness LANGUAGES C CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(bench)

include(CTest)
enable_testing()
add_test(NAME timing_conformance COMMAND bench_timing_conformance)
set_tests_properties(timing_conformance PROPERTIES LABELS "conformance")
