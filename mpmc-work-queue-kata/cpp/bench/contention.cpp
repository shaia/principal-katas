// Phase 7 — contention and fairness.
//
// Step 7 asks for arithmetic rather than adjectives. Three measurements supply
// it:
//
//   1. Scaling: retries per operation and throughput as producers are added,
//      for the CAS reservation (look, then claim), the same with backoff, and
//      the ticket (claim, then look). Phase 0 priced one contended
//      read-modify-write; this is how many each event actually pays.
//
//   2. Fairness under oversubscription: 48 threads on 32 CPUs, saturated. The
//      spread between the fastest and slowest producer, and the push latency
//      tail. A CAS loop can lose repeatedly; a ticket holder waits for its turn.
//
//   3. Layout: one slot per cache line against two slots sharing one. Padding
//      costs 65,536 × 24 bytes; sharing puts two producers' writes, and two
//      consumers' reads, on one line.

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
    std::string                              name;
    std::function<RunResult(const RunSpec&)> go;
};

}  // namespace

void phase_contention(const Context& ctx) {
    const std::vector<Arm> arms = {
        {"CAS", [&](const RunSpec& s) { return once<Turnstile>(ctx, s, "contention CAS"); }},
        {"CAS + backoff", [&](const RunSpec& s) { return once<Backoff>(ctx, s, "contention CAS + backoff"); }},
        {"ticket", [&](const RunSpec& s) { return once<Ticket>(ctx, s, "contention ticket"); }},
    };

    // 1. Scaling.
    constexpr int kScaleReps = 3;
    std::printf("  saturated, 2M events per run, consumers = producers / 2, median of %d:\n", kScaleReps);
    std::printf("    %10s", "producers");
    for (const auto& a : arms) std::printf("  %24s", (a.name + ": rate, retries").c_str());
    std::printf("\n");
    for (int k : {1, 2, 4, 8, 16, 32}) {
        std::vector<std::vector<RunResult>> reps(arms.size());
        for (int rep = 0; rep < kScaleReps; ++rep)
            for (std::size_t a = 0; a < arms.size(); ++a) {
                RunSpec s;
                s.producers = k;
                s.consumers = std::max(1, k / 2);
                s.events    = 2'000'000 / static_cast<std::uint64_t>(k);
                reps[a].push_back(arms[a].go(s));
            }
        std::printf("    %10d", k);
        for (std::size_t a = 0; a < arms.size(); ++a)
            std::printf("  %15s  %6.2f", fmt_rate(median_of(reps[a], [](const RunResult& r) { return r.rate; })).c_str(),
                        median_of(reps[a], [](const RunResult& r) {
                            return double(r.producer_ops.retries) / double(r.accepted);
                        }));
        std::printf("\n");
    }
    std::printf("    retries = per push: CAS failures plus arrivals at a position another producer already took\n");

    // 2. Fairness, oversubscribed.
    std::printf("\n  fairness: %d x %d saturated, 250k events per producer, latency recorded, median of 3:\n",
                kProducers, kConsumers);
    std::printf("    %-14s %11s %11s %9s   %9s %9s %9s %9s\n", "arm", "slowest", "fastest", "spread", "push p50",
                "p99", "p99.99", "max");
    std::vector<std::vector<RunResult>> fair(arms.size());
    for (int rep = 0; rep < 3; ++rep)  // rep-major, like every other comparison here
        for (std::size_t a = 0; a < arms.size(); ++a) {
            RunSpec s;
            s.events  = 250'000;
            s.latency = true;
            fair[a].push_back(arms[a].go(s));
        }
    for (std::size_t ai = 0; ai < arms.size(); ++ai) {
        const auto&  a  = arms[ai];
        const auto&  v  = fair[ai];
        const double lo = median_of(v, [](const RunResult& r) { return r.min_producer_rate; });
        const double hi = median_of(v, [](const RunResult& r) { return r.max_producer_rate; });
        auto         q  = [&](double quant) {
            return fmt_ns(median_of(v, [&](const RunResult& r) { return ctx.clk.to_ns(r.push.quantile(quant)); }));
        };
        std::printf("    %-14s %11s %11s %8.2fx   %9s %9s %9s %9s\n", a.name.c_str(), fmt_rate(lo).c_str(),
                    fmt_rate(hi).c_str(), hi / lo, q(0.5).c_str(), q(0.99).c_str(), q(0.9999).c_str(),
                    fmt_ns(median_of(v, [&](const RunResult& r) { return ctx.clk.to_ns(r.push.max()); })).c_str());
    }
    std::printf("    per-producer rate = its events / the time it took to push them all\n");

    // 3. Layout.
    std::printf("\n  layout: %zu-byte slots (one per line) against %zu-byte slots (packed), saturated, median of %d:\n",
                Turnstile::kSlotBytes, Packed::kSlotBytes, kReps);
    std::printf("    ring footprint %.1f MiB padded, %.1f MiB packed\n",
                double(Turnstile::kSlotBytes * kCapacity) / (1 << 20), double(Packed::kSlotBytes * kCapacity) / (1 << 20));
    std::vector<RunResult> pad, pack;
    for (int rep = 0; rep < kReps; ++rep) {
        RunSpec s;
        s.events = 250'000;
        pad.push_back(once<Turnstile>(ctx, s, "layout padded"));
        pack.push_back(once<Packed>(ctx, s, "layout packed"));
    }
    std::printf("    padded %s, packed %s\n", fmt_rate(median_of(pad, [](const RunResult& r) { return r.rate; })).c_str(),
                fmt_rate(median_of(pack, [](const RunResult& r) { return r.rate; })).c_str());
}

}  // namespace turnstile::bench
