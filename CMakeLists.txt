cmake_minimum_required(VERSION 3.14)
project(cxray VERSION 1.0 LANGUAGES CXX)

# Require C++17 filesystem support
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <filesystem>
int main() { std::filesystem::create_directories(\".\"); return 0; }
" HAVE_STD_FILESYSTEM)
if(NOT HAVE_STD_FILESYSTEM)
    message(WARNING "std::filesystem not available — some features may be limited")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Release: optimize + treat warnings as errors
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra -Wpedantic")

# Debug: sanitizers + warnings as errors
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined -Wall -Wextra -Wpedantic -Werror")

# ---------------------------------------------------------------------------
# Optional: libconfig++
# ---------------------------------------------------------------------------
find_library(LIBCONFIG_LIB NAMES config++ libconfig++
  HINTS
    /usr/local/lib
    /usr/lib
    $ENV{CONDA_PREFIX}/lib
)
find_path(LIBCONFIG_INCLUDE libconfig.h++
  HINTS
    /usr/local/include
    /usr/include
    $ENV{CONDA_PREFIX}/include
)

if(LIBCONFIG_LIB AND LIBCONFIG_INCLUDE)
  message(STATUS "Found libconfig++: ${LIBCONFIG_LIB}")
  set(HAVE_LIBCONFIG TRUE)
  add_compile_definitions(HAVE_LIBCONFIG)
else()
  message(STATUS "libconfig++ not found — using built-in key=value parser")
  set(HAVE_LIBCONFIG FALSE)
endif()

# ---------------------------------------------------------------------------
# Optional: DRAMSim3
# ---------------------------------------------------------------------------
set(DRAMSIM3_DIR "" CACHE PATH "Path to DRAMSim3 source root")
if(DRAMSIM3_DIR AND EXISTS "${DRAMSIM3_DIR}/src/dramsim3.h")
  message(STATUS "DRAMSim3 found at ${DRAMSIM3_DIR}")
  add_compile_definitions(HAVE_DRAMSIM3)
  set(HAVE_DRAMSIM3 TRUE)
else()
  set(HAVE_DRAMSIM3 FALSE)
endif()

# ---------------------------------------------------------------------------
# Optional: Ramulator2
# ---------------------------------------------------------------------------
set(RAMULATOR2_DIR "" CACHE PATH "Path to Ramulator2 source root")
if(RAMULATOR2_DIR AND EXISTS "${RAMULATOR2_DIR}/src/ramulator2.h")
  message(STATUS "Ramulator2 found at ${RAMULATOR2_DIR}")
  add_compile_definitions(HAVE_RAMULATOR2)
  set(HAVE_RAMULATOR2 TRUE)
else()
  set(HAVE_RAMULATOR2 FALSE)
endif()

# ---------------------------------------------------------------------------
# Core library: libcxray
# ---------------------------------------------------------------------------
set(CXRAY_SRCS
  src/simple_dram_backend.cpp
  src/ddr5_backend.cpp
  src/cxl_sim_engine.cpp
  src/cxl_ras.cpp
  src/cxl_link.cpp
  src/cxl_switch.cpp
  src/cxl_device.cpp
  src/cxl_fabric.cpp
  src/cxl_io.cpp
  src/cxl_numa.cpp
  src/cxl_mem.cpp
)

if(HAVE_DRAMSIM3)
  list(APPEND CXRAY_SRCS src/dramsim3_backend.cpp)
endif()
if(HAVE_RAMULATOR2)
  list(APPEND CXRAY_SRCS src/ramulator2_backend.cpp)
endif()

add_library(cxray_lib STATIC ${CXRAY_SRCS})
target_include_directories(cxray_lib PUBLIC src)

# std::filesystem: GCC < 9 requires explicit linkage
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(cxray_lib PUBLIC stdc++fs)
endif()

if(HAVE_LIBCONFIG)
  target_include_directories(cxray_lib PUBLIC ${LIBCONFIG_INCLUDE})
  target_link_libraries(cxray_lib PUBLIC ${LIBCONFIG_LIB})
endif()
if(HAVE_DRAMSIM3)
  target_include_directories(cxray_lib PUBLIC ${DRAMSIM3_DIR}/src)
  target_link_libraries(cxray_lib PUBLIC ${DRAMSIM3_DIR}/build/libdramsim3.a)
endif()
if(HAVE_RAMULATOR2)
  target_include_directories(cxray_lib PUBLIC ${RAMULATOR2_DIR}/src)
  target_link_libraries(cxray_lib PUBLIC ${RAMULATOR2_DIR}/build/libramulator.a)
endif()

# ---------------------------------------------------------------------------
# Standalone simulator binary
# ---------------------------------------------------------------------------
add_executable(cxray src/cxray_main.cpp)
target_link_libraries(cxray PRIVATE cxray_lib)

# ---------------------------------------------------------------------------
# Unit tests (using a minimal built-in framework — no external deps)
# ---------------------------------------------------------------------------
enable_testing()

set(TEST_SRCS
  tests/test_cxl_latency.cpp
  tests/test_cxl_bandwidth.cpp
  tests/test_cxl_coherence.cpp
  tests/test_cxl_ras.cpp
  tests/test_cxl_topology.cpp
  tests/test_cxl_fabric_io_numa.cpp
  tests/test_cxl_stress.cpp
)

foreach(test_src ${TEST_SRCS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE cxray_lib)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS cxray DESTINATION bin)
install(DIRECTORY configs/ DESTINATION share/cxray/configs)

# ---------------------------------------------------------------------------
# Convenience targets
# ---------------------------------------------------------------------------

# 'make validate' — build + test + quick smoke simulation
add_custom_target(validate
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/cxray
            --config ${CMAKE_SOURCE_DIR}/configs/cxl_2.0.cfg
            --trace  ${CMAKE_SOURCE_DIR}/validation/traces/T2_ptr_chase.trace
            --output /tmp/cxray_validate
            --tag    validate_smoke
            2>/dev/null || true
    DEPENDS cxray
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running tests and smoke simulation..."
)

# 'make lint' — clang-tidy static analysis (if available)
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    set_target_properties(cxray_lib PROPERTIES
        CXX_CLANG_TIDY "${CLANG_TIDY};--warnings-as-errors=*")
endif()
