// Phase 4 — the critical path.
//
// Two graphs with the same work, 2,448 us, and the same critical path, 400 us:
//
//   fork     a source, then a chain of 8 steps of 50 us beside a fan-out of 512
//            leaves of 4 us. One decision: at the source.
//   pipeline 8 steps of 50 us, each making 64 leaves of 4 us ready beside the
//            next step. A decision at every step, and the builder numbers the
//            next step before the leaves (graph.hpp says why that is not rigged).
//
// Events arrive on a schedule, open loop, and latency runs from when each was
// due to when its sink completed. Four arms, interleaved at each load:
// successors resolved in id order or in rank order, and the last ready one
// pushed or run inline. Capacity first, per arm, because near saturation an
// arm a few percent slower is not at the same load as the others.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace keystone::bench {
namespace {

struct Arm {
    const char* label;
    bool        prio, inl;
};
const Arm kArms[] = {
    {"id order, push all", false, false},
    {"id order, inline last", false, true},
    {"rank order, push all", true, false},
    {"rank order, inline last", true, true},
};

Config arm_config(const Arm& a, int workers) {
    Config cfg;
    cfg.workers             = workers;
    cfg.priority            = a.prio;
    cfg.inline_continuation = a.inl;
    return cfg;
}

void one_graph(const Context& ctx, const char* title, const Shape& s, int workers) {
    const double work_us  = ctx.clk.to_ns(s.total_work()) / 1e3;
    const double cp_us    = ctx.clk.to_ns(s.critical_path()) / 1e3;
    const double capacity = workers * 1e6 / work_us;
    std::printf("  %s: %u nodes, %.0f us of work, critical path %.0f us; %d workers: at most %.0f events/s\n", title,
                s.n, work_us, cp_us, workers, capacity);
    // Brent: any schedule that never idles a worker while work is ready finishes
    // one event, alone, between these two. Priority can only move it within them.
    const double wp = work_us / workers;
    std::printf("    W/P = %.0f us; every work-conserving schedule of one event takes between max(W/P, CP) = %.0f us\n"
                "    and W/P + CP = %.0f us — the whole of what an ordering can win, before queueing\n",
                wp, std::max(wp, cp_us), wp + cp_us);

    std::printf("    measured capacity, closed loop, 32 in flight (median of 3):\n");
    for (const Arm& a : kArms) {
        std::vector<double> caps;
        for (int rep = 0; rep < 3; ++rep) {
            Load load;
            load.kind      = Load::closed;
            load.in_flight = 32;
            load.instances = 4000;
            caps.push_back(run(ctx, s, arm_config(a, workers), load, false).per_sec());
        }
        std::printf("      %-26s %6.0f/s\n", a.label, median_of(caps, [](double x) { return x; }));
    }
    std::printf("    %-26s %6s %10s %10s %10s %10s\n", "arm", "load", "p50", "p99", "p99.9", "max");
    for (double load_frac : {0.5, 0.8, 0.9}) {
        for (const Arm& a : kArms) {
            Load load;
            load.kind    = Load::open;
            load.rate    = capacity * load_frac;
            load.seconds = 2.0;
            load.pool    = 1024;
            const Result r = run(ctx, s, arm_config(a, workers), load, false);
            const auto&  h = r.st.latency;
            std::printf("    %-26s %5.0f%% %10s %10s %10s %10s\n", a.label, 100 * load_frac,
                        fmt_ns(ctx.clk.to_ns(h.quantile(0.5))).c_str(), fmt_ns(ctx.clk.to_ns(h.quantile(0.99))).c_str(),
                        fmt_ns(ctx.clk.to_ns(h.quantile(0.999))).c_str(), fmt_ns(ctx.clk.to_ns(h.max())).c_str());
            check(r.st.completed == r.submitted && r.rejected == 0,
                  std::string(title) + ": every event admitted and completed");
        }
    }
}

}  // namespace

void phase_critical_path(const Context& ctx) {
    const int workers = 16;
    one_graph(ctx, "fork", event_graph(8, ctx.clk.ticks(50e3), 512, ctx.clk.ticks(4e3)), workers);
    one_graph(ctx, "pipeline", pipeline_graph(8, ctx.clk.ticks(50e3), 64, ctx.clk.ticks(4e3)), workers);
    // The same pipeline on 4 workers: 64 leaves per step is 16 per worker, so a
    // worker that keeps the leaves is busy for 64 us at every step, and thieves
    // that would rescue a buried chain step are rarely idle.
    one_graph(ctx, "pipeline on 4 workers", pipeline_graph(8, ctx.clk.ticks(50e3), 64, ctx.clk.ticks(4e3)), 4);
}

}  // namespace keystone::bench
