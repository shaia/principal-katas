// Phase 0 — the instrument.
//
// Nothing here is a result. It is the set of facts the later numbers depend
// on: the clocks and their skew between cores, the kinds of core, what a pause
// and a cache-line transfer cost, what the dependency decrement costs under
// every ordering the brief's question is about, and what a wake costs.

#include "eventcount.hpp"
#include "harness.hpp"
#include "histogram.hpp"
#include "phases.hpp"
#include "platform.hpp"
#include "scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace keystone::bench {
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

// Two threads pass a counter back and forth, each stamping its TSC. Half the
// difference of the smallest one-way readings is the skew; half their sum is a
// lower bound on moving one line between the two cores.
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
    for (std::uint64_t r = 0; r < static_cast<std::uint64_t>(rounds); ++r) {
        stamp.store(TscClock::now(), std::memory_order_relaxed);
        turn.store(2 * r + 1, std::memory_order_release);
        while (turn.load(std::memory_order_acquire) != 2 * r + 2) {}
        const auto t = static_cast<std::int64_t>(TscClock::now());
        min_ba = std::min(min_ba, t - static_cast<std::int64_t>(stamp.load(std::memory_order_relaxed)));
    }
    b.join();
    unpin();
    return PingPong{ctx.clk.to_ns(static_cast<double>(min_ab - min_ba) / 2),
                    ctx.clk.to_ns(static_cast<double>(min_ab + min_ba) / 2)};
}

double pause_ns(const Context& ctx, int cpu) {
    pin_to_cpu(cpu);
    std::this_thread::yield();
    constexpr int kN = 200000;
    const auto    t0 = TscClock::now();
    for (int i = 0; i < kN; ++i) KEYSTONE_PAUSE();
    const auto t1 = TscClock::now();
    unpin();
    return ctx.clk.to_ns(t1 - t0) / kN;
}

// k unpinned threads decrementing one counter that never reaches the last
// predecessor's branch: the price of resolving an edge into a wide join. Per
// operation, as the issuing thread sees it.
template <DecOrder O>
double join_decrement_ns(const Context& ctx, int k, int ops) {
    alignas(kCacheLine) std::atomic<std::uint32_t> counter{0xF0000000u};
    std::atomic<int>                               ready{0};
    std::atomic<bool>                              go{false};
    std::vector<std::thread>                       ts;
    std::atomic<std::uint64_t>                     sink{0};
    for (int i = 0; i < k; ++i)
        ts.emplace_back([&] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            std::uint64_t lasts = 0;
            for (int n = 0; n < ops; ++n) lasts += resolve_edge(counter, O);
            sink.fetch_add(lasts, std::memory_order_relaxed);
        });
    while (ready.load() < k) std::this_thread::yield();
    const auto t0 = TscClock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    return ctx.clk.to_ns(TscClock::now() - t0) / ops;
}

// One thread, the counter re-armed to 1 each time: every decrement is the last,
// so this is what the last predecessor's extra acquire (if any) costs.
template <DecOrder O>
double last_decrement_ns(const Context& ctx, int cpu) {
    pin_to_cpu(cpu);
    alignas(kCacheLine) std::atomic<std::uint32_t> counter{1};
    constexpr int kN    = 2'000'000;
    std::uint64_t lasts = 0;
    const auto    t0    = TscClock::now();
    for (int i = 0; i < kN; ++i) {
        counter.store(1, std::memory_order_relaxed);
        lasts += resolve_edge(counter, O);
    }
    const auto t1 = TscClock::now();
    unpin();
    if (lasts != kN) std::abort();
    return ctx.clk.to_ns(t1 - t0) / kN;
}

template <typename F>
double ns_per_call(const Context& ctx, F f, int n = 1'000'000) {
    const auto t0 = TscClock::now();
    for (int i = 0; i < n; ++i) f();
    return ctx.clk.to_ns(TscClock::now() - t0) / n;
}

// A sleeper waits on an address; the waker records what the wake costs it, and
// when the sleeper actually runs again.
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

double clock_indicator_ns(const Context& ctx) {
    pin_to_cpu(ctx.fast_cpu);
    std::this_thread::yield();
    double best = 1e9;
    for (int r = 0; r < 7; ++r) {
        constexpr int kN = 20000;
        const auto    t0 = TscClock::now();
        for (int i = 0; i < kN; ++i) KEYSTONE_PAUSE();
        best = std::min(best, ctx.clk.to_ns(TscClock::now() - t0) / kN);
    }
    unpin();
    return best;
}

Context make_context() {
    Context ctx;
    ctx.efficiency = cpu_efficiency_classes();
    if (!ctx.efficiency.empty()) {
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

    if (ctx.efficiency.empty()) {
        std::printf("  CPUs           %d logical; the OS does not say which kind (WSL hides it)\n", logical_cpus());
    } else {
        int fast = 0;
        for (int c : ctx.efficiency) fast += c == ctx.efficiency[static_cast<std::size_t>(ctx.fast_cpu)];
        std::printf("  CPUs           %d logical: %d of the fastest class (e.g. CPU %d), %d slower (e.g. CPU %d)\n",
                    logical_cpus(), fast, ctx.fast_cpu, logical_cpus() - fast, ctx.slow_cpu);
    }
    std::printf("  NUMA           one node: every cross-socket claim in the solution is reasoning, not measurement\n");

    // Another core of the fastest class: CPU numbering does not say which are
    // which on a hybrid part, so look it up rather than assume a neighbour.
    int other = std::min(1, logical_cpus() - 1);
    for (int c = 0; c < static_cast<int>(ctx.efficiency.size()); ++c)
        if (c != ctx.fast_cpu && ctx.efficiency[static_cast<std::size_t>(c)] ==
                                     ctx.efficiency[static_cast<std::size_t>(ctx.fast_cpu)]) {
            other = c;
            break;
        }
    const auto pp = ping_pong(ctx, ctx.fast_cpu, other);
    std::printf("  TSC skew       CPU %d vs CPU %d: %+.1f ns\n", ctx.fast_cpu, other, pp.skew_ns);
    check(pp.skew_ns > -100 && pp.skew_ns < 100, "TSC skew between cores under 100 ns, so cross-core latencies mean something");
    if (ctx.hybrid) {
        const auto pe = ping_pong(ctx, ctx.fast_cpu, ctx.slow_cpu);
        std::printf("  TSC skew       CPU %d vs CPU %d: %+.1f ns\n", ctx.fast_cpu, ctx.slow_cpu, pe.skew_ns);
        check(pe.skew_ns > -100 && pe.skew_ns < 100, "TSC skew to a slower core under 100 ns");
        std::printf("  line transfer  one way, fast CPU %d -> fast CPU %d: %s;  -> slow CPU %d: %s\n", ctx.fast_cpu,
                    other, fmt_ns(pp.one_way_ns).c_str(), ctx.slow_cpu, fmt_ns(pe.one_way_ns).c_str());
    } else {
        std::printf("  line transfer  one way, CPU %d -> %d: %s\n", ctx.fast_cpu, other, fmt_ns(pp.one_way_ns).c_str());
    }

    const double pf = pause_ns(ctx, ctx.fast_cpu);
    const double ps = pause_ns(ctx, ctx.slow_cpu);
    std::printf("  pause          %s on CPU %d, %s on CPU %d\n", fmt_ns(pf).c_str(), ctx.fast_cpu, fmt_ns(ps).c_str(),
                ctx.slow_cpu);

    // The decrement, under each ordering. x86-64 has one locked instruction for
    // all of them, so any difference here is noise or the extra acquire.
    std::printf("  dependency decrement, one counter, unpinned threads, ns per decrement (median of 5, interleaved):\n");
    std::printf("    %-14s %8s %8s %8s %8s   %s\n", "ordering", "1 thr", "4 thr", "16 thr", "32 thr", "last one, alone");
    struct Row {
        DecOrder o;
        double (*join)(const Context&, int, int);
        double (*last)(const Context&, int);
    };
    const Row rows[] = {
        {DecOrder::relaxed, join_decrement_ns<DecOrder::relaxed>, last_decrement_ns<DecOrder::relaxed>},
        {DecOrder::acq_rel, join_decrement_ns<DecOrder::acq_rel>, last_decrement_ns<DecOrder::acq_rel>},
        {DecOrder::release_fence, join_decrement_ns<DecOrder::release_fence>, last_decrement_ns<DecOrder::release_fence>},
        {DecOrder::release_load, join_decrement_ns<DecOrder::release_load>, last_decrement_ns<DecOrder::release_load>},
        {DecOrder::seq_cst, join_decrement_ns<DecOrder::seq_cst>, last_decrement_ns<DecOrder::seq_cst>},
    };
    const int ks[] = {1, 4, 16, 32};
    std::vector<std::vector<std::vector<double>>> samples(std::size(rows), std::vector<std::vector<double>>(std::size(ks) + 1));
    for (int rep = 0; rep < 5; ++rep)
        for (std::size_t r = 0; r < std::size(rows); ++r) {
            for (std::size_t ki = 0; ki < std::size(ks); ++ki)
                samples[r][ki].push_back(rows[r].join(ctx, ks[ki], ks[ki] == 1 ? 2'000'000 : 400'000));
            samples[r][std::size(ks)].push_back(rows[r].last(ctx, ctx.fast_cpu));
        }
    const auto self = [](double x) { return x; };
    for (std::size_t r = 0; r < std::size(rows); ++r) {
        std::printf("    %-14s", name(rows[r].o));
        for (std::size_t ki = 0; ki < std::size(ks); ++ki) std::printf(" %8s", fmt_ns(median_of(samples[r][ki], self)).c_str());
        std::printf("   %s\n", fmt_ns(median_of(samples[r][std::size(ks)], self)).c_str());
    }

    // The wake. With nobody asleep: what every push into a deque pays under the
    // budgeted policy.
    EventCount                 ec;
    std::atomic<std::uint32_t> word{0};
    const double fence  = ns_per_call(ctx, [] { std::atomic_thread_fence(std::memory_order_seq_cst); });
    const double ecount = ns_per_call(ctx, [&] { ec.notify_one(); });
    const double os     = ns_per_call(ctx, [&] { wake_one(word); }, 200'000);
    std::printf("  wake, nobody asleep:   seq_cst fence %s;  EventCount::notify_one %s;  the OS wake regardless %s\n",
                fmt_ns(fence).c_str(), fmt_ns(ecount).c_str(), fmt_ns(os).c_str());
    const auto wk = wake_a_sleeper();
    std::printf("  wake, somebody asleep: the waker pays p50 %s, p99 %s;  the sleeper runs again after p50 %s, p99 %s\n",
                fmt_ns(ctx.clk.to_ns(wk.waker.quantile(0.5))).c_str(),
                fmt_ns(ctx.clk.to_ns(wk.waker.quantile(0.99))).c_str(),
                fmt_ns(ctx.clk.to_ns(wk.sleeper.quantile(0.5))).c_str(),
                fmt_ns(ctx.clk.to_ns(wk.sleeper.quantile(0.99))).c_str());
}

}  // namespace keystone::bench
