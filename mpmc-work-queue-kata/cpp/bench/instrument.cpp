// Phase 0 — the instrument.
//
// Nothing here is a result. It is the set of facts every later number depends
// on, established first so that a later number cannot quietly be an artifact of
// one of them:
//
//   1. The clocks: steady_clock's smallest observable tick, the TSC's rate, and
//      the TSC's skew between cores. Every end-to-end latency in this benchmark
//      subtracts one core's TSC reading from another's.
//   2. The machine: how many CPUs, of which kinds. A pause and a line transfer
//      both cost different amounts on a P-core and an E-core.
//   3. The unit costs step 7 does its arithmetic with: one cache line moving
//      between two cores, and a contended read-modify-write as threads are added.
//   4. The wake, which step 5 prices against the 2 us budget: what each
//      primitive costs with nobody asleep — paid on every push — and what a
//      producer pays, and a sleeper waits, when somebody is.

#include "arms.hpp"
#include "config.hpp"
#include "driver.hpp"
#include "eventcount.hpp"
#include "harness.hpp"
#include "histogram.hpp"
#include "phases.hpp"
#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace turnstile::bench {
namespace {

double steady_tick_ns() {
    std::vector<std::int64_t> v;
    v.reserve(1 << 16);
    const auto deadline = Clock::now() + std::chrono::milliseconds(5);
    while (Clock::now() < deadline && v.size() < v.capacity()) v.push_back(Clock::now().time_since_epoch().count());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() < 2) return 0.0;
    std::int64_t smallest = v[1] - v[0];
    for (std::size_t i = 2; i < v.size(); ++i) smallest = std::min(smallest, v[i] - v[i - 1]);
    return static_cast<double>(smallest) * 1e9 * static_cast<double>(Clock::period::num) /
           static_cast<double>(Clock::period::den);
}

// Two threads on two cores pass a counter back and forth, each stamping its TSC.
// The smallest A→B difference is transfer latency plus skew, the smallest B→A is
// transfer latency minus skew; half their difference is the skew, and half their
// sum is a lower bound on the one-way transfer — the cost of moving one line.
struct PingPong {
    double skew_ns, one_way_ns;
};
PingPong ping_pong(const Context& ctx, int cpu_a, int cpu_b, int rounds = 20000) {
    alignas(kCacheLine) std::atomic<std::uint64_t> turn{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> stamp{0};
    std::int64_t min_ab = INT64_MAX, min_ba = INT64_MAX;
    std::thread  b([&] {
        pin_to_cpu(cpu_b);
        for (std::uint64_t r = 0; r < static_cast<std::uint64_t>(rounds); ++r) {
            while (turn.load(std::memory_order_acquire) != 2 * r + 1) {}
            const auto t = static_cast<std::int64_t>(TscClock::now());
            min_ab = std::min(min_ab, t - static_cast<std::int64_t>(stamp.load(std::memory_order_relaxed)));
            stamp.store(TscClock::now(), std::memory_order_relaxed);
            turn.store(2 * r + 2, std::memory_order_release);
        }
    });
    pin_to_cpu(cpu_a);
    std::int64_t local_min_ba = INT64_MAX;
    for (std::uint64_t r = 0; r < static_cast<std::uint64_t>(rounds); ++r) {
        stamp.store(TscClock::now(), std::memory_order_relaxed);
        turn.store(2 * r + 1, std::memory_order_release);
        while (turn.load(std::memory_order_acquire) != 2 * r + 2) {}
        const auto t = static_cast<std::int64_t>(TscClock::now());
        local_min_ba = std::min(local_min_ba, t - static_cast<std::int64_t>(stamp.load(std::memory_order_relaxed)));
    }
    b.join();
    unpin();
    min_ba = local_min_ba;
    return PingPong{ctx.clk.to_ns(static_cast<double>(min_ab - min_ba) / 2),
                    ctx.clk.to_ns(static_cast<double>(min_ab + min_ba) / 2)};
}

double pause_ns(const Context& ctx, int cpu) {
    pin_to_cpu(cpu);
    std::this_thread::yield();
    constexpr int kN = 200000;
    const auto    t0 = TscClock::now();
    for (int i = 0; i < kN; ++i) TURNSTILE_PAUSE();
    const auto t1 = TscClock::now();
    unpin();
    return ctx.clk.to_ns(t1 - t0) / kN;
}

// k unpinned threads hammering one counter: aggregate operations per second, and
// what each operation costs the thread that issues it.
struct Contended {
    double per_op_ns, aggregate_per_sec;
};
Contended contended_fetch_add(const Context& ctx, int k, int ops = 1'000'000) {
    alignas(kCacheLine) std::atomic<std::uint64_t> counter{0};
    std::atomic<int>                               ready{0};
    std::atomic<bool>                              go{false};
    std::vector<std::thread>                       ts;
    for (int i = 0; i < k; ++i)
        ts.emplace_back([&] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int n = 0; n < ops; ++n) counter.fetch_add(1, std::memory_order_relaxed);
        });
    while (ready.load() < k) std::this_thread::yield();
    const auto t0 = TscClock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    const double ns = ctx.clk.to_ns(TscClock::now() - t0);
    return Contended{ns / ops, static_cast<double>(k) * ops / (ns / 1e9)};
}

template <typename F>
double ns_per_call(const Context& ctx, F f, int n = 1'000'000) {
    const auto t0 = TscClock::now();
    for (int i = 0; i < n; ++i) f();
    return ctx.clk.to_ns(TscClock::now() - t0) / n;
}

// A sleeper waits on an address; the waker records what the wake costs it, and
// when the sleeper actually runs again. 300 rounds, the sleeper given time to
// really be asleep before each one.
struct WakeCost {
    Histogram waker, sleeper;
};
WakeCost wake_a_sleeper(int rounds = 300) {
    WakeCost                   out;
    std::atomic<std::uint32_t> word{0};
    std::atomic<std::uint64_t> woke_at{0};
    std::atomic<int>           asleep{-1};
    std::thread                sleeper([&] {
        for (int r = 0; r < rounds; ++r) {
            const std::uint32_t key = word.load(std::memory_order_acquire);
            asleep.store(r, std::memory_order_release);
            while (word.load(std::memory_order_acquire) == key) wait_on(word, key, std::chrono::seconds(1));
            woke_at.store(TscClock::now(), std::memory_order_release);
        }
    });
    for (int r = 0; r < rounds; ++r) {
        while (asleep.load(std::memory_order_acquire) != r) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        woke_at.store(0, std::memory_order_relaxed);
        const auto t0 = TscClock::now();
        word.fetch_add(1, std::memory_order_release);
        wake_one(word);
        const auto t1 = TscClock::now();
        out.waker.record(t1 - t0);
        std::uint64_t w;
        while ((w = woke_at.load(std::memory_order_acquire)) == 0) std::this_thread::yield();
        out.sleeper.record(w - t0);
    }
    sleeper.join();
    return out;
}

}  // namespace

// The smallest of several short readings: a reading that was interrupted is too
// high, never too low, so the minimum is the one that measured the clock.
double clock_indicator_ns(const Context& ctx) {
    pin_to_cpu(ctx.fast_cpu);
    std::this_thread::yield();
    double best = 1e9;
    for (int r = 0; r < 7; ++r) {
        constexpr int kN = 20000;
        const auto    t0 = TscClock::now();
        for (int i = 0; i < kN; ++i) TURNSTILE_PAUSE();
        best = std::min(best, ctx.clk.to_ns(TscClock::now() - t0) / kN);
    }
    unpin();
    return best;
}

Context make_context() {
    Context ctx;
    ctx.efficiency = cpu_efficiency_classes();
    if (!ctx.efficiency.empty()) {
        // The first CPU of each class, not minmax_element's last maximum.
        const auto hi = std::max_element(ctx.efficiency.begin(), ctx.efficiency.end());
        const auto lo = std::min_element(ctx.efficiency.begin(), ctx.efficiency.end());
        ctx.hybrid    = *lo != *hi;
        ctx.fast_cpu  = static_cast<int>(hi - ctx.efficiency.begin());
        ctx.slow_cpu  = static_cast<int>(lo - ctx.efficiency.begin());
    } else {
        ctx.fast_cpu = 0;
        ctx.slow_cpu = logical_cpus() - 1;
    }
    return ctx;
}

void phase_instrument(const Context& ctx) {
    std::printf("  steady_clock   smallest tick %s\n", fmt_ns(steady_tick_ns()).c_str());
    std::printf("  TSC            %.3f ns/tick (%.3f GHz), calibrated over 200 ms\n", ctx.clk.ns_per_tick(),
                1.0 / ctx.clk.ns_per_tick());

    // Topology.
    if (ctx.efficiency.empty()) {
        std::printf("  CPUs           %d logical; the OS does not say which kind (WSL hides it)\n", logical_cpus());
    } else {
        int fast = 0;
        for (int c : ctx.efficiency) fast += c == ctx.efficiency[static_cast<std::size_t>(ctx.fast_cpu)];
        std::printf("  CPUs           %d logical: %d of the fastest class (e.g. CPU %d), %d slower (e.g. CPU %d)\n",
                    logical_cpus(), fast, ctx.fast_cpu, logical_cpus() - fast, ctx.slow_cpu);
    }
    std::printf("  oversubscribed the brief's %d threads on %d CPUs: %s\n", kProducers + kConsumers, logical_cpus(),
                kProducers + kConsumers > logical_cpus() ? "yes — preemption inside a window is real here" : "no");

    // What the OS does to a benchmark whose window is not in the foreground: one
    // short saturated run with execution-speed throttling forced on, and off,
    // interleaved. Found the hard way: midway through writing this, every
    // multi-threaded number fell by about two thirds while single-thread pinned
    // ones did not move, and this is why.
    {
        std::vector<double> on, off;
        for (int rep = 0; rep < 3; ++rep)
            for (const bool throttled : {true, false}) {
                set_power_throttling(throttled);
                auto    q = std::make_unique<Turnstile>();
                RunSpec s;
                s.producers       = 8;
                s.consumers       = 4;
                s.events          = 250'000;
                const RunResult r = run(*q, s, ctx);
                check_run(r, "power-throttling A/B");
                (throttled ? on : off).push_back(r.rate);
            }
        set_power_throttling(false);
        const auto self = [](double x) { return x; };
#if defined(_WIN32)
        std::printf("  EcoQoS         8 x 4 saturated, median of 3: %s throttled, %s not — every later phase runs not\n",
                    fmt_rate(median_of(on, self)).c_str(), fmt_rate(median_of(off, self)).c_str());
#else
        std::printf("  EcoQoS         not on this OS; 8 x 4 saturated, median of 3: %s\n",
                    fmt_rate(median_of(off, self)).c_str());
#endif
    }

    // The TSC across cores. A neighbour that is a different physical core: on
    // this part logical CPUs 2k and 2k+1 are siblings, so 0 and 2 are not.
    const int  other = std::min(2, logical_cpus() - 1);
    const auto pp    = ping_pong(ctx, 0, other);
    std::printf("  TSC skew       CPU 0 vs CPU %d: %+.1f ns\n", other, pp.skew_ns);
    check(pp.skew_ns > -100 && pp.skew_ns < 100, "TSC skew between cores under 100 ns, so cross-core latencies mean something");
    if (ctx.slow_cpu != 0) {
        const auto pe = ping_pong(ctx, 0, ctx.slow_cpu);
        std::printf("  TSC skew       CPU 0 vs CPU %d: %+.1f ns\n", ctx.slow_cpu, pe.skew_ns);
        check(pe.skew_ns > -100 && pe.skew_ns < 100, "TSC skew to a slower core under 100 ns");
        std::printf("  line transfer  one way, CPU 0 -> %d: %s;  CPU 0 -> %d: %s\n", other,
                    fmt_ns(pp.one_way_ns).c_str(), ctx.slow_cpu, fmt_ns(pe.one_way_ns).c_str());
    } else {
        std::printf("  line transfer  one way, CPU 0 -> %d: %s\n", other, fmt_ns(pp.one_way_ns).c_str());
    }
    std::printf("                 (one socket: a cross-socket transfer cannot be measured on this machine)\n");

    // The spin unit.
    const double pf = pause_ns(ctx, ctx.fast_cpu);
    const double ps = pause_ns(ctx, ctx.slow_cpu);
    std::printf("  pause          %s on CPU %d, %s on CPU %d — the queue spins 128 of them before parking\n",
                fmt_ns(pf).c_str(), ctx.fast_cpu, fmt_ns(ps).c_str(), ctx.slow_cpu);

    // A contended read-modify-write, as threads are added.
    std::printf("  fetch_add on one line, unpinned threads:\n");
    for (int k : {1, 2, 4, 8, 16, 32}) {
        const auto c = contended_fetch_add(ctx, k);
        std::printf("    %2d threads   %7s per op per thread, %s aggregate   (the brief needs %s on the tail)\n", k,
                    fmt_ns(c.per_op_ns).c_str(), fmt_rate(c.aggregate_per_sec).c_str(),
                    fmt_rate(kTargetEventsPerSec).c_str());
    }

    // The wake. First with nobody waiting: the cost a producer pays on every push.
    std::atomic<std::uint32_t> word{0};
    EventCount                 ec;
    std::mutex                 mu;
    std::condition_variable    cv;
    const double fence   = ns_per_call(ctx, [] { std::atomic_thread_fence(std::memory_order_seq_cst); });
    const double ecount  = ns_per_call(ctx, [&] { ec.notify_one(); });
    const double atomic_ = ns_per_call(ctx, [&] { word.notify_one(); });
    const double os      = ns_per_call(ctx, [&] { wake_one(word); }, 200'000);
    const double condvar = ns_per_call(ctx, [&] { cv.notify_one(); });
    std::printf("  wake, nobody asleep (paid on every push):\n");
    std::printf("    seq_cst fence alone            %s\n", fmt_ns(fence).c_str());
    std::printf("    EventCount::notify_one         %s   (fence + one load of a shared line)\n", fmt_ns(ecount).c_str());
    std::printf("    std::atomic::notify_one        %s\n", fmt_ns(atomic_).c_str());
    std::printf("    condition_variable::notify_one %s\n", fmt_ns(condvar).c_str());
    std::printf("    the OS wake, unconditionally   %s   (%s)\n", fmt_ns(os).c_str(),
#if defined(_WIN32)
                "WakeByAddressSingle"
#else
                "futex FUTEX_WAKE"
#endif
    );
    (void)mu;

    const auto wk = wake_a_sleeper();
    std::printf("  wake, somebody asleep:\n");
    std::printf("    the waker pays                 p50 %s, p99 %s   (against a %s p99 push budget)\n",
                fmt_ns(ctx.clk.to_ns(wk.waker.quantile(0.5))).c_str(),
                fmt_ns(ctx.clk.to_ns(wk.waker.quantile(0.99))).c_str(), fmt_ns(kP99BudgetNs).c_str());
    std::printf("    the sleeper runs again after   p50 %s, p99 %s\n",
                fmt_ns(ctx.clk.to_ns(wk.sleeper.quantile(0.5))).c_str(),
                fmt_ns(ctx.clk.to_ns(wk.sleeper.quantile(0.99))).c_str());
}

}  // namespace turnstile::bench
