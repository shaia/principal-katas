#pragma once
//
// Platform primitives shared by everything else: cache-line size, the CPU
// spin-wait hint, the inlining controls the measurement depends on, and the
// clock aliases.

#include <chrono>
#include <cstddef>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
// Hints to the core that this is a spin-wait: de-pipelines the loop, cuts power,
// and on SMT yields issue slots to the sibling thread.
#define SWY_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SWY_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define SWY_PAUSE() ((void)0)
#endif

// Handler bodies must not be inlined into their call sites. The whole subject of
// this kata is what a *call* costs and how much code the callees occupy; a
// compiler that inlines 100 handlers into one dispatch loop has answered a
// different question, and it would answer it differently for each mechanism —
// which is exactly the comparison phase 4 is trying to make fair.
#if defined(_MSC_VER) && !defined(__clang__)
#define SWY_NOINLINE __declspec(noinline)
#else
#define SWY_NOINLINE __attribute__((noinline))
#endif

namespace switchyard {

// Not std::hardware_destructive_interference_size: using it in a type's layout
// bakes a compiler constant into the ABI, and clang warns for exactly that
// reason. 64 is correct on every target this is meant to run on.
inline constexpr std::size_t kCacheLine = 64;

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

// Pin to one core, and make it a fast one.
//
// This is not a nicety on a hybrid CPU. On Raptor Lake the scheduler will move
// a busy thread between performance and efficiency cores mid-run, and the two
// differ enough that the same arm measured twice lands in different columns —
// which shows up as a benchmark whose cost is not monotonic in the handler
// count, and reads as a surprising result rather than as an artifact.
//
// CPU 0 is a P-core on every hybrid part shipped so far. Pinning also stops the
// working set from following the thread to a different L1 and L2.
inline void pin_to_one_core() {
#if defined(_WIN32)
    ::SetThreadAffinityMask(::GetCurrentThread(), 1ull);
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
#endif
}

}  // namespace switchyard
