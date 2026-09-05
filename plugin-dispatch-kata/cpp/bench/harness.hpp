#pragma once
//
// Shared benchmark plumbing: assertions, the measured run, and repetition.

#include "packet.hpp"
#include "platform.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace switchyard::bench {

// Records a failure and prints it. The process exit code is derived from
// failure_count(), so an invariant that breaks fails the build, not just the
// eye of whoever is reading the output.
void check(bool cond, const std::string& what);
int  failure_count();

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

struct Measured {
    double        ns_per_packet;
    std::uint64_t checksum;
};

// One timed pass of one arm over one packet stream.
//
// Templated on the arm rather than taking a base-class pointer, because an arm
// reached through a virtual call would add an indirect call to every single
// measurement — the same order of magnitude as some of the differences this
// benchmark exists to report.
//
// The checksum is not decoration. This is a single-threaded loop whose results
// are otherwise unused, which a dead-code eliminator is entitled to delete
// outright; accumulating into a value that is later asserted keeps the work
// alive structurally, without a compiler-specific barrier, and doubles as the
// cross-arm correctness invariant.
template <typename Arm>
Measured run_arm(Arm& arm, const Packet* pkts, std::size_t n) {
    // The pool is replayed until kDispatchesPerRun dispatches have happened, so
    // the sample count is fixed while the working set stays L2-resident. The
    // pool is far too large for a branch predictor to memorize, so replaying it
    // does not make the targets predictable.
    const std::size_t passes = (kDispatchesPerRun + n - 1) / n;
    Checksum sum;
    const auto t0 = Clock::now();
    for (std::size_t r = 0; r < passes; ++r)
        for (std::size_t i = 0; i < n; ++i) sum.feed(arm.dispatch(pkts[i]));
    const auto t1 = Clock::now();
    const double secs  = std::chrono::duration<double>(t1 - t0).count();
    const double total = static_cast<double>(passes) * static_cast<double>(n);
    return Measured{secs * 1e9 / total, sum.value()};
}

// kReps timed passes, plus one discarded warm-up.
//
// The arm is rebuilt for every repetition. That matters for the self-organizing
// scan, which mutates its own order as it runs: reusing one instance would
// measure a list that had already learned the distribution during the warm-up,
// which is the arm next to it, not this one.
template <typename Factory>
Measured measure(Factory make, const Packet* pkts, std::size_t n) {
    {
        auto warm = make();
        run_arm(warm, pkts, n);
    }
    std::vector<Measured> reps;
    reps.reserve(kReps);
    std::uint64_t sum = 0;
    for (int i = 0; i < kReps; ++i) {
        auto arm = make();
        reps.push_back(run_arm(arm, pkts, n));
        sum = reps.back().checksum;
    }
    return Measured{median_of(reps, [](const Measured& m) { return m.ns_per_packet; }), sum};
}

// Several arms, measured rep-major: one repetition of each arm, then the next
// repetition of each, never all repetitions of one arm before the next starts.
//
// This is not fussiness. Arm-major ordering was measurably wrong here: a phase
// runs for long enough that the core's clock drifts down under sustained load,
// so the arm that happened to run last was charged for the drift and the arm
// that ran first got a discount. Two arms doing identical work came out 27 %
// apart, in a stable and entirely reproducible way — which is worse than noise,
// because it looks like a result. Rep-major makes the drift common-mode.
template <typename... Fs>
auto measure_interleaved(const Packet* pkts, std::size_t n, Fs... fs) {
    constexpr std::size_t N = sizeof...(Fs);
    std::array<std::vector<Measured>, N> reps;
    for (auto& v : reps) v.reserve(kReps);

    // One discarded pass per arm, in the same order, for the same reason.
    (void)std::initializer_list<int>{(([&] {
                                         auto a = fs();
                                         run_arm(a, pkts, n);
                                     }()),
                                     0)...};

    for (int r = 0; r < kReps; ++r) {
        std::size_t k = 0;
        (void)std::initializer_list<int>{(([&] {
                                             auto a = fs();
                                             reps[k++].push_back(run_arm(a, pkts, n));
                                         }()),
                                         0)...};
    }

    std::array<Measured, N> out{};
    for (std::size_t i = 0; i < N; ++i)
        out[i] = Measured{median_of(reps[i], [](const Measured& m) { return m.ns_per_packet; }),
                          reps[i].back().checksum};
    return out;
}

// The same idea when the arms do not share a packet stream.
//
// Phase 4 subtracts one column from another to price the instruction cache, and
// two separate interleaved sets would put the drift the interleaving exists to
// remove straight back into the subtraction. Pairing each factory with its own
// stream lets a difference-of-columns be measured under one set of conditions.
template <typename F>
struct ArmOver {
    const Packet* pkts;
    std::size_t   n;
    F             make;
};
template <typename F>
ArmOver(const Packet*, std::size_t, F) -> ArmOver<F>;

template <typename... As>
auto measure_arms(As... as) {
    constexpr std::size_t N = sizeof...(As);
    std::array<std::vector<Measured>, N> reps;
    for (auto& v : reps) v.reserve(kReps);

    (void)std::initializer_list<int>{(([&] {
                                         auto a = as.make();
                                         run_arm(a, as.pkts, as.n);
                                     }()),
                                     0)...};

    for (int r = 0; r < kReps; ++r) {
        std::size_t k = 0;
        (void)std::initializer_list<int>{(([&] {
                                             auto a = as.make();
                                             reps[k++].push_back(run_arm(a, as.pkts, as.n));
                                         }()),
                                         0)...};
    }

    std::array<Measured, N> out{};
    for (std::size_t i = 0; i < N; ++i)
        out[i] = Measured{median_of(reps[i], [](const Measured& m) { return m.ns_per_packet; }),
                          reps[i].back().checksum};
    return out;
}

// Probes per packet, counted in a separate untimed pass. A counter incremented
// on the measured path would cost more than several of the differences being
// reported here.
template <typename Arm>
double count_probes(Arm& arm, const Packet* pkts, std::size_t n) {
    arm.reset_probes();
    Checksum sum;
    for (std::size_t i = 0; i < n; ++i) sum.feed(arm.dispatch_counted(pkts[i]));
    // Consumed so the pass is not elided; the value is not otherwise used.
    if (sum.value() == 1) std::printf(" ");
    return static_cast<double>(arm.probes()) / static_cast<double>(n);
}

}  // namespace switchyard::bench
