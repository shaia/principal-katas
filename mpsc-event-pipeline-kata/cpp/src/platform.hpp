#pragma once
//
// Platform primitives shared by everything else: cache-line size, the CPU
// spin-wait hint, and the clock aliases.

#include <chrono>
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
// Hints to the core that this is a spin-wait: de-pipelines the loop, cuts power,
// and on SMT yields issue slots to the sibling thread.
#define WEIR_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#define WEIR_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define WEIR_PAUSE() ((void)0)
#endif

namespace weir {

// Not std::hardware_destructive_interference_size: using it in a type's layout
// bakes a compiler constant into the ABI, and clang warns for exactly that
// reason. 64 is correct on every target this is meant to run on.
inline constexpr std::size_t kCacheLine = 64;

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

}  // namespace weir
