// Phase 6 — preemption inside the window.
//
//   1. The window as it really is. A queue built with measure_window records,
//      per push, the time from winning the reservation to storing the
//      publication. On an idle core that is a few dozen nanoseconds — a move and
//      a store. With 48 threads on 32 CPUs some producers are descheduled inside
//      it, and the tail of this distribution is how often step 6 happens on its
//      own, with nothing injected.
//
//   2. A producer stalled inside its window for 100 ms. Consumers reach the hole
//      and stop — the queue reports empty while it fills behind them — and once
//      the ring has wrapped round to the hole, producers start being refused: by
//      arithmetic, N / rate after the stall began. At the brief's 5 M/s that is
//      13.1 ms; measured at 1 M/s, where it is 65.5 ms (see kStudyRate).
//
//   3. A consumer stalled between claiming a slot and releasing it, the same way.
//      The other consumers carry on and the queue stays nearly empty — until the
//      producers have gone once round the ring and reach the stalled slot, which
//      still holds the previous lap. Then the queue reports full with almost
//      nothing in it.

#include "arms.hpp"
#include "config.hpp"
#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace turnstile::bench {
namespace {

using Stalled  = Queue<Event, kCapacity, Options{}, StallHooks>;
using Windowed = Queue<Event, kCapacity, Options{.measure_window = true}>;

// Samples the queue's occupancy — positions reserved but not yet claimed — about
// every 50 us while a run is going, into a timeline allocated up front, so that
// afterwards the occupancy can be read at any moment the run reports.
struct Monitor {
    struct Sample {
        std::uint64_t tsc, occupancy;
    };
    std::atomic<bool>   stop{false};
    std::vector<Sample> samples;
    std::thread         thread;

    template <typename Q>
    void start(const Q& q) {
        samples.reserve(1 << 16);
        thread = std::thread([this, &q] {
            while (!stop.load(std::memory_order_acquire) && samples.size() < samples.capacity()) {
                samples.push_back({TscClock::now(), q.reserved() - q.claimed()});
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }
    void finish() {
        stop.store(true, std::memory_order_release);
        thread.join();
    }
    std::uint64_t peak() const {
        std::uint64_t m = 0;
        for (const auto& x : samples) m = std::max(m, x.occupancy);
        return m;
    }
    // The last sample taken at or before `t`.
    std::uint64_t at(std::uint64_t t) const {
        std::uint64_t v = 0;
        for (const auto& x : samples) {
            if (x.tsc > t) break;
            v = x.occupancy;
        }
        return v;
    }
};

}  // namespace

void phase_preemption(const Context& ctx) {
    // 1. The natural window.
    std::printf("  reserve -> publish, %d x %d, nothing injected:\n", kProducers, kConsumers);
    std::printf("    %-34s %8s %8s %8s %9s %9s   %8s %8s\n", "", "p50", "p99", "p99.99", "max", "pushes", "> 100us",
                "> 1 ms");
    for (const bool saturated : {true, false}) {
        auto    q = std::make_unique<Windowed>();
        RunSpec s;
        if (saturated) {
            s.events = 250'000;
        } else {
            s.rate             = kStudyRate;
            s.seconds          = 1.0;
            s.consumer_work_ns = kConsumerWorkNs;
        }
        const RunResult r = run(*q, s, ctx);
        check_run(r, "window measurement");
        const auto& w  = r.window;
        auto        ns = [&](std::uint64_t t) { return fmt_ns(ctx.clk.to_ns(t)); };
        std::printf("    %-34s %8s %8s %8s %9s %9llu   %8llu %8llu\n",
                    saturated ? "saturated" : "paced at 1 M/s, 1 us consumer work", ns(w.quantile(0.5)).c_str(),
                    ns(w.quantile(0.99)).c_str(), ns(w.quantile(0.9999)).c_str(), ns(w.max()).c_str(),
                    static_cast<unsigned long long>(w.count()),
                    static_cast<unsigned long long>(w.count_above(ctx.clk.ticks(1e5))),
                    static_cast<unsigned long long>(w.count_above(ctx.clk.ticks(1e6))));
    }

    // 2 and 3. One stall, injected, at the study rate.
    const double fill_ms = static_cast<double>(kCapacity) / kStudyRate * 1e3;
    for (const bool producer_side : {true, false}) {
        StallControl ctl;
        ctl.duration = std::chrono::milliseconds(100);
        // About 200 ms into the run.
        (producer_side ? ctl.producer_at : ctl.consumer_at).store(200'000);
        auto    q = std::make_unique<Stalled>(std::chrono::milliseconds(50), StallHooks{&ctl});
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
        check_run(r, producer_side ? "producer stalled in its window" : "consumer stalled in its window");

        const std::uint64_t began = ctl.began.load(), ended = ctl.ended.load();
        auto                ms    = [&](std::uint64_t a, std::uint64_t b) { return ctx.clk.to_ns(b - a) / 1e6; };
        std::printf("\n  a %s stalled for %lld ms inside its window at position 200,000, %s offered, reject on full:\n",
                    producer_side ? "producer" : "consumer", static_cast<long long>(ctl.duration.count()),
                    fmt_rate(kStudyRate).c_str());
        if (began == 0) {
            check(false, "the injected stall happened");
            continue;
        }
        std::printf("    stall began %.1f ms into the run and lasted %.1f ms\n", ms(r.t_go, began), ms(began, ended));
        if (r.first_full > began)
            std::printf("    first refusal %.1f ms after the stall began   (N / rate: %.1f ms);"
                        " occupancy then %llu\n",
                        ms(began, r.first_full), fill_ms, static_cast<unsigned long long>(mon.at(r.first_full)));
        else
            std::printf("    no producer was refused during the stall\n");
        std::printf("    refused %llu events (%.1f %% of %llu offered)\n", static_cast<unsigned long long>(r.rejected),
                    100.0 * double(r.rejected) / double(r.offered), static_cast<unsigned long long>(r.offered));
        std::printf("    peak occupancy %llu of %zu   (reserved, not yet claimed)\n",
                    static_cast<unsigned long long>(mon.peak()), kCapacity);
        std::printf("    end-to-end p99 %s, max %s;  consumers parked %llu times with a hole at the head\n",
                    fmt_ns(ctx.clk.to_ns(r.e2e.quantile(0.99))).c_str(), fmt_ns(ctx.clk.to_ns(r.e2e.max())).c_str(),
                    static_cast<unsigned long long>(r.consumer_ops.holes));
    }
}

}  // namespace turnstile::bench
