#pragma once
//
// Platform primitives shared by everything else: cache-line size, the CPU
// spin-wait hint, the clocks, thread placement, and the one OS facility the
// standard library does not offer — a wait on an address that can time out.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
// Hints to the core that this is a spin-wait: de-pipelines the loop, cuts power,
// and on SMT yields issue slots to the sibling thread. Its latency is not a
// constant — phase 0 measures it per core type, because every spin budget in
// this project is counted in these.
#define TURNSTILE_PAUSE() _mm_pause()
#define TURNSTILE_HAVE_TSC 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define TURNSTILE_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define TURNSTILE_PAUSE() ((void)0)
#endif

namespace turnstile {

// Not std::hardware_destructive_interference_size: using it in a type's layout
// bakes a compiler constant into the ABI, and clang warns for exactly that
// reason. 64 is correct on every target this is meant to run on.
inline constexpr std::size_t kCacheLine = 64;

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

// The TSC, calibrated against steady_clock.
//
// steady_clock on Windows is QueryPerformanceCounter at 10 MHz: a 100 ns tick,
// coarser than the median push this benchmark exists to measure. The TSC ticks
// at a constant rate whatever the core's frequency, and on this CPU it is
// synchronized across cores — phase 0 checks that rather than assuming it,
// because every cross-thread latency here subtracts one core's reading from
// another's.
//
// Calibration produces a value, not a global: the hot paths record raw ticks,
// and whoever reports them owns the conversion.
class TscClock {
public:
    static std::uint64_t now() noexcept {
#if defined(TURNSTILE_HAVE_TSC)
        return __rdtsc();
#else
        return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
    }

    static TscClock calibrate(std::chrono::milliseconds over = std::chrono::milliseconds(200)) {
        const auto     c0 = Clock::now();
        const auto     t0 = now();
        Clock::time_point c1;
        do { c1 = Clock::now(); } while (c1 - c0 < over);
        const auto t1 = now();
        const double ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
        return TscClock(ns / static_cast<double>(t1 - t0));
    }

    double        ns_per_tick() const noexcept { return ns_per_tick_; }
    double        to_ns(std::uint64_t ticks) const noexcept { return static_cast<double>(ticks) * ns_per_tick_; }
    double        to_ns(double ticks) const noexcept { return ticks * ns_per_tick_; }
    std::uint64_t ticks(double ns) const noexcept { return static_cast<std::uint64_t>(ns / ns_per_tick_); }

private:
    explicit TscClock(double ns_per_tick) : ns_per_tick_(ns_per_tick) {}
    double ns_per_tick_;
};

// Busy work that is not a spin-wait: a stand-in for a consumer evaluating rules
// against an event. No PAUSE, because this is meant to occupy the core.
inline void burn(std::uint64_t ticks) noexcept {
    const std::uint64_t until = TscClock::now() + ticks;
    while (TscClock::now() < until) {}
}

// ------------------------------------------------------------------ waiting

// Block while `word == expected`, for at most `timeout`. Returns on a wake, on a
// timeout, spuriously, or at once if the value has already changed; the caller
// re-checks in every case.
//
// C++20's atomic::wait has no timeout, and every blocking path in the queue
// needs one — a deadline on a blocking push, and a backstop on a consumer's
// park — so this goes to the OS primitive the standard library would have used.
inline void wait_on(const std::atomic<std::uint32_t>& word, std::uint32_t expected, Nanos timeout) noexcept {
    if (timeout <= Nanos::zero()) return;
#if defined(_WIN32)
    // Milliseconds, rounded up: WaitOnAddress cannot express less.
    const auto ms = (timeout.count() + 999'999) / 1'000'000;
    ::WaitOnAddress(const_cast<std::atomic<std::uint32_t>*>(&word), &expected, sizeof(expected),
                    static_cast<DWORD>(ms > 0x7fffffff ? INFINITE : ms));
#elif defined(__linux__)
    timespec ts{static_cast<time_t>(timeout.count() / 1'000'000'000),
                static_cast<long>(timeout.count() % 1'000'000'000)};
    ::syscall(SYS_futex, reinterpret_cast<const std::uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, &ts,
              nullptr, 0);
#else
    (void)word, (void)expected;
    std::this_thread::sleep_for(timeout);
#endif
}

inline void wake_one(std::atomic<std::uint32_t>& word) noexcept {
#if defined(_WIN32)
    ::WakeByAddressSingle(&word);
#elif defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#else
    (void)word;
#endif
}

inline void wake_all(std::atomic<std::uint32_t>& word) noexcept {
#if defined(_WIN32)
    ::WakeByAddressAll(&word);
#elif defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr,
              0);
#else
    (void)word;
#endif
}

// ---------------------------------------------------------------- placement

inline int logical_cpus() noexcept { return static_cast<int>(std::thread::hardware_concurrency()); }

// Efficiency class per logical CPU, higher meaning faster, or empty where the OS
// does not say. On a hybrid part this is the P-core / E-core split; WSL exposes
// the CPUs but not their kind.
inline std::vector<int> cpu_efficiency_classes() {
    std::vector<int> out;
#if defined(_WIN32)
    DWORD len = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<unsigned char> buf(len);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) return out;
    out.assign(static_cast<std::size_t>(logical_cpus()), -1);
    for (DWORD off = 0; off < len;) {
        auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        const KAFFINITY mask = p->Processor.GroupMask[0].Mask;
        for (int cpu = 0; cpu < 64; ++cpu)
            if ((mask >> cpu) & 1 && cpu < static_cast<int>(out.size()))
                out[static_cast<std::size_t>(cpu)] = p->Processor.EfficiencyClass;
        off += p->Size;
    }
#endif
    return out;
}

inline void pin_to_cpu(int cpu) noexcept {
#if defined(_WIN32)
    ::SetThreadAffinityMask(::GetCurrentThread(), DWORD_PTR{1} << cpu);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

// Windows' execution-speed power throttling (EcoQoS), forced on or off for this
// process. Left to itself, Windows 11 may treat a process whose window is not in
// the foreground as background work and run its threads at efficiency settings —
// steered toward E-cores, at lower clocks. Pinned single-thread measurements do
// not show it; unpinned multi-threaded runs do, and phase 0 measures by how much.
// main turns it off: a benchmark has to say it wants to be measured at full speed.
inline void set_power_throttling(bool throttled) noexcept {
#if defined(_WIN32)
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask   = throttled ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    ::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#else
    (void)throttled;
#endif
}

inline void unpin() noexcept {
#if defined(_WIN32)
    DWORD_PTR process = 0, system = 0;
    ::GetProcessAffinityMask(::GetCurrentProcess(), &process, &system);
    ::SetThreadAffinityMask(::GetCurrentThread(), process);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < logical_cpus(); ++cpu) CPU_SET(cpu, &set);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
#endif
}

}  // namespace turnstile
