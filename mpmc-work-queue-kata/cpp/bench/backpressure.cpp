// Phase 4 — full.
//
// Step 4's arithmetic, measured.
//
//   1. No consumers at all. Producers paced, rejecting on full. The queue must
//      accept exactly 65,536 events and refuse every one after — "never overwrite
//      unread events" stated as a count — and the first refusal should arrive
//      after N / rate. At the brief's 5 M/s that is 13.1 ms; measured here at
//      1 M/s, where it is 65.5 ms, because the division is the claim and this
//      machine cannot hold 5 M/s with 48 threads once its clock drops.
//
//   2. A consumer deficit: sixteen consumers that can sustain at most 1.6 M/s,
//      against 2 M/s offered. With reject, when refusals begin and what fraction
//      is refused. With block, what p99 enqueue becomes once the queue is full — a
//      number about the consumers, which no queue design moves.
//
//   3. Blocking producers, and who pays to wake them. Consumers that wake a
//      blocked producer on every release, against once per 64, run side by side
//      under the same deficit.

#include "arms.hpp"
#include "config.hpp"
#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace turnstile::bench {
namespace {

constexpr double kDeficitRate   = 2.0e6;
constexpr double kDeficitWorkNs = 16.0 / 1.6e6 * 1e9;  // 10 us: sixteen consumers top out at 1.6 M/s

RunSpec deficit(PushMode mode) {
    RunSpec s;
    s.rate             = kDeficitRate;
    s.seconds          = 1.0;
    s.consumer_work_ns = kDeficitWorkNs;
    s.mode             = mode;
    s.latency          = true;
    s.block_timeout    = std::chrono::seconds(10);
    return s;
}

}  // namespace

void phase_backpressure(const Context& ctx) {
    auto ms_after_go = [&](const RunResult& r, std::uint64_t t) { return ctx.clk.to_ns(t - r.t_go) / 1e6; };
    auto ns          = [&](std::uint64_t t) { return fmt_ns(ctx.clk.to_ns(t)); };
    const double fill_ms = static_cast<double>(kCapacity) / kStudyRate * 1e3;

    // 1. Nobody consuming.
    {
        auto    q = std::make_unique<Turnstile>();
        RunSpec s;
        s.consumers = 0;
        s.rate      = kStudyRate;
        s.seconds   = 0.15;
        s.mode      = PushMode::reject;
        const RunResult r = run(*q, s, ctx);
        std::printf("  no consumers, %s offered for 150 ms, reject on full:\n", fmt_rate(kStudyRate).c_str());
        std::printf("    accepted %llu, refused %llu; first refusal %.1f ms after the start (N / rate = %.1f ms;"
                    " at 5 M/s, %.1f ms)\n",
                    static_cast<unsigned long long>(r.accepted), static_cast<unsigned long long>(r.rejected),
                    r.first_full ? ms_after_go(r, r.first_full) : 0.0, fill_ms, kCapacityMs);
        check(r.accepted == kCapacity, "with no consumer, exactly 65,536 events are accepted — none overwritten");
        check(r.offered == r.accepted + r.rejected, "no consumers: offered == accepted + rejected");
    }

    // 2. A deficit of 0.4 M/s.
    const double deficit_fill_ms = static_cast<double>(kCapacity) / (kDeficitRate - 1.6e6) * 1e3;
    for (const PushMode mode : {PushMode::reject, PushMode::block}) {
        auto            q = std::make_unique<Turnstile>();
        const RunResult r = run(*q, deficit(mode), ctx);
        check_run(r, mode == PushMode::reject ? "deficit, reject" : "deficit, block");
        std::printf("\n  consumer deficit: %s offered for 1 s, consumers spend %s per event (at most 1.60 M/s), %s:\n",
                    fmt_rate(kDeficitRate).c_str(), fmt_ns(kDeficitWorkNs).c_str(),
                    mode == PushMode::reject ? "reject on full" : "block on full");
        if (mode == PushMode::reject)
            std::printf("    first refusal %.0f ms after the start (a 0.4 M/s deficit fills 65,536 slots in %.0f ms)\n",
                        r.first_full ? ms_after_go(r, r.first_full) : 0.0, deficit_fill_ms);
        std::printf("    accepted %llu, refused %llu (%.1f %%); consumed at %s over %.2f s\n",
                    static_cast<unsigned long long>(r.accepted), static_cast<unsigned long long>(r.rejected),
                    100.0 * double(r.rejected) / double(r.offered), fmt_rate(r.rate).c_str(), r.seconds);
        std::printf("    push p50 %s, p99 %s, max %s\n", ns(r.push.quantile(0.5)).c_str(),
                    ns(r.push.quantile(0.99)).c_str(), ns(r.push.max()).c_str());
    }

    // 3. Waking blocked producers: per release, or per 64.
    constexpr int          kAB = 3;
    std::vector<RunResult> every, batched;
    for (int rep = 0; rep < kAB; ++rep) {
        {
            auto q = std::make_unique<WakeEveryRelease>();
            every.push_back(run(*q, deficit(PushMode::block), ctx));
            check_run(every.back(), "block, wake every release");
        }
        {
            auto q = std::make_unique<Turnstile>();
            batched.push_back(run(*q, deficit(PushMode::block), ctx));
            check_run(batched.back(), "block, wake every 64 releases");
        }
    }
    std::printf("\n  blocked producers under that deficit, and who wakes them — interleaved, median of %d:\n", kAB);
    std::printf("    %-30s %12s %16s %16s %10s\n", "consumers wake a producer", "consumed", "consumer wakes",
                "producer parks", "push p99");
    for (const auto& [label, v] : {std::pair{"on every release", &every}, std::pair{"once per 64 releases", &batched}}) {
        std::printf("    %-30s %12s %16.0f %16.0f %10s\n", label,
                    fmt_rate(median_of(*v, [](const RunResult& r) { return r.rate; })).c_str(),
                    median_of(*v, [](const RunResult& r) { return double(r.consumer_ops.wakes); }),
                    median_of(*v, [](const RunResult& r) { return double(r.producer_ops.parks); }),
                    fmt_ns(median_of(*v, [&](const RunResult& r) { return ctx.clk.to_ns(r.push.quantile(0.99)); }))
                        .c_str());
    }
}

}  // namespace turnstile::bench
