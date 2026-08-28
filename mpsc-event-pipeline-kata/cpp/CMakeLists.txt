cmake_minimum_required(VERSION 3.20)
project(weir_event_pipeline CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # so clangd sees the right flags

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Executables land in <project>/bin, not buried in the build tree, so the thing
# you run is in a predictable place whichever build directory produced it.
set(WEIR_BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${WEIR_BIN_DIR})
# Multi-config generators (Visual Studio) otherwise append bin/Release,
# bin/Debug, ... — pin every config to the same directory.
foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER ${cfg} cfg_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} ${WEIR_BIN_DIR})
endforeach()

find_package(Threads REQUIRED)

# Header-only: the pipeline is templated on its Sink, so there is nothing to
# compile separately.
add_library(weir_pipeline INTERFACE)
target_include_directories(weir_pipeline INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(weir_pipeline INTERFACE Threads::Threads)

add_executable(solution
  bench/main.cpp
  bench/harness.cpp
  bench/correctness.cpp
  bench/throughput.cpp
  bench/backpressure.cpp
  bench/scan_ab.cpp)
target_link_libraries(solution PRIVATE weir_pipeline)
target_include_directories(solution PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench)

# Same reasoning as the sanitizer OUTPUT_NAMEs below: every build type writes to
# one shared bin/, so an unsuffixed Debug build would overwrite the Release
# binary and you would benchmark -O0 without noticing. Debugging is the only
# reason to build this target Debug, so the suffix marks what it is for.
set_target_properties(solution PROPERTIES DEBUG_POSTFIX _debug)

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(solution PRIVATE /W4)
else()
  target_compile_options(solution PRIVATE -Wall -Wextra)
endif()

# Optional: -DWEIR_ASAN=ON / -DWEIR_TSAN=ON.
#
# TSan is unavailable on x86_64-pc-windows-msvc — clang rejects the flag
# outright on that target. It is available under WSL with gcc, which is where
# this target is meant to be built; see solution.md section 10.
option(WEIR_ASAN "Build with AddressSanitizer" OFF)
option(WEIR_TSAN "Build with ThreadSanitizer (Linux/WSL only)" OFF)
if(WEIR_ASAN)
  target_compile_options(solution PRIVATE -fsanitize=address -g)
  target_link_options(solution PRIVATE -fsanitize=address)
  # Distinct name: the sanitized and plain builds use separate build
  # directories but share one bin/, and silently overwriting each other there
  # is how you end up benchmarking an ASan binary.
  set_target_properties(solution PROPERTIES OUTPUT_NAME solution_asan)
  # ASan on Windows does not support the debug CRT. Mixing them aborts during
  # CRT teardown with a bad-free whose stack contains no frames from this
  # project at all — a confusing failure that looks like a real lifetime bug
  # and is not. Since CMake defaults single-config generators to Debug here,
  # pin the release runtime for this target regardless of build type.
  set_property(TARGET solution PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

if(WEIR_TSAN)
  # The check solution.md section 10 asks for: "Before shipping this, the check
  # to add is a Linux TSan run, and it is worth the port."
  #
  # -O1 rather than -O2: TSan needs frame pointers and inlinable-but-not-inlined
  # frames to produce a usable stack, and the race being looked for is a
  # protocol error, not something -O2 would expose differently.
  target_compile_options(solution PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g -O1)
  target_link_options(solution PRIVATE -fsanitize=thread)
  set_target_properties(solution PROPERTIES OUTPUT_NAME solution_tsan)
endif()
