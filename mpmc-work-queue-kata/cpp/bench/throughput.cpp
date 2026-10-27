// Phase 3 — does it meet the brief?
//
// Two questions, measured separately because they are different questions.
//
//   The ceiling: saturated. Every producer pushes as fast as it can and every
//   consumer pops with no work to do. Does a design reach 5 M/s at all, with the
//   brief's 48 threads on this machine's 32 CPUs?
//
//   The promise: at the brief's rate. Producers paced to 5 M/s in aggregate,
//   each consumer spending 1 us of stand-in work per event. The brief's p99 is a
//   claim about a push at that rate; measured at saturation it would only report
//   how full the queue was.
//
// Arms run rep-major — one repetition of each, then the next — so that the
// clock drifting down under sustained load is common to all of them rather than
// charged to whichever ran last. The plugin-dispatch kata's harness records what
// arm-major ordering did there: two identical arms, 27 % apart.

#include "arms.hpp"
#include "config.hpp"
#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace turnstile::bench {
namespace {

template <typename Q>
RunResult once(const Context& ctx, const RunSpec& spec, const std::string& label) {
    auto            q = std::make_unique<Q>();
    const RunResult r = run(*q, spec, ctx);
    check_run(r, label);
    return r;
}

struct Arm {
    std::string                                name;
    std::size_t                                batch;
    std::function<RunResult(const RunSpec&)>   go;
};

double ns(const Context& ctx, std::uint64_t ticks) { return ctx.clk.to_ns(ticks); }

}  // namespace

void phase_throughput(const Context& ctx) {
    // ------------------------------------------------------------ the ceiling
    const std::vector<Arm> saturated = {
        {"turnstile (CAS)", 1, [&](const RunSpec& s) { return once<Turnstile>(ctx, s, "saturated turnstile"); }},
        {"ticket (fetch_add)", 1, [&](const RunSpec& s) { return once<Ticket>(ctx, s, "saturated ticket"); }},
        {"mutex + 2 condvars", 1, [&](const RunSpec& s) { return once<Locked>(ctx, s, "saturated mutex"); }},
        {"mutex, batches of 64", 64, [&](const RunSpec& s) { return once<Locked>(ctx, s, "saturated mutex x64"); }},
    };
    std::vector<std::vector<RunResult>> reps(saturated.size());
    constexpr int kSatReps = 3;
    for (int rep = 0; rep < kSatReps; ++rep)
        for (std::size_t a = 0; a < saturated.size(); ++a) {
            RunSpec s;
            s.events = 250'000;
            s.batch  = saturated[a].batch;
            reps[a].push_back(saturated[a].go(s));
        }

    std::printf("  saturated, %d producers x %d consumers, %d x 250k events, median of %d:\n", kProducers,
                kConsumers, kProducers, kSatReps);
    std::printf("    %-22s %12s  %16s  %16s\n", "arm", "throughput", "retries / push", "retries / pop");
    std::vector<double> ceiling(saturated.size());
    for (std::size_t a = 0; a < saturated.size(); ++a) {
        const auto& v = reps[a];
        ceiling[a]    = median_of(v, [](const RunResult& r) { return r.rate; });
        std::printf("    %-22s %12s  %16.3f  %16.3f\n", saturated[a].name.c_str(), fmt_rate(ceiling[a]).c_str(),
                    median_of(v, [](const RunResult& r) { return double(r.producer_ops.retries) / double(r.accepted); }),
                    median_of(v, [](const RunResult& r) { return double(r.consumer_ops.retries) / double(r.consumed); }));
    }

    // ------------------------------------------------------------ the promise
    struct Config {
        int    producers, consumers;
        double rate;
    };
    const std::vector<Arm> paced = {
        {"turnstile (CAS)", 1, [&](const RunSpec& s) { return once<Turnstile>(ctx, s, "paced turnstile"); }},
        {"ticket (fetch_add)", 1, [&](const RunSpec& s) { return once<Ticket>(ctx, s, "paced ticket"); }},
        {"mutex + 2 condvars", 1, [&](const RunSpec& s) { return once<Locked>(ctx, s, "paced mutex"); }},
    };
    double turnstile_p99_brief = 0;
    for (const Config cfg : {Config{kProducers, kConsumers, kTargetEventsPerSec}, Config{kProducers, kConsumers, 1e6},
                             Config{16, 8, kTargetEventsPerSec}}) {
        constexpr int                       kPacedReps = 3;
        std::vector<std::vector<RunResult>> pr(paced.size());
        for (int rep = 0; rep < kPacedReps; ++rep)
            for (std::size_t a = 0; a < paced.size(); ++a) {
                RunSpec s;
                s.producers        = cfg.producers;
                s.consumers        = cfg.consumers;
                s.rate             = cfg.rate;
                s.seconds          = 1.0;
                s.consumer_work_ns = kConsumerWorkNs;
                s.latency          = true;
                pr[a].push_back(paced[a].go(s));
            }
        std::printf("\n  paced at %s, %d x %d (%d threads on %d CPUs), consumers spend %s per event, median of %d:\n",
                    fmt_rate(cfg.rate).c_str(), cfg.producers, cfg.consumers,
                    cfg.producers + cfg.consumers, logical_cpus(), fmt_ns(kConsumerWorkNs).c_str(), kPacedReps);
        std::printf("    %-20s %9s %9s %9s %9s %9s   %11s %11s  %9s\n", "arm", "push p50", "p99", "p99.9", "p99.99",
                    "max", "ready p99", "e2e p99", "wakes/ev");
        for (std::size_t a = 0; a < paced.size(); ++a) {
            const auto& v = pr[a];
            auto        q = [&](double quant) {
                return fmt_ns(median_of(v, [&](const RunResult& r) { return ns(ctx, r.push.quantile(quant)); }));
            };
            const double p99 = median_of(v, [&](const RunResult& r) { return ns(ctx, r.push.quantile(0.99)); });
            if (a == 0 && cfg.producers == kProducers && cfg.rate == kTargetEventsPerSec) turnstile_p99_brief = p99;
            std::printf("    %-20s %9s %9s %9s %9s %9s   %11s %11s  %9.4f\n", paced[a].name.c_str(), q(0.5).c_str(),
                        q(0.99).c_str(), q(0.999).c_str(), q(0.9999).c_str(),
                        fmt_ns(median_of(v, [&](const RunResult& r) { return ns(ctx, r.push.max()); })).c_str(),
                        fmt_ns(median_of(v, [&](const RunResult& r) { return ns(ctx, r.ready.quantile(0.99)); })).c_str(),
                        fmt_ns(median_of(v, [&](const RunResult& r) { return ns(ctx, r.e2e.quantile(0.99)); })).c_str(),
                        median_of(v, [](const RunResult& r) {
                            return double(r.producer_ops.wakes + r.consumer_ops.wakes) / double(r.accepted);
                        }));
        }
        const double bursts = median_of(pr[0], [](const RunResult& r) { return double(r.bursts); });
        std::printf("    offered %s; each producer woke every %s and pushed %.0f events back to back\n",
                    fmt_rate(median_of(pr[0], [](const RunResult& r) { return double(r.offered); })).c_str(),
                    fmt_ns(1e9 * cfg.producers / bursts).c_str(),
                    median_of(pr[0], [](const RunResult& r) { return double(r.offered); }) / bursts);
        std::printf("    push = one call; ready = from when the event could first have been pushed\n");
    }

    // ------------------------------------------------------------ verdict
    std::printf("\n  against the brief, turnstile at %d x %d:\n", kProducers, kConsumers);
    std::printf("    5 M events/s sustained       %s   (ceiling %s)\n",
                ceiling[0] >= kTargetEventsPerSec ? "met   " : "MISSED", fmt_rate(ceiling[0]).c_str());
    std::printf("    p99 enqueue below 2 us       %s   (p99 %s at 5 M/s)\n",
                turnstile_p99_brief < kP99BudgetNs ? "met   " : "MISSED", fmt_ns(turnstile_p99_brief).c_str());
}

}  // namespace turnstile::bench
