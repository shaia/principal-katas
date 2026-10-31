cmake_minimum_required(VERSION 3.20)
project(turnstile_mpmc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # so clangd sees the right flags

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Executables land in <project>/bin, not buried in the build tree, so the thing
# you run is in a predictable place whichever build directory produced it.
set(TURNSTILE_BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${TURNSTILE_BIN_DIR})
foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER ${cfg} cfg_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} ${TURNSTILE_BIN_DIR})
endforeach()

find_package(Threads REQUIRED)

# Header-only: every queue is a template over its slot type and options, so the
# arms the benchmark compares are instantiated side by side in one binary.
add_library(turnstile INTERFACE)
target_include_directories(turnstile INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(turnstile INTERFACE Threads::Threads)
if(WIN32)
  # WaitOnAddress / WakeByAddress*, and the 1 ms timer resolution the paced
  # producers sleep on.
  target_link_libraries(turnstile INTERFACE synchronization winmm)
endif()

add_executable(solution
  bench/main.cpp
  bench/harness.cpp
  bench/instrument.cpp
  bench/brief.cpp
  bench/invariants.cpp
  bench/throughput.cpp
  bench/backpressure.cpp
  bench/wakeup.cpp
  bench/preemption.cpp
  bench/contention.cpp
  bench/shutdown.cpp
  bench/outline.cpp
  bench/bugs.cpp)
target_link_libraries(solution PRIVATE turnstile)
target_include_directories(solution PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench)

# Same reasoning as the other katas': every build type writes to one shared
# bin/, so an unsuffixed Debug build would overwrite the Release binary and you
# would benchmark -O0 without noticing.
set_target_properties(solution PROPERTIES DEBUG_POSTFIX _debug)

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(solution PRIVATE /W4)
else()
  target_compile_options(solution PRIVATE -Wall -Wextra)
endif()

# Optional: -DTURNSTILE_ASAN=ON / -DTURNSTILE_TSAN=ON.
#
# TSan is unavailable on x86_64-pc-windows-msvc; build it under WSL with gcc.
# The TSan binary skips phase 1 — the brief's queue is a data race by design, and
# reporting it is not news — and is meant to be run on the correctness phases and
# the deliberate bugs:  ./solution_tsan --phase 2,8   and   ./solution_tsan --bug <name>
option(TURNSTILE_ASAN "Build with AddressSanitizer" OFF)
option(TURNSTILE_TSAN "Build with ThreadSanitizer (Linux/WSL only)" OFF)
if(TURNSTILE_ASAN)
  target_compile_options(solution PRIVATE -fsanitize=address -g)
  target_link_options(solution PRIVATE -fsanitize=address)
  set_target_properties(solution PROPERTIES OUTPUT_NAME solution_asan)
  # ASan cannot be mixed with the debug CRT: such a binary aborts during CRT
  # teardown with a bad-free whose stack has no frames from this project, which
  # looks like a lifetime bug and is not. CMake defaults single-config generators
  # to Debug here, so pin the release runtime for this target.
  set_property(TARGET solution PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
if(TURNSTILE_TSAN)
  # -O1: TSan wants frame pointers and frames it can name, and the races being
  # looked for are protocol errors that -O2 would not expose differently.
  target_compile_options(solution PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g -O1)
  # gcc warns, at every one of the 263 instantiations, that "atomic_thread_fence
  # is not supported with -fsanitize=thread". That warning is a finding, not
  # noise: the wakeup protocol depends on exactly that fence, so TSan can neither
  # confirm it nor catch its removal (solution.md §12). Said once here, rather
  # than 263 times per build.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(solution PRIVATE -Wno-tsan)
  endif()
  target_link_options(solution PRIVATE -fsanitize=thread)
  target_compile_definitions(solution PRIVATE TURNSTILE_TSAN=1)
  set_target_properties(solution PROPERTIES OUTPUT_NAME solution_tsan)
endif()
