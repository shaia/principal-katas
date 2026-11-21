cmake_minimum_required(VERSION 3.20)

# CMake's Windows-Clang platform module enables the resource compiler, and with
# the GNU-style clang++ driver it does not look for llvm-rc beside the compiler.
if(WIN32 AND CMAKE_CXX_COMPILER AND NOT CMAKE_RC_COMPILER)
  get_filename_component(_keystone_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(CMAKE_RC_COMPILER NAMES llvm-rc HINTS "${_keystone_cxx_dir}")
endif()

project(keystone_dag CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # so clangd sees the right flags

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Executables land in <project>/bin, not buried in the build tree.
set(KEYSTONE_BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${KEYSTONE_BIN_DIR})
foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER ${cfg} cfg_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} ${KEYSTONE_BIN_DIR})
endforeach()

find_package(Threads REQUIRED)

# Header-only: the scheduler, its deque, the graph and the event count.
add_library(keystone INTERFACE)
target_include_directories(keystone INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(keystone INTERFACE Threads::Threads)
if(WIN32)
  # WaitOnAddress / WakeByAddress*, and the 1 ms timer resolution the open-loop
  # submitter sleeps on.
  target_link_libraries(keystone INTERFACE synchronization winmm)
endif()

add_executable(solution
  bench/main.cpp
  bench/harness.cpp
  bench/driver.cpp
  bench/instrument.cpp
  bench/brief.cpp
  bench/invariants.cpp
  bench/granularity.cpp
  bench/critical_path.cpp
  bench/failure.cpp
  bench/scaling.cpp
  bench/bugs.cpp)
target_link_libraries(solution PRIVATE keystone)
target_include_directories(solution PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench)

# Every build type writes to one shared bin/, so an unsuffixed Debug build would
# overwrite the Release binary and you would benchmark -O0 without noticing.
set_target_properties(solution PROPERTIES DEBUG_POSTFIX _debug)

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(solution PRIVATE /W4)
else()
  target_compile_options(solution PRIVATE -Wall -Wextra)
endif()

# Optional: -DKEYSTONE_ASAN=ON / -DKEYSTONE_TSAN=ON.
#
# TSan is unavailable on x86_64-pc-windows-msvc; build it under WSL with gcc, and
# run it on the correctness phase and the deliberate bugs:
#   ./bin/solution_tsan --phase 2   and   ./bin/solution_tsan --bug <name>
option(KEYSTONE_ASAN "Build with AddressSanitizer" OFF)
option(KEYSTONE_TSAN "Build with ThreadSanitizer (Linux/WSL only)" OFF)
if(KEYSTONE_ASAN)
  target_compile_options(solution PRIVATE -fsanitize=address -g)
  target_link_options(solution PRIVATE -fsanitize=address)
  set_target_properties(solution PROPERTIES OUTPUT_NAME solution_asan)
  # ASan cannot be mixed with the debug CRT; pin the release runtime.
  set_property(TARGET solution PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
if(KEYSTONE_TSAN)
  target_compile_options(solution PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g -O1)
  # gcc warns at every instantiation that "atomic_thread_fence is not supported
  # with -fsanitize=thread". That warning is a finding, not noise: it is why the
  # release+fence decrement, which is correct, is reported as a race
  # (solution.md §8). Said once here rather than at every instantiation.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(solution PRIVATE -Wno-tsan)
  endif()
  target_link_options(solution PRIVATE -fsanitize=thread)
  target_compile_definitions(solution PRIVATE KEYSTONE_TSAN=1)
  set_target_properties(solution PROPERTIES OUTPUT_NAME solution_tsan)
endif()
