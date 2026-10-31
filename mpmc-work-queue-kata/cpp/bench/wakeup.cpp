// Phase 5 — wakeups.
//
//   1. Consumers that really do sleep: 32 x 16 at rates low enough that the queue
//      is empty most of the time. What a push costs when it has to wake someone,
//      how often it has to, and how long the event waits for a consumer to run —
//      with the first version of the event count, which wakes on every push that
//      finds a sleeper, beside the one that allows one wake in flight.
//
//   2. A race engineered to lose a wakeup. One consumer, one producer, an
//      eight-slot ring. Each iteration the consumer finds the queue empty and
//      heads for sleep; a hook announces that, then pauses a random few
//      nanoseconds, and the producer — watching for the announcement — pauses a
//      random few of its own and pushes. Across tens of thousands of iterations
//      some pushes land exactly inside the consumer's announce-then-re-check, and
//      that is where a missing fence shows. Each iteration ends one of three ways:
//      the consumer's re-check saw the event, it slept and was woken, or it slept
//      through the push and only the 2 ms backstop got it up — a lost wakeup.
//
//      Run three times: against the real protocol, with the producer's fence
//      removed (Bug::no_fence), and against the reference outline's protocol
//      (Wake::epoch), which has no fence at all and should not need one. x86
//      performs exactly one reordering, store then load to another address, and
//      the second is that shape; the third never loads before it stores.

#include "arms.hpp"
#include "config.hpp"
#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace turnstile::bench {
namespace {

struct Race {
    int recheck = 0, woken = 0, lost = 0, late = 0;
};

template <Options O>
Race race(const Context& ctx, int iterations, std::uint64_t spread) {
    using Q = Queue<Event, 8, O, ParkHooks>;
    ParkControl                ctl;
    ctl.spread = spread;
    auto                       q = std::make_unique<Q>(std::chrono::milliseconds(2), ParkHooks{&ctl});
    std::atomic<int>           acked{-1};
    std::atomic<std::uint64_t> parking_at_ack{0};  // the consumer's announcement count as it finished
    std::atomic<std::uint64_t> pushed_at{0};
    Race                       out;
    const std::uint64_t        one_ms = ctx.clk.ticks(1e6);

    std::thread consumer([&] {
        OpStats    st;
        Event      ev;
        const auto forever = Clock::now() + std::chrono::hours(1);
        for (int i = 0; i < iterations; ++i) {
            const auto parks = st.parks, backstops = st.backstops;
            if (q->pop(ev, forever, st) != Pop::ok) break;
            const std::uint64_t now = TscClock::now();
            if (st.backstops > backstops) {
                // Slept through it. A lost wakeup if the producer's push — notify and
                // all — had returned long before the backstop fired; merely late if
                // the producer itself was descheduled in between.
                if (now - pushed_at.load(std::memory_order_acquire) > one_ms) ++out.lost;
                else ++out.late;
            } else if (st.parks > parks) {
                ++out.woken;
            } else {
                ++out.recheck;
            }
            // Hand over the announcement count with the acknowledgement: the next
            // announcement may come before the producer looks, and it must not be
            // mistaken for this one.
            parking_at_ack.store(ctl.parking.load(std::memory_order_acquire), std::memory_order_relaxed);
            acked.store(i, std::memory_order_release);
        }
    });

    std::thread producer([&] {
        OpStats       st;
        std::uint64_t rng  = 0x2545f4914f6cdd1dull;
        std::uint64_t seen = 0;
        for (int i = 0; i < iterations; ++i) {
            while (ctl.parking.load(std::memory_order_acquire) == seen) TURNSTILE_PAUSE();
            rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
            spin_iterations(rng % 64);
            q->push(make_event(0, static_cast<std::uint32_t>(i), TscClock::now()), Clock::now() + std::chrono::seconds(5),
                    st);
            pushed_at.store(TscClock::now(), std::memory_order_release);
            while (acked.load(std::memory_order_acquire) < i) std::this_thread::yield();
            seen = parking_at_ack.load(std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();
    return out;
}

void print_race(const char* label, std::uint64_t spread, const Race& r) {
    std::printf("    %-26s %6llu   %9d %9d %9d %9d\n", label, static_cast<unsigned long long>(spread), r.recheck,
                r.woken, r.lost, r.late);
}

}  // namespace

int lost_wakeups_without_fence(const Context& ctx) {
    constexpr int kIterations = 20'000;
    std::printf("  the engineered race with the producer's fence removed, %d iterations per row:\n", kIterations);
    std::printf("    %-26s %6s   %9s %9s %9s %9s\n", "", "spread", "re-check", "woken", "LOST", "late");
    int lost = 0;
    for (const std::uint64_t spread : {64ull, 256ull, 1024ull, 4096ull}) {
        const Race u = race<Options{.bug = Bug::no_fence}>(ctx, kIterations, spread);
        print_race("producer's fence removed", spread, u);
        lost += u.lost;
    }
    return lost;
}

void phase_wakeup(const Context& ctx) {
    // 1. Consumers asleep most of the time, and the price of waking them: a wake
    // on every push that finds a sleeper, against one wake in flight. Interleaved,
    // so the clock is the same for both.
    constexpr int kAB = 3;
    std::printf("  %d x %d, consumers spend %s per event; the queue is empty most of the time; median of %d:\n",
                kProducers, kConsumers, fmt_ns(kConsumerWorkNs).c_str(), kAB);
    std::printf("    %10s  %-20s %10s %10s %9s %9s %9s %9s\n", "rate", "wakes", "wakes/push", "parks/pop",
                "push p50", "p99", "p99.9", "e2e p99");
    for (const double rate : {20e3, 200e3, 1e6}) {
        RunSpec s;
        s.rate             = rate;
        s.seconds          = 1.0;
        s.consumer_work_ns = kConsumerWorkNs;
        s.latency          = true;
        std::vector<RunResult> every, one;
        for (int rep = 0; rep < kAB; ++rep) {
            {
                auto q = std::make_unique<WakeEveryPush>();
                every.push_back(run(*q, s, ctx));
                check_run(every.back(), "sleeping consumers, a wake per push");
            }
            {
                auto q = std::make_unique<Turnstile>();
                one.push_back(run(*q, s, ctx));
                check_run(one.back(), "sleeping consumers, one wake in flight");
            }
        }
        for (const auto& [label, v] : {std::pair{"on every push", &every}, std::pair{"one in flight", &one}}) {
            auto q = [&](double quant) {
                return fmt_ns(median_of(*v, [&](const RunResult& r) { return ctx.clk.to_ns(r.push.quantile(quant)); }));
            };
            std::printf("    %10s  %-20s %10.4f %10.4f %9s %9s %9s %9s\n", fmt_rate(rate).c_str(), label,
                        median_of(*v, [](const RunResult& r) { return double(r.producer_ops.wakes) / double(r.accepted); }),
                        median_of(*v, [](const RunResult& r) { return double(r.consumer_ops.parks) / double(r.consumed); }),
                        q(0.5).c_str(), q(0.99).c_str(), q(0.999).c_str(),
                        fmt_ns(median_of(*v, [&](const RunResult& r) { return ctx.clk.to_ns(r.e2e.quantile(0.99)); }))
                            .c_str());
        }
    }
    std::printf("    a push that wakes someone pays the wake (phase 0); wakes/push is how often that happens\n");

    // 2. The race. The consumer's random pause is swept, because where the
    // producer lands relative to the consumer's re-check depends on it: too short
    // and the producer always arrives after the consumer is asleep (and wakes it),
    // too long and always before (and the re-check sees it). The race lives where
    // both outcomes occur.
    constexpr int kIterations = 20'000;
    std::printf("\n  the engineered race, %d iterations per row, 2 ms backstop:\n", kIterations);
    std::printf("    %-26s %6s   %9s %9s %9s %9s\n", "", "spread", "re-check", "woken", "LOST", "late");
    int fenced_lost = 0, unfenced_lost = 0, epoch_lost = 0;
    for (const std::uint64_t spread : {64ull, 256ull, 1024ull, 4096ull}) {
        const Race f = race<Options{}>(ctx, kIterations, spread);
        print_race("fenced (the real protocol)", spread, f);
        const Race u = race<Options{.bug = Bug::no_fence}>(ctx, kIterations, spread);
        print_race("producer's fence removed", spread, u);
        const Race e = race<Options{.wake = Wake::epoch}>(ctx, kIterations, spread);
        print_race("epoch every push, no fence", spread, e);
        fenced_lost += f.lost;
        unfenced_lost += u.lost;
        epoch_lost += e.lost;
    }
    std::printf("    spread = the consumer's random pause before announcing itself, in loop iterations;\n"
                "    LOST = slept through a push whose notify had returned more than 1 ms before the backstop\n");
    std::printf("    lost wakeups: %d fenced, %d with the producer's fence removed, %d with the outline's epoch\n",
                fenced_lost, unfenced_lost, epoch_lost);
    check(fenced_lost == 0, "with both fences in place, no wakeup is ever lost");
    check(epoch_lost == 0, "an epoch advanced on every push loses no wakeup, with no fence");
}

}  // namespace turnstile::bench
