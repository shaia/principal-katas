// Phase 3 — task size: what one task costs the scheduler.
//
//   a. A chain with empty bodies: nothing to balance, so the time per hop is
//      the scheduler's alone — handing the successor on through the deque, or
//      running it inline, with and without the dependency RMW a single-
//      predecessor node does not need.
//   b. A fan-out of 4,096 leaves at five grain sizes, and the same work fused
//      into fewer, larger tasks: efficiency is useful work over worker time.
//      Fusion buys back the overhead and spends balancing freedom; the last
//      column is where it has spent too much.
//   c. A wide join: 4,096 decrements of one counter, against a tree of partial
//      joins that spreads them over 64 counters.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace keystone::bench {
namespace {

const auto self = [](double x) { return x; };

}  // namespace

void phase_granularity(const Context& ctx) {
    // a. The chain.
    {
        const std::uint32_t n = 20'000;
        const Shape         s = chain(n, 0);
        struct Arm {
            const char* label;
            bool        inl, skip;
        };
        const Arm arms[] = {
            {"push to own deque, pop it back, RMW on every edge", false, false},
            {"push to own deque, pop it back, no RMW on 1-pred edges", false, true},
            {"inline continuation, RMW on every edge", true, false},
            {"inline continuation, no RMW on 1-pred edges", true, true},
        };
        std::vector<std::vector<double>> hop(std::size(arms));
        for (int rep = 0; rep < 5; ++rep)
            for (std::size_t i = 0; i < std::size(arms); ++i) {
                Config cfg;
                cfg.workers              = 4;
                cfg.inline_continuation  = arms[i].inl;
                cfg.skip_single_pred_rmw = arms[i].skip;
                Load load;
                load.kind      = Load::closed;
                load.in_flight = 1;
                load.instances = 50;
                const Result r = run(ctx, s, cfg, load, false);
                hop[i].push_back(r.wall_s * 1e9 / (static_cast<double>(r.st.completed) * n));
            }
        std::printf("  a. a chain of %u empty tasks, one instance at a time: time per hop (median of 5)\n", n);
        for (std::size_t i = 0; i < std::size(arms); ++i)
            std::printf("    %-56s %s\n", arms[i].label, fmt_ns(median_of(hop[i], self)).c_str());
    }

    // b. Grain and fusion.
    {
        const int                 workers = 16;
        const std::uint32_t       leaves  = 4096;
        const double              grains_us[] = {0, 0.25, 1, 4, 16};
        const std::uint32_t       groups[]    = {1, 8, 64, 512};
        std::printf("  b. %u leaves of g us each under one source, %d workers, 2 instances in flight:\n", leaves,
                    workers);
        std::printf("     worker time per leaf-unit of work (and efficiency: useful work / worker time), median of 3\n");
        std::printf("    %8s", "g");
        for (auto gr : groups) std::printf("  %20s", (std::to_string(gr) + (gr == 1 ? " leaf/task" : " leaves/task")).c_str());
        std::printf("\n");
        for (double g : grains_us) {
            std::vector<std::vector<double>> per(std::size(groups)), eff(std::size(groups));
            std::vector<Shape>               shapes;
            for (auto gr : groups) shapes.push_back(fan_out(leaves, ctx.clk.ticks(g * 1e3), gr));
            // Aim at about 0.25 s of work per arm.
            const double inst_us  = std::max(30.0, g * leaves / workers);
            const auto   instances = static_cast<std::uint64_t>(std::max(20.0, 250'000.0 / inst_us));
            for (int rep = 0; rep < 3; ++rep)
                for (std::size_t i = 0; i < std::size(groups); ++i) {
                    Config cfg;
                    cfg.workers = workers;
                    Load load;
                    load.kind      = Load::closed;
                    load.in_flight = 2;
                    load.instances = instances;
                    const Result r     = run(ctx, shapes[i], cfg, load, false);
                    const double wt_ns = r.wall_s * 1e9 * workers;
                    per[i].push_back(wt_ns / (static_cast<double>(r.st.completed) * leaves));
                    eff[i].push_back(ctx.clk.to_ns(r.st.busy_ticks) / wt_ns);
                }
            std::printf("    %6.2f us", g);
            for (std::size_t i = 0; i < std::size(groups); ++i) {
                char buf[64];
                if (g == 0) std::snprintf(buf, sizeof buf, "%s", fmt_ns(median_of(per[i], self)).c_str());
                else std::snprintf(buf, sizeof buf, "%s (%2.0f%%)", fmt_ns(median_of(per[i], self)).c_str(),
                                   100 * median_of(eff[i], self));
                std::printf("  %20s", buf);
            }
            std::printf("\n");
        }
    }

    // c. The wide join.
    {
        const Shape flat = fan_out(4096, 0, 1, false);
        const Shape tree = fan_out(4096, 0, 1, true);
        std::vector<double> f, t;
        for (int rep = 0; rep < 5; ++rep)
            for (int which = 0; which < 2; ++which) {
                Config cfg;
                cfg.workers = 16;
                Load load;
                load.kind      = Load::closed;
                load.in_flight = 2;
                load.instances = 400;
                const Result r = run(ctx, which ? tree : flat, cfg, load, false);
                (which ? t : f).push_back(r.wall_s * 1e9 / static_cast<double>(r.st.completed));
            }
        std::printf("  c. 4,096 empty leaves into one join, 16 workers: %s per instance flat (one counter takes 4,096 "
                    "decrements), %s through 64 partial joins\n",
                    fmt_ns(median_of(f, self)).c_str(), fmt_ns(median_of(t, self)).c_str());
    }
}

}  // namespace keystone::bench
