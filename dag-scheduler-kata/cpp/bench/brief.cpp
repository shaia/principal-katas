// Phase 1 — the brief's relaxed decrement.
//
// Two questions, about seven orderings of the same decrement:
//
//   1. Does a stress test on this machine tell the broken ones from the correct
//      ones? Random DAGs, fan-in up to 8, zero-cost bodies so that the
//      predecessors of a join finish within nanoseconds of each other on
//      different cores, every input checked against the value it must have.
//   2. What does correctness cost? The same graphs, checking off, interleaved.
//
// The expected answer to the first is "no": x86-64 emits one `lock` instruction
// for every one of them, and the compiler did not move the payload store across
// it (probe/ordering_codegen.c shows both). So this phase cannot fail on x86 —
// which is the finding. ThreadSanitizer is the instrument that can see it.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <vector>

namespace keystone::bench {

void phase_brief(const Context& ctx) {
    const DecOrder orders[] = {DecOrder::relaxed,       DecOrder::release_only, DecOrder::acquire_only,
                               DecOrder::acq_rel,       DecOrder::release_fence, DecOrder::release_load,
                               DecOrder::seq_cst};
    const Shape shape = stress_shape(ctx, 42);
    std::uint64_t joins = 0;
    for (std::uint32_t i = 0; i < shape.n; ++i) joins += shape.in_degree(i) > 1;
    std::printf("  graph: %u nodes, %zu edges, %llu joins (nodes with more than one predecessor), 300 ns bodies;\n"
                "  16 workers, every ready successor pushed rather than run inline, idle workers never sleeping\n",
                shape.n, shape.edges(), static_cast<unsigned long long>(joins));

    std::printf("  1. stress, every input checked. A join tests publication only if its predecessors ran on\n"
                "     different workers, so those are counted:\n");
    std::printf("    %-14s %10s %11s %13s %10s %11s   %s\n", "ordering", "instances", "joins", "cross-worker", "stolen",
                "bad inputs", "correct?");
    for (DecOrder o : orders) {
        Config cfg = stress_config();
        cfg.order  = o;
        Load load = stress_load();
        const Result r = run(ctx, shape, cfg, load, true, {}, 3);
        const bool   correct = o != DecOrder::relaxed && o != DecOrder::release_only && o != DecOrder::acquire_only;
        std::printf("    %-14s %10llu %11llu %13llu %10llu %11llu   %s\n", name(o),
                    static_cast<unsigned long long>(r.st.completed), static_cast<unsigned long long>(r.st.joins),
                    static_cast<unsigned long long>(r.st.cross_joins), static_cast<unsigned long long>(r.st.steals),
                    static_cast<unsigned long long>(r.st.violations + r.st.bad_completions),
                    correct ? "yes" : "NO, and the stress cannot tell");
        if (correct) check(r.st.violations + r.st.bad_completions == 0, std::string(name(o)) + ": every input seen");
    }

    // The cost is measured on the same graph with empty bodies, in the default
    // configuration (inline continuation, a search budget), so that the
    // decrement is as large a share of each task as it ever is.
    const Shape bare = random_dag(10, 16, 8, 0, 42);
    std::printf("  2. what each costs: the same graph with empty bodies, default configuration, checks off;\n"
                "     ns of worker time per task (median of 5, interleaved)\n");
    std::vector<std::vector<double>> ns(std::size(orders));
    for (int rep = 0; rep < 5; ++rep)
        for (std::size_t i = 0; i < std::size(orders); ++i) {
            Config cfg;
            cfg.workers = 16;
            cfg.order   = orders[i];
            Load load;
            load.kind      = Load::closed;
            load.in_flight = 32;
            load.instances = 30'000 / kScale;
            const Result r = run(ctx, bare, cfg, load, false, {}, 5);
            ns[i].push_back(r.wall_s * 1e9 * cfg.workers / static_cast<double>(r.st.tasks));
        }
    const auto self = [](double x) { return x; };
    for (std::size_t i = 0; i < std::size(orders); ++i)
        std::printf("    %-14s %s\n", name(orders[i]), fmt_ns(median_of(ns[i], self)).c_str());
}

}  // namespace keystone::bench
