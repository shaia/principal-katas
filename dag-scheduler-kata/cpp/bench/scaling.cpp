// Phase 6 — the follow-up: more workers, more thieves.
//
// "Going from 16 to 64 workers raises throughput but worsens p99 from 4 ms to
// 18 ms while steal attempts explode." The worker counts and rates are derived
// from this machine's CPU count C, so that the same binary asks the same
// question on 24 CPUs and on 64 cores: the brief's 16 and 64, C itself, and 2C,
// which oversubscribes any machine. Two parts:
//
//   a. What thieves cost the work they steal from. A chain of empty tasks, each
//      successor pushed and popped back by its owner — unless a thief takes it
//      first, in which case the chain moves core. One task exists at a time, so
//      every other worker is a thief with nothing to do but try.
//   b. An event workload at two rates: 12 cores of work, which 16 workers
//      sustain (the brief's 16 at its 88 %), and 0.76 C cores, which they
//      cannot and the machine can. Greedy thieves against a search budget:
//      capacity, tail latency, steal attempts, and the CPU spent. Then the
//      redesign one change at a time: tasks fused 8 to 1, a budget of four
//      searchers, both, and both at the CPU count. Then where each arm's slow
//      events lost their time: a body taken off its CPU, a late start from the
//      injection queue, or neither.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace keystone::bench {
namespace {

const auto self = [](double x) { return x; };

int cpus() { return std::max(1, logical_cpus()); }

std::vector<int> sorted_unique(std::vector<int> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

void victim_cost(const Context& ctx) {
    const std::uint32_t n  = 20'000;
    const Shape         s  = chain(n, 0);
    const int           c  = cpus();
    const auto          ws = sorted_unique({1, 2, 4, 8, 16, std::max(1, c / 2), c, 2 * c, 64});
    struct Cell {
        std::vector<double> hop, stolen, tries;
    };
    std::vector<Cell> greedy(ws.size()), budget(ws.size());
    for (int rep = 0; rep < 3; ++rep)
        for (std::size_t i = 0; i < std::size(ws); ++i)
            for (Idle idle : {Idle::greedy, Idle::budgeted}) {
                Config cfg;
                cfg.workers             = ws[i];
                cfg.idle                = idle;
                cfg.inline_continuation = false;
                Load load;
                load.kind      = Load::closed;
                load.in_flight = 1;
                load.instances = 20;
                const Result r    = run(ctx, s, cfg, load, false);
                const double hops = static_cast<double>(r.st.completed) * n;
                Cell&        c    = idle == Idle::greedy ? greedy[i] : budget[i];
                c.hop.push_back(r.wall_s * 1e9 / hops);
                c.stolen.push_back(100.0 * static_cast<double>(r.st.steals) / hops);
                c.tries.push_back(static_cast<double>(r.st.steal_attempts) / hops);
            }
    std::printf("  a. a chain of %u empty tasks, each pushed and popped back unless stolen first; one task exists at a\n"
                "     time, so every other worker is a thief (median of 3, interleaved)\n", n);
    std::printf("    %8s   %-34s   %-34s\n", "", "greedy: never sleeps", "budgeted: half may search");
    std::printf("    %8s   %10s %10s %12s   %10s %10s %12s\n", "workers", "per hop", "stolen", "tries/hop", "per hop",
                "stolen", "tries/hop");
    for (std::size_t i = 0; i < std::size(ws); ++i)
        std::printf("    %8d   %10s %9.1f%% %12.2f   %10s %9.1f%% %12.2f\n", ws[i],
                    fmt_ns(median_of(greedy[i].hop, self)).c_str(), median_of(greedy[i].stolen, self),
                    median_of(greedy[i].tries, self), fmt_ns(median_of(budget[i].hop, self)).c_str(),
                    median_of(budget[i].stolen, self), median_of(budget[i].tries, self));
}

struct Arm {
    std::string label;
    int         workers;
    Idle        idle;
    int         max_searchers = 0;
    int         shape         = 0;  // 0: 2 us leaves; 1: fused 8 to 1
};

struct Row {
    std::vector<double> p50, p99, p999, attempts, success, cores, parks;
    std::vector<double> p99_clean, inject_p99;
    std::uint64_t       slow = 0, stalled = 0, late = 0, late_submit = 0, neither = 0, stalls = 0;
};

void under_load(const Context& ctx) {
    const Shape shapes[] = {
        event_graph(4, ctx.clk.ticks(100e3), 256, ctx.clk.ticks(2e3), 1),
        event_graph(4, ctx.clk.ticks(100e3), 256, ctx.clk.ticks(2e3), 8),
    };
    const double work_us = ctx.clk.to_ns(shapes[0].total_work()) / 1e3;
    const int    c       = cpus();
    // Rounded to a thousand events a second, so the rate reads as a choice and
    // not as an artifact of this run's clock calibration.
    const auto   rate_for = [&](double cores) { return std::max(1000.0, std::round(cores * 1e3 / work_us) * 1e3); };
    const double rates[]  = {rate_for(12), std::max(rate_for(12) * 1.25, rate_for(0.76 * c))};
    std::printf("  b. event: chain 4 x 100 us beside 256 leaves x 2 us, %.0f us of work. Open loop at %.0f/s (%.1f cores\n"
                "     of work, which 16 workers sustain) and %.0f/s (%.1f cores, 0.76 of the %d CPUs, which they cannot)\n",
                work_us, rates[0], rates[0] * work_us / 1e6, rates[1], rates[1] * work_us / 1e6, c);

    std::vector<Arm> arms;
    for (int w : sorted_unique({16, 32, c, 64, 2 * c})) {
        arms.push_back({std::to_string(w) + " greedy", w, Idle::greedy});
        arms.push_back({std::to_string(w) + " budgeted", w, Idle::budgeted});
    }
    // The redesign, one change at a time: fusion alone, the budget alone, both,
    // and both at the CPU count.
    arms.push_back({"64 greedy, fused x8", 64, Idle::greedy, 0, 1});
    arms.push_back({"64 budget 4", 64, Idle::budgeted, 4, 0});
    arms.push_back({"64 budget 4, fused x8", 64, Idle::budgeted, 4, 1});
    if (c != 64) arms.push_back({std::to_string(c) + " budget 4, fused x8", c, Idle::budgeted, 4, 1});

    const std::uint64_t stall_ticks = ctx.clk.ticks(250e3);  // a body more than 250 us past its deadline
    const std::uint64_t slow_ticks  = ctx.clk.ticks(2e6);    // an event slower than 2 ms
    // KEYSTONE_STALL_TRACE=0 turns the tracing off, for checking that it does not
    // change what it measures. Table c is then empty.
    const bool trace = [] {
#if defined(_WIN32)
        char*       v = nullptr;
        std::size_t n = 0;
        if (_dupenv_s(&v, &n, "KEYSTONE_STALL_TRACE") != 0) return true;
        const bool off = v && std::strcmp(v, "0") == 0;
        std::free(v);
        return !off;
#else
        const char* v = std::getenv("KEYSTONE_STALL_TRACE");
        return !(v && std::strcmp(v, "0") == 0);
#endif
    }();
    if (!trace) std::printf("     (stall tracing off: KEYSTONE_STALL_TRACE=0)\n");

    std::vector<std::vector<double>> cap(arms.size());
    std::vector<std::vector<Row>>    rows(std::size(rates), std::vector<Row>(arms.size()));
    for (int rep = 0; rep < 3; ++rep)
        for (std::size_t i = 0; i < arms.size(); ++i) {
            const Arm& a = arms[i];
            Config     cfg;
            cfg.workers       = a.workers;
            cfg.idle          = a.idle;
            cfg.max_searchers = a.max_searchers;
            const Shape& s    = shapes[a.shape];

            Load closed;
            closed.kind      = Load::closed;
            closed.in_flight = 2 * a.workers;
            closed.instances = 8000;
            cap[i].push_back(run(ctx, s, cfg, closed, false).per_sec());

            for (std::size_t ri = 0; ri < std::size(rates); ++ri) {
                Load open;
                open.kind    = Load::open;
                open.rate    = rates[ri];
                open.seconds = 1.5;
                open.pool    = 4096;
                Config traced      = cfg;
                traced.stall_ticks = trace ? stall_ticks : 0;
                traced.slow_ticks  = slow_ticks;
                const Result o = run(ctx, s, traced, open, false);
                check(o.st.completed == o.submitted, a.label + ": every admitted event completed");
                Row& r = rows[ri][i];
                r.p50.push_back(ctx.clk.to_ns(o.st.latency.quantile(0.5)));
                r.p99.push_back(ctx.clk.to_ns(o.st.latency.quantile(0.99)));
                r.p999.push_back(ctx.clk.to_ns(o.st.latency.quantile(0.999)));
                r.attempts.push_back(static_cast<double>(o.st.steal_attempts) / o.wall_s);
                r.success.push_back(o.st.steal_attempts ? 100.0 * static_cast<double>(o.st.steals) /
                                                              static_cast<double>(o.st.steal_attempts)
                                                        : 0);
                r.cores.push_back(o.cores_busy());
                r.parks.push_back(static_cast<double>(o.st.parks) / o.wall_s);
                r.p99_clean.push_back(ctx.clk.to_ns(o.st.latency_clean.quantile(0.99)));
                r.inject_p99.push_back(ctx.clk.to_ns(o.st.inject_delay.quantile(0.99)));
                // Stalled and late may overlap; neither excludes both. So each is at
                // most `slow`, and neither plus either one is too.
                check(o.st.slow_neither + std::max(o.st.slow_stalled, o.st.slow_late) <= o.st.slow &&
                          o.st.slow <= o.st.slow_neither + o.st.slow_stalled + o.st.slow_late,
                      a.label + ": slow events attributed");
                r.slow += o.st.slow, r.stalled += o.st.slow_stalled, r.late += o.st.slow_late;
                r.late_submit += o.st.slow_late_submit;
                r.neither += o.st.slow_neither, r.stalls += o.st.stalls;
            }
        }
    for (std::size_t ri = 0; ri < std::size(rates); ++ri) {
        std::printf("    at %.0f/s:\n", rates[ri]);
        std::printf("    %-24s %11s %9s %9s %9s %13s %8s %6s %9s\n", "workers, idle policy", "capacity/s", "p50", "p99",
                    "p99.9", "steal tries/s", "success", "cores", "parks/s");
        for (std::size_t i = 0; i < arms.size(); ++i) {
            const Row& r = rows[ri][i];
            std::printf("    %-24s %11.0f %9s %9s %9s %13.3g %7.2f%% %6.1f %9.0f\n", arms[i].label.c_str(),
                        median_of(cap[i], self), fmt_ns(median_of(r.p50, self)).c_str(),
                        fmt_ns(median_of(r.p99, self)).c_str(), fmt_ns(median_of(r.p999, self)).c_str(),
                        median_of(r.attempts, self), median_of(r.success, self), median_of(r.cores, self),
                        median_of(r.parks, self));
        }
    }
    std::printf("    (median of 3, interleaved. capacity: closed loop, 2 instances per worker in flight. The rest:\n"
                "     open loop; cores is CPU time per second of wall time, the submitter's excluded)\n");

    std::printf("  c. where the slow events lost their time. A body that ran more than 250 us past its deadline was off\n"
                "     its CPU (bodies spin to a deadline, so nothing else makes them late). An event slower than 2 ms is\n"
                "     counted as stalled if one of its bodies was, late if it left the injection queue more than 1 ms\n"
                "     after it was due, or neither. Of the late, 'by submitter' is the part where the benchmark's own\n"
                "     thread queued the event late, which no scheduler can fix. Counts summed over 3 runs; p99s are\n"
                "     medians.\n");
    for (std::size_t ri = 0; ri < std::size(rates); ++ri) {
        std::printf("    at %.0f/s:\n", rates[ri]);
        std::printf("    %-24s %9s %13s %12s %8s %8s %8s %12s %8s %13s\n", "workers, idle policy", "p99",
                    "p99 unstalled", "inject p99", "> 2 ms", "stalled", "late", "by submitter", "neither",
                    "stalled bodies");
        for (std::size_t i = 0; i < arms.size(); ++i) {
            const Row& r = rows[ri][i];
            std::printf("    %-24s %9s %13s %12s %8llu %8llu %8llu %12llu %8llu %13llu\n", arms[i].label.c_str(),
                        fmt_ns(median_of(r.p99, self)).c_str(), fmt_ns(median_of(r.p99_clean, self)).c_str(),
                        fmt_ns(median_of(r.inject_p99, self)).c_str(), static_cast<unsigned long long>(r.slow),
                        static_cast<unsigned long long>(r.stalled), static_cast<unsigned long long>(r.late),
                        static_cast<unsigned long long>(r.late_submit), static_cast<unsigned long long>(r.neither),
                        static_cast<unsigned long long>(r.stalls));
        }
    }
}

}  // namespace

void phase_scaling(const Context& ctx) {
    victim_cost(ctx);
    under_load(ctx);
}

}  // namespace keystone::bench
