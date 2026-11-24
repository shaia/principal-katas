#pragma once
//
// Platform primitives shared by everything else: cache-line size, the CPU
// spin-wait hint, the clocks, thread placement, and the one OS facility the
// standard library does not offer — a wait on an address that can time out.
// (Adapted from the mpmc kata's; this kata stands alone.)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <climits>
#include <cstring>
#include <ctime>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
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
#define KEYSTONE_PAUSE() _mm_pause()
#define KEYSTONE_HAVE_TSC 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define KEYSTONE_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define KEYSTONE_PAUSE() ((void)0)
#endif

namespace keystone {

// Not std::hardware_destructive_interference_size: using it in a type's layout
// bakes a compiler constant into the ABI, and clang warns for exactly that
// reason. 64 is correct on every target this is meant to run on.
inline constexpr std::size_t kCacheLine = 64;

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

// The TSC, calibrated against steady_clock.
//
// steady_clock on Windows is QueryPerformanceCounter at 10 MHz: a 100 ns tick,
// coarser than the scheduling overhead this benchmark exists to measure. The TSC ticks
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
#if defined(KEYSTONE_HAVE_TSC)
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

// Busy work that is not a spin-wait: the body of a task. No PAUSE, because this is meant to occupy the core.
// Returns the clock when it stopped. The body runs to a deadline, not a count, so a thread the OS
// takes off its CPU mid-body returns late: anything past start + ticks is time spent not running.
inline std::uint64_t burn_from(std::uint64_t start, std::uint64_t ticks) noexcept {
    const std::uint64_t until = start + ticks;
    std::uint64_t       t;
    while ((t = TscClock::now()) < until) {}
    return t;
}
inline void burn(std::uint64_t ticks) noexcept { (void)burn_from(TscClock::now(), ticks); }

// ------------------------------------------------------------------ waiting

// Block while `word == expected`, for at most `timeout`. Returns on a wake, on a
// timeout, spuriously, or at once if the value has already changed; the caller
// re-checks in every case.
//
// C++20's atomic::wait has no timeout, and a parked worker needs one as a
// backstop, so this goes to the OS primitive the standard library would have
// used.
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

// CPU time this process has used, every thread, user and kernel, in seconds.
// On Windows from the cycle-time accounting, which reads the TSC at each context
// switch, so `ns_per_tsc_tick` converts it. The user and kernel times
// GetProcessTimes reports are charged a timer tick at a time, too coarse for the
// few milliseconds a second an idle scheduler uses.
inline double process_cpu_seconds(double ns_per_tsc_tick) noexcept {
#if defined(_WIN32)
    ULONG64 cycles = 0;
    if (!::QueryProcessCycleTime(::GetCurrentProcess(), &cycles)) return 0;
    return static_cast<double>(cycles) * ns_per_tsc_tick * 1e-9;
#elif defined(__linux__)
    (void)ns_per_tsc_tick;
    timespec ts{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#else
    (void)ns_per_tsc_tick;
    return 0;
#endif
}

// CPU time the calling thread has used, in seconds, by the same accounting.
inline double thread_cpu_seconds(double ns_per_tsc_tick) noexcept {
#if defined(_WIN32)
    ULONG64 cycles = 0;
    if (!::QueryThreadCycleTime(::GetCurrentThread(), &cycles)) return 0;
    return static_cast<double>(cycles) * ns_per_tsc_tick * 1e-9;
#elif defined(__linux__)
    (void)ns_per_tsc_tick;
    timespec ts{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#else
    (void)ns_per_tsc_tick;
    return 0;
#endif
}

// Busy and total time of each online CPU since boot, in the kernel's clock ticks,
// indexed by CPU number; empty where the OS does not say. Two readings bracket a
// run: a CPU that was mostly idle while every worker spun means two workers
// shared another CPU, which the process's own CPU time cannot tell apart from a
// slow scheduler. Linux only (/proc/stat); idle and iowait count as not busy.
struct CpuTimes {
    std::uint64_t busy = 0, total = 0;
};
inline std::vector<CpuTimes> cpu_times() {
    std::vector<CpuTimes> out;
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return out;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "cpu", 3) != 0) break;  // the per-CPU lines come first
        if (line[3] == ' ') continue;                   // the all-CPU total
        int                cpu  = 0;
        unsigned long long v[8] = {};  // user nice system idle iowait irq softirq steal
        if (std::sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1], &v[2], &v[3], &v[4],
                        &v[5], &v[6], &v[7]) != 9 ||
            cpu < 0)
            continue;
        CpuTimes t;
        for (unsigned long long x : v) t.total += x;
        t.busy = t.total - v[3] - v[4];
        if (static_cast<std::size_t>(cpu) >= out.size()) out.resize(static_cast<std::size_t>(cpu) + 1);
        out[static_cast<std::size_t>(cpu)] = t;
    }
    std::fclose(f);
#endif
    return out;
}

// Times this process's threads were switched out while still runnable, all
// threads, since it started: preempted, or a yield that found another thread to
// run (Linux counts both as involuntary). Linux only; zero elsewhere.
inline std::uint64_t involuntary_switches() noexcept {
#if defined(__linux__)
    rusage u{};
    if (::getrusage(RUSAGE_SELF, &u) != 0) return 0;
    return static_cast<std::uint64_t>(u.ru_nivcsw);
#else
    return 0;
#endif
}

// ---------------------------------------------------------------- placement

inline int logical_cpus() noexcept { return static_cast<int>(std::thread::hardware_concurrency()); }

// Where worker `i` runs when workers are pinned: CPU 0 is left to the thread that
// submits, and the workers take the others in turn, so a pool larger than the
// machine doubles up evenly instead of where the OS happens to put it.
inline int pinned_worker_cpu(int i) noexcept {
    const int n = logical_cpus();
    return n > 1 ? 1 + i % (n - 1) : 0;
}

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

// The NUMA node of each logical CPU, or empty where the OS does not say. On a
// two-socket machine this is the socket, which is what the follow-up's
// cross-socket reasoning needs measured rather than assumed.
inline std::vector<int> cpu_numa_nodes() {
    std::vector<int> out;
    const int        n = logical_cpus();
#if defined(_WIN32)
    out.assign(static_cast<std::size_t>(n), 0);
    for (int cpu = 0; cpu < n && cpu < 64; ++cpu) {
        PROCESSOR_NUMBER p{};
        p.Number    = static_cast<BYTE>(cpu);
        USHORT node = 0;
        if (!::GetNumaProcessorNodeEx(&p, &node) || node == 0xffff) return {};
        out[static_cast<std::size_t>(cpu)] = node;
    }
#elif defined(__linux__)
    out.assign(static_cast<std::size_t>(n), -1);
    for (int node = 0; node < 1024; ++node) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
        std::FILE* f = std::fopen(path, "r");
        if (!f) {
            if (node == 0) return {};
            break;
        }
        // "0-31,64-95": ranges and single CPUs, comma separated.
        int a = 0, b = 0;
        char sep = 0;
        while (std::fscanf(f, "%d", &a) == 1) {
            b = a;
            if (std::fscanf(f, "%c", &sep) == 1 && sep == '-') {
                if (std::fscanf(f, "%d", &b) != 1) break;
                if (std::fscanf(f, "%c", &sep) != 1) sep = 0;
            }
            for (int c = a; c <= b; ++c)
                if (c >= 0 && c < n) out[static_cast<std::size_t>(c)] = node;
            if (sep != ',') break;
        }
        std::fclose(f);
    }
    for (int v : out)
        if (v < 0) return {};  // online CPUs not numbered 0..n-1: say nothing rather than guess
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
// not show it; unpinned multi-threaded runs do (the mpmc kata measured 4x).
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

}  // namespace keystone
