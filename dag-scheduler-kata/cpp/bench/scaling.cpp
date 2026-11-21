// Phase 6 — the follow-up: more workers, more thieves.
//
// "Going from 16 to 64 workers raises throughput but worsens p99 from 4 ms to
// 18 ms while steal attempts explode." This machine has 24 logical CPUs and
// one socket, so 64 workers is oversubscription rather than 64 cores, and no
// steal here crosses a socket. Two parts:
//
//   a. What thieves cost the work they steal from. A chain of empty tasks, each
//      successor pushed and popped back by its owner — unless a thief takes it
//      first, in which case the chain moves core. One task exists at a time, so
//      every other worker is a thief with nothing to do but try.
//   b. An event workload that 16 workers can sustain, run with 16 to 64
//      workers, greedy thieves against a search budget: capacity, tail latency,
//      steal attempts, and the CPU spent, at a rate 16 workers sustain and one
//      they cannot. Then the redesign one change at a time: tasks fused 8 to 1,
//      a budget of four searchers, both, and both at the CPU count.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace keystone::bench {
namespace {

const auto self = [](double x) { return x; };

void victim_cost(const Context& ctx) {
    const std::uint32_t n = 20'000;
    const Shape         s = chain(n, 0);
    const int           ws[] = {1, 2, 4, 8, 16, 24, 32, 64};
    struct Cell {
        std::vector<double> hop, stolen, tries;
    };
    std::vector<Cell> greedy(std::size(ws)), budget(std::size(ws));
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
};

void under_load(const Context& ctx) {
    const Shape shapes[] = {
        event_graph(4, ctx.clk.ticks(100e3), 256, ctx.clk.ticks(2e3), 1),
        event_graph(4, ctx.clk.ticks(100e3), 256, ctx.clk.ticks(2e3), 8),
    };
    const double work_us = ctx.clk.to_ns(shapes[0].total_work()) / 1e3;
    const double rates[] = {13000, 20000};
    std::printf("  b. event: chain 4 x 100 us beside 256 leaves x 2 us, %.0f us of work. Open loop at %.0f/s (%.1f cores\n"
                "     of work, which 16 workers sustain) and %.0f/s (%.1f cores, which they cannot)\n",
                work_us, rates[0], rates[0] * work_us / 1e6, rates[1], rates[1] * work_us / 1e6);

    std::vector<Arm> arms;
    for (int w : {16, 24, 32, 64}) {
        arms.push_back({std::to_string(w) + " greedy", w, Idle::greedy});
        arms.push_back({std::to_string(w) + " budgeted", w, Idle::budgeted});
    }
    // The redesign, one change at a time: fusion alone, the budget alone, both.
    arms.push_back({"64 greedy, fused x8", 64, Idle::greedy, 0, 1});
    arms.push_back({"64 budget 4", 64, Idle::budgeted, 4, 0});
    arms.push_back({"64 budget 4, fused x8", 64, Idle::budgeted, 4, 1});
    arms.push_back({"24 budget 4, fused x8", 24, Idle::budgeted, 4, 1});

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
                const Result o = run(ctx, s, cfg, open, false);
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
}

}  // namespace

void phase_scaling(const Context& ctx) {
    victim_cost(ctx);
    under_load(ctx);
}

}  // namespace keystone::bench
