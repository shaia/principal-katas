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

namespace keystone::bench {

// Work in the correctness runs is divided by this under ThreadSanitizer, which
// runs the same code an order of magnitude slower. The races it looks for need
// many interleavings, not many instances.
#if defined(KEYSTONE_TSAN)
inline constexpr std::uint64_t kScale = 20;
#else
inline constexpr std::uint64_t kScale = 1;
#endif

// Records a failure and prints it. The exit code is derived from
// failure_count(), so an invariant that breaks fails the run, not just the eye
// of whoever is reading the output.
void check(bool cond, const std::string& what);
int  failure_count();

// Facts established once and read by every phase.
struct Context {
    TscClock         clk = TscClock::calibrate();
    std::vector<int> efficiency;  // per logical CPU; empty when the OS does not say
    int              fast_cpu = 0;
    int              slow_cpu = 0;
    bool             hybrid   = false;
    double           us_ticks(double us) const { return static_cast<double>(clk.ticks(us * 1e3)); }
};

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

// What a watchdog does when a run hangs: exit 3 after 60 s normally; in --bug
// mode exit 1 after 15 s, because there a hang is one of the ways a deliberate
// bug is detected.
void set_hang_policy(int exit_code, int seconds);
int  hang_exit_code();
int  hang_seconds();

// Ends the process if a run has not finished by its deadline: a scheduler that
// loses a wakeup or a dependency hangs, and must fail loudly, not silently.
class Watchdog {
public:
    Watchdog(std::string what, std::chrono::seconds limit, int exit_code = hang_exit_code());
    ~Watchdog();
    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    done_ = false;
    std::thread             thread_;
};

}  // namespace keystone::bench
