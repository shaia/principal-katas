#pragma once
//
// Shared benchmark plumbing: assertions, medians, formatting, the calibrated
// clock every phase reports through, and a watchdog for runs that can hang.

#include "platform.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace turnstile::bench {

// Event counts in the correctness runs are divided by this under
// ThreadSanitizer, which runs the same code an order of magnitude slower or
// worse. The races it looks for need many interleavings, not many events.
#if defined(TURNSTILE_TSAN)
inline constexpr std::uint64_t kScale = 20;
#else
inline constexpr std::uint64_t kScale = 1;
#endif

// Records a failure and prints it. The process exit code is derived from
// failure_count(), so an invariant that breaks fails the build, not just the
// eye of whoever is reading the output.
void check(bool cond, const std::string& what);
int  failure_count();

// Facts established once, by phase 0, and read by every later phase.
struct Context {
    TscClock         clk = TscClock::calibrate();
    std::vector<int> efficiency;  // per logical CPU; empty when the OS does not say
    int              fast_cpu = 0;  // a logical CPU of the highest efficiency class
    int              slow_cpu = 0;  // and of the lowest
    bool             hybrid   = false;
};

// Median across repetitions, computed per statistic rather than by picking one
// "median run". Choosing a whole run by one column lets a single noisy figure
// drag unrelated ones along with it.
template <typename T, typename F>
double median_of(const std::vector<T>& reps, F get) {
    std::vector<double> v;
    v.reserve(reps.size());
    for (const auto& r : reps) v.push_back(static_cast<double>(get(r)));
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// "812 ns", "1.94 us", "13.1 ms" — three significant figures, unit chosen to fit.
std::string fmt_ns(double ns);
// "4.72 M/s"
std::string fmt_rate(double per_sec);

// Ends the process if a run has not finished by its deadline. A shutdown that
// deadlocks, or a deliberately broken queue that livelocks, must fail loudly
// rather than leave the benchmark hanging with no verdict.
class Watchdog {
public:
    Watchdog(std::string what, std::chrono::seconds limit, int exit_code = 3);
    ~Watchdog();
    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    done_ = false;
    std::thread             thread_;
};

}  // namespace turnstile::bench
