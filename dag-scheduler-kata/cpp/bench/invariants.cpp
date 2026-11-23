// Phase 2 — invariants across the configuration matrix.
//
// Every configuration runs random layered DAGs — diamonds everywhere, fan-in up
// to 6, so the last predecessor is a different thread nearly every time — with
// a third of the instances failing at a random interior node. Asserted for
// every instance, by the thread that completes it:
//
//   - every node is resolved exactly once (ran, failed, or skipped);
//   - every body that ran saw every input it read already written, with the
//     value it must have — a read that raced the write would see 0 or a stale
//     generation's value;
//   - no body ran twice;
//   - under poison, a body ran exactly when nothing upstream of it failed; under
//     fail-fast, never when something upstream failed;
//
// and for the run: every instance submitted completed, and nothing hung. Parked
// workers have a 30-second backstop here instead of the usual 100 ms, so that a
// lost wakeup is a hang the watchdog reports rather than a delay nobody sees.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <string>

namespace keystone::bench {

void phase_invariants(const Context& ctx) {
    const Shape shapes[] = {
        random_dag(12, 24, 6, 0, 11),
        random_dag(40, 6, 3, 0, 12),
        random_dag(6, 200, 6, ctx.clk.ticks(200), 13),
    };
    std::printf("  shapes: ");
    for (const auto& s : shapes) std::printf("%u nodes/%zu edges  ", s.n, s.edges());
    std::printf("\n");

    int           configs = 0;
    std::uint64_t instances = 0, tasks = 0, steals = 0, skipped = 0, overflow = 0;
    for (int workers : {1, 4, 32, 64})
        for (bool inl : {false, true})
            for (bool prio : {false, true})
                for (Idle idle : {Idle::greedy, Idle::budgeted})
                    for (FailPolicy fail : {FailPolicy::poison, FailPolicy::cancel}) {
                        Config cfg;
                        cfg.workers             = workers;
                        cfg.inline_continuation = inl;
                        cfg.priority            = prio;
                        cfg.idle                = idle;
                        cfg.fail                = fail;
                        cfg.park_backstop       = std::chrono::seconds(30);
                        // Some configurations get a tiny deque, so that the
                        // overflow path runs as well.
                        cfg.deque_capacity = (configs % 3 == 0) ? 8 : 1 << 14;
                        const Shape& s     = shapes[configs % std::size(shapes)];
                        Load load;
                        load.kind      = Load::closed;
                        load.in_flight = 2 * workers;
                        load.instances = 3000 / kScale;
                        Faults f;
                        f.probability = 0.33;
                        const Result r = run(ctx, s, cfg, load, true, f, static_cast<std::uint64_t>(configs) + 1);
                        const std::string tag = std::to_string(workers) + " workers, " + (inl ? "inline" : "push") +
                                                ", " + (prio ? "priority" : "fifo") + ", " +
                                                (idle == Idle::greedy ? "greedy" : "budgeted") + ", " +
                                                (fail == FailPolicy::poison ? "poison" : "fail-fast");
                        check(r.st.violations == 0, tag + ": every input seen written, no body twice (" +
                                                        std::to_string(r.st.violations) + " violations)");
                        check(r.st.bad_completions == 0, tag + ": exactly-once resolution and failure semantics (" +
                                                             std::to_string(r.st.bad_completions) + " bad)");
                        check(r.st.completed >= load.instances, tag + ": every instance completed");
                        check(r.st.failed_instances > 0, tag + ": the failure path ran");
                        ++configs;
                        instances += r.st.completed;
                        tasks += r.st.tasks;
                        steals += r.st.steals;
                        skipped += r.st.skipped;
                        overflow += r.st.push_full;
                    }
    std::printf("  %d configurations, %llu instances, %llu bodies run, %llu skipped by failure, %llu stolen, "
                "%llu overflowed a full deque\n",
                configs, static_cast<unsigned long long>(instances), static_cast<unsigned long long>(tasks),
                static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(steals),
                static_cast<unsigned long long>(overflow));
    check(overflow > 0, "the full-deque overflow path ran");
    std::printf("  every configuration: exactly-once, inputs seen, failure semantics, no hang — %s\n",
                failure_count() == 0 ? "held" : "SEE FAILURES ABOVE");
}

}  // namespace keystone::bench
