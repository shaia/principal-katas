// Phase 9 — the reference outline's alternatives, beside the queue.
//
// The outline that came with the question (../../reference-outline.md) agrees
// with this answer on the slot protocol, and differs on four things this phase
// runs, each beside the existing arms in one process, rep-major:
//
//   1. At the brief's rate, and saturated: its queue (Outline — a CAS
//      reservation, and an epoch advanced and woken on every push), the
//      turnstile with the event count, the ticket, and the experiment it
//      proposes — one SPSC ring per producer (PerProducer). With the dequeue
//      latency its measurement list asks for and phases 3–8 do not report.
//
//   2. Asleep most of the time: what each way of sleeping costs a push and an
//      event — the event count, the epoch, spinning (its tradeoff table's other
//      row), and a sleeper per consumer.
//
//   3. Idle: how much CPU each keeps busy with nothing to do, the other number
//      its list asks for.
//
//   4. Where one ring per producer should break, and where it should win: a
//      stalled producer, a stalled consumer, a hot source within capacity, and a
//      flood from one source.

#include "arms.hpp"
#include "config.hpp"
#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace turnstile::bench {
namespace {

constexpr int kAB = 3;

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

template <typename Q>
Arm arm(const Context& ctx, const std::string& name) {
    return {name, [&ctx, name](const RunSpec& s) { return once<Q>(ctx, s, name); }};
}

// One repetition of every arm, then the next, so drift is common to all of them.
std::vector<std::vector<RunResult>> interleaved(const std::vector<Arm>& arms, const RunSpec& s) {
    std::vector<std::vector<RunResult>> out(arms.size());
    for (int rep = 0; rep < kAB; ++rep)
        for (std::size_t a = 0; a < arms.size(); ++a) out[a].push_back(arms[a].go(s));
    return out;
}

// --------------------------------------------------------------- idle

struct Idle {
    double cpus = 0, parks_per_sec = 0;
};

// C consumers in pop with nothing to pop, for a second, after 200 ms to settle.
template <typename Q>
Idle idle(const Context& ctx) {
    auto                     q = std::make_unique<Q>();
    std::vector<OpStats>     stats(kConsumers);
    std::vector<std::thread> threads;
    const auto               start = Clock::now();
    for (int id = 0; id < kConsumers; ++id)
        threads.emplace_back([&, id] {
            Event      ev;
            const auto forever = Clock::now() + std::chrono::hours(1);
            while (detail::pop_one(*q, ev, forever, stats[static_cast<std::size_t>(id)], id) != Pop::drained) {}
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double c0 = process_cpu_seconds(ctx.clk.ns_per_tick());
    const auto   t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const double c1   = process_cpu_seconds(ctx.clk.ns_per_tick());
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    q->close();
    for (auto& t : threads) t.join();
    const double  lifetime = std::chrono::duration<double>(Clock::now() - start).count();
    std::uint64_t parks    = 0;
    for (const auto& s : stats) parks += s.parks;
    return {(c1 - c0) / wall, static_cast<double>(parks) / lifetime};
}

// --------------------------------------------------------------- stalls

using StalledShared  = Queue<Event, kCapacity, Options{}, StallHooks>;
using StalledSharded = ShardedQueue<Event, kCapacity, kProducers, kConsumers, StallHooks>;

template <typename Q>
void stalled(const Context& ctx, const char* label, bool producer_side) {
    StallControl ctl;
    ctl.duration = std::chrono::milliseconds(100);
    (producer_side ? ctl.producer_at : ctl.consumer_at).store(200'000);
    auto    q = std::make_unique<Q>(std::chrono::milliseconds(50), StallHooks{&ctl});
    RunSpec s;
    s.rate             = kStudyRate;
    s.seconds          = 0.6;
    s.consumer_work_ns = kConsumerWorkNs;
    s.mode             = PushMode::reject;
    s.latency          = true;
    Monitor mon;
    mon.start(*q);
    const RunResult r = run(*q, s, ctx);
    mon.finish();
    const char* side = producer_side ? "producer" : "consumer";
    check_run(r, std::string(label) + ", a " + side + " stalled");
    const std::uint64_t began = ctl.began.load();
    check(began != 0, std::string(label) + ": the injected stall happened");
    std::string first = "none", occupancy = "-";
    if (began != 0 && r.first_full > began) {
        first     = fmt_ns(ctx.clk.to_ns(r.first_full - began));
        occupancy = std::to_string(mon.at(r.first_full));
    }
    std::printf("    %-9s %-22s %13s %10zu of %-3d %10s %9.1f %% %12llu  %10s\n", side, label, first.c_str(),
                r.producers_refused, s.producers, occupancy.c_str(), 100.0 * double(r.rejected) / double(r.offered),
                static_cast<unsigned long long>(r.hot_rejected), fmt_ns(ctx.clk.to_ns(r.e2e.quantile(0.99))).c_str());
}

// --------------------------------------------------------------- skew

void skewed(const Context& ctx, const std::vector<Arm>& arms, const char* title, double rate, double hot_share,
            double work_ns) {
    RunSpec s;
    s.rate             = rate;
    s.hot_share        = hot_share;
    s.seconds          = 0.5;
    s.consumer_work_ns = work_ns;
    s.mode             = PushMode::reject;
    s.latency          = true;
    const auto v       = interleaved(arms, s);
    std::printf("\n  %s: %s offered, source 0 sends %.0f %% of it, consumers spend %s per event"
                " (%s across %d of them), reject on full, median of %d:\n",
                title, fmt_rate(rate).c_str(), 100 * hot_share, fmt_ns(work_ns).c_str(),
                fmt_rate(1e9 / work_ns * kConsumers).c_str(), kConsumers, kAB);
    std::printf("    %-22s %16s %16s %18s %11s %16s %10s %10s\n", "arm", "source 0 refused", "others refused",
                "producers refused", "consumed", "busiest consumer", "e2e p99", "ready p99");
    for (std::size_t a = 0; a < arms.size(); ++a) {
        const auto& r = v[a];
        std::printf("    %-22s %14.1f %% %14.1f %% %11.0f of %-3d %11s %16s %10s %10s\n", arms[a].name.c_str(),
                    median_of(r, [](const RunResult& x) { return 100.0 * double(x.hot_rejected) / double(x.hot_offered); }),
                    median_of(r,
                              [](const RunResult& x) {
                                  return 100.0 * double(x.rejected - x.hot_rejected) /
                                         double(x.offered - x.hot_offered);
                              }),
                    median_of(r, [](const RunResult& x) { return double(x.producers_refused); }), kProducers,
                    fmt_rate(median_of(r, [](const RunResult& x) { return x.rate; })).c_str(),
                    fmt_rate(median_of(r, [](const RunResult& x) { return x.max_consumer_rate; })).c_str(),
                    fmt_ns(median_of(r, [&](const RunResult& x) { return ctx.clk.to_ns(x.e2e.quantile(0.99)); })).c_str(),
                    fmt_ns(median_of(r, [&](const RunResult& x) { return ctx.clk.to_ns(x.ready.quantile(0.99)); }))
                        .c_str());
    }
}

}  // namespace

void phase_outline(const Context& ctx) {
    auto ns = [&](std::uint64_t t) { return ctx.clk.to_ns(t); };

    const std::vector<Arm> four = {
        arm<Outline>(ctx, "outline (CAS + epoch)"),
        arm<Turnstile>(ctx, "turnstile (CAS)"),
        arm<Ticket>(ctx, "ticket (fetch_add)"),
        arm<PerProducer>(ctx, "per-producer SPSC"),
    };

    // ------------------------------------------------ 1. at the brief's rate
    {
        RunSpec s;
        s.rate             = kTargetEventsPerSec;
        s.seconds          = 1.0;
        s.consumer_work_ns = kConsumerWorkNs;
        s.latency          = true;
        s.pop_latency      = true;
        const auto v       = interleaved(four, s);
        std::printf("  paced at %s, %d x %d, consumers spend %s per event, median of %d:\n",
                    fmt_rate(s.rate).c_str(), kProducers, kConsumers, fmt_ns(kConsumerWorkNs).c_str(), kAB);
        std::printf("    %-22s %9s %9s %9s   %9s %9s   %10s %10s  %10s\n", "arm", "push p50", "p99", "p99.9",
                    "pop p50", "p99", "ready p99", "e2e p99", "wakes/push");
        for (std::size_t a = 0; a < four.size(); ++a) {
            const auto& r = v[a];
            auto        q = [&](double quant, Histogram RunResult::*h) {
                return fmt_ns(median_of(r, [&](const RunResult& x) { return ns((x.*h).quantile(quant)); }));
            };
            std::printf("    %-22s %9s %9s %9s   %9s %9s   %10s %10s  %10.4f\n", four[a].name.c_str(),
                        q(0.5, &RunResult::push).c_str(), q(0.99, &RunResult::push).c_str(),
                        q(0.999, &RunResult::push).c_str(), q(0.5, &RunResult::pop).c_str(),
                        q(0.99, &RunResult::pop).c_str(), q(0.99, &RunResult::ready).c_str(),
                        q(0.99, &RunResult::e2e).c_str(),
                        median_of(r, [](const RunResult& x) { return double(x.producer_ops.wakes) / double(x.accepted); }));
        }
        std::printf("    pop = one non-blocking pop that found an event; wakes/push = wake calls a push made\n");
    }

    // ------------------------------------------------ saturated
    {
        RunSpec s;
        s.events     = 250'000;
        const auto v = interleaved(four, s);
        std::printf("\n  saturated, %d x %d, %d x 250k events, median of %d:\n", kProducers, kConsumers, kProducers,
                    kAB);
        std::printf("    %-22s %12s  %16s  %16s\n", "arm", "throughput", "retries / push", "retries / pop");
        for (std::size_t a = 0; a < four.size(); ++a) {
            const auto& r = v[a];
            std::printf("    %-22s %12s  %16.3f  %16.3f\n", four[a].name.c_str(),
                        fmt_rate(median_of(r, [](const RunResult& x) { return x.rate; })).c_str(),
                        median_of(r, [](const RunResult& x) { return double(x.producer_ops.retries) / double(x.accepted); }),
                        median_of(r, [](const RunResult& x) { return double(x.consumer_ops.retries) / double(x.consumed); }));
        }
    }

    // ------------------------------------------------ 2. asleep most of the time
    const std::vector<Arm> sleepers = {
        arm<Turnstile>(ctx, "event count"),
        arm<Outline>(ctx, "epoch, every push"),
        arm<Spinning>(ctx, "spin, never sleep"),
        arm<PerProducer>(ctx, "per-producer SPSC"),
    };
    std::printf("\n  %d x %d, consumers spend %s per event, the queue empty most of the time; the first three"
                " reserve by CAS; median of %d:\n",
                kProducers, kConsumers, fmt_ns(kConsumerWorkNs).c_str(), kAB);
    std::printf("    %10s  %-22s %10s %10s %9s %9s %9s %9s\n", "rate", "consumers wait by", "wakes/push", "parks/pop",
                "push p50", "p99", "e2e p50", "e2e p99");
    for (const double rate : {20e3, 200e3}) {
        RunSpec s;
        s.rate             = rate;
        s.seconds          = 1.0;
        s.consumer_work_ns = kConsumerWorkNs;
        s.latency          = true;
        const auto v       = interleaved(sleepers, s);
        for (std::size_t a = 0; a < sleepers.size(); ++a) {
            const auto& r = v[a];
            auto        q = [&](double quant, Histogram RunResult::*h) {
                return fmt_ns(median_of(r, [&](const RunResult& x) { return ns((x.*h).quantile(quant)); }));
            };
            std::printf("    %10s  %-22s %10.4f %10.4f %9s %9s %9s %9s\n", fmt_rate(rate).c_str(),
                        sleepers[a].name.c_str(),
                        median_of(r, [](const RunResult& x) { return double(x.producer_ops.wakes) / double(x.accepted); }),
                        median_of(r, [](const RunResult& x) { return double(x.consumer_ops.parks) / double(x.consumed); }),
                        q(0.5, &RunResult::push).c_str(), q(0.99, &RunResult::push).c_str(),
                        q(0.5, &RunResult::e2e).c_str(), q(0.99, &RunResult::e2e).c_str());
        }
    }

    // ------------------------------------------------ 3. idle
    {
        // The instrument first: a thread kept busy for a known time.
        const double c0 = process_cpu_seconds(ctx.clk.ns_per_tick());
        burn(ctx.clk.ticks(200e6));
        const double c1 = process_cpu_seconds(ctx.clk.ns_per_tick());
        std::printf("\n  idle: %d consumers waiting, no producers, for 1 s; CPU time by process cycle accounting"
                    " (one thread busy for 200 ms reads %.1f ms):\n",
                    kConsumers, (c1 - c0) * 1e3);
        std::printf("    %-22s %12s %12s\n", "consumers wait by", "CPUs busy", "parks / s");
        const std::pair<const char*, Idle> rows[] = {
            {"event count", idle<Turnstile>(ctx)},
            {"epoch, every push", idle<Outline>(ctx)},
            {"spin, never sleep", idle<Spinning>(ctx)},
            {"per-producer SPSC", idle<PerProducer>(ctx)},
        };
        for (const auto& [label, r] : rows) std::printf("    %-22s %12.4f %12.0f\n", label, r.cpus, r.parks_per_sec);
        std::printf("    with nothing to wake them, every park ends at the 50 ms backstop\n");
    }

    // ------------------------------------------------ 4. where one ring per producer breaks, and wins
    std::printf("\n  one thread stalled for 100 ms at position 200,000, %s offered, consumers spend %s per event,"
                " reject on full:\n",
                fmt_rate(kStudyRate).c_str(), fmt_ns(kConsumerWorkNs).c_str());
    std::printf("    %-9s %-22s %13s %17s %10s %11s %12s  %10s\n", "stalled", "arm", "first refusal", "producers refused",
                "occupancy", "refused", "by source 0", "e2e p99");
    for (const bool producer_side : {true, false}) {
        stalled<StalledShared>(ctx, "turnstile (CAS)", producer_side);
        stalled<StalledSharded>(ctx, "per-producer SPSC", producer_side);
    }
    std::printf("    first refusal is from the start of the stall (N / rate: %s); occupancy is at that moment;\n"
                "    the stall is in source 0, or in the consumer that owns its ring\n",
                fmt_ns(static_cast<double>(kCapacity) / kStudyRate * 1e9).c_str());

    const std::vector<Arm> two = {
        arm<Turnstile>(ctx, "turnstile (CAS)"),
        arm<PerProducer>(ctx, "per-producer SPSC"),
    };
    skewed(ctx, two, "a hot source, within capacity", kStudyRate, 0.5, 4000);
    skewed(ctx, two, "a flood from one source, past capacity", 8e6, 15.0 / 16.0, 4000);
}

}  // namespace turnstile::bench
