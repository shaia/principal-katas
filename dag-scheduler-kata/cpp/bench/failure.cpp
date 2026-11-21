// Phase 5 — failure propagation.
//
// The event graph of phase 4 at 60 % load, with one event in five failing at a
// uniformly chosen interior node — usually a leaf, sometimes a chain step.
// Two policies:
//
//   poison  — the failed node's descendants are skipped; everything else runs.
//   cancel  — fail-fast: every node not yet started is skipped.
//
// Either way every edge is still resolved exactly once: an instance cannot be
// reused until every thread that might touch it has finished with it, and the
// edge walk is how that is known. What differs is how long a failed event
// takes to report, and how much work runs after the failure was already known.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <vector>

namespace keystone::bench {

void phase_failure(const Context& ctx) {
    const int    workers  = 16;
    const Shape  s        = event_graph(8, ctx.clk.ticks(50e3), 512, ctx.clk.ticks(4e3));
    const double capacity = workers * 1e9 / ctx.clk.to_ns(s.total_work());
    std::printf("  phase 4's event at 60%% load (%.0f events/s), one in five failing at a random interior node\n",
                capacity * 0.6);
    std::printf("    %-8s %9s %14s %14s %16s %12s %12s\n", "policy", "failed", "fail->done p50", "fail->done p99",
                "bodies after fail", "all p50", "all p99");
    for (int rep = 0; rep < 2; ++rep)
        for (FailPolicy fp : {FailPolicy::poison, FailPolicy::cancel}) {
            Config cfg;
            cfg.workers = workers;
            cfg.fail    = fp;
            Load load;
            load.kind    = Load::open;
            load.rate    = capacity * 0.6;
            load.seconds = 2.0;
            load.pool    = 512;
            Faults f;
            f.probability  = 0.2;
            const Result r = run(ctx, s, cfg, load, true, f, 77 + static_cast<std::uint64_t>(rep));
            check(r.st.violations == 0 && r.st.bad_completions == 0, "failure: exactly-once and failure semantics");
            check(r.st.completed == r.submitted, "failure: every admitted event completed");
            const double per_failed = r.st.failed_instances
                                          ? static_cast<double>(r.st.wasted_after_failure) /
                                                static_cast<double>(r.st.failed_instances)
                                          : 0;
            std::printf("    %-8s %9llu %14s %14s %16.1f %12s %12s\n", fp == FailPolicy::poison ? "poison" : "cancel",
                        static_cast<unsigned long long>(r.st.failed_instances),
                        fmt_ns(ctx.clk.to_ns(r.st.fail_to_done.quantile(0.5))).c_str(),
                        fmt_ns(ctx.clk.to_ns(r.st.fail_to_done.quantile(0.99))).c_str(), per_failed,
                        fmt_ns(ctx.clk.to_ns(r.st.latency.quantile(0.5))).c_str(),
                        fmt_ns(ctx.clk.to_ns(r.st.latency.quantile(0.99))).c_str());
        }
}

}  // namespace keystone::bench
