#include "driver.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

namespace keystone::bench {
namespace {

struct Shared {
    const Shape*  shape  = nullptr;
    Load          load;
    Faults        faults;
    bool          verify = false;
    Scheduler*    sched  = nullptr;
    std::atomic<std::uint64_t> gen{1};
    std::vector<std::minstd_rand> rng;  // one per worker, for the completion callback

    // closed
    std::atomic<std::uint64_t> issued{0};
    std::atomic<int>           finished{0};
    // open
    std::mutex             mu;
    std::vector<Instance*> free;
};

std::uint32_t pick_fail(std::minstd_rand& rng, const Faults& f, const Shape& s) {
    if (f.probability <= 0) return kNone;
    if (std::uniform_real_distribution<double>(0, 1)(rng) >= f.probability) return kNone;
    if (f.node != kNone) return f.node;
    // The interior: neither the source, which would fail everything, nor the
    // sink, which has nothing downstream to propagate to.
    return 1 + static_cast<std::uint32_t>(rng() % (s.n - 2));
}

void rearm(Shared& sh, Instance& g, std::minstd_rand& rng, std::uint64_t due) {
    g.reset(sh.gen.fetch_add(1, std::memory_order_relaxed), pick_fail(rng, sh.faults, *sh.shape), sh.verify);
    g.due_tsc = due;
}

void on_complete_closed(void* p, Instance& g, int w) {
    auto& sh = *static_cast<Shared*>(p);
    if (sh.issued.fetch_add(1, std::memory_order_relaxed) < sh.load.instances) {
        rearm(sh, g, sh.rng[static_cast<std::size_t>(w)], TscClock::now());
        if (!sh.sched->submit(g)) std::abort();  // the injection ring holds far more than in_flight
    } else {
        sh.finished.fetch_add(1, std::memory_order_release);
    }
}

void on_complete_open(void* p, Instance& g, int) {
    auto&           sh = *static_cast<Shared*>(p);
    std::lock_guard lk(sh.mu);
    sh.free.push_back(&g);
}

}  // namespace

Config stress_config() {
    Config cfg;
    cfg.workers             = 16;
    cfg.inline_continuation = false;
    cfg.idle                = Idle::greedy;
    return cfg;
}

Load stress_load() {
    Load load;
    load.kind      = Load::closed;
    load.in_flight = 2;
    load.instances = 40'000 / kScale;
    return load;
}

Shape stress_shape(const Context& ctx, std::uint64_t seed) { return random_dag(10, 16, 8, ctx.clk.ticks(300), seed); }

Result run(const Context& ctx, const Shape& shape, const Config& cfg, const Load& load, bool verify,
           const Faults& faults, std::uint64_t seed) {
    Shared sh;
    sh.shape  = &shape;
    sh.load   = load;
    sh.faults = faults;
    sh.verify = verify;
    for (int i = 0; i < cfg.workers; ++i) sh.rng.emplace_back(static_cast<unsigned>(seed * 7919 + static_cast<std::uint64_t>(i)));
    std::minstd_rand rng(static_cast<unsigned>(seed));

    const bool closed = load.kind == Load::closed;
    const int  pool   = closed ? load.in_flight : load.pool;
    std::vector<std::unique_ptr<Instance>> instances;
    for (int i = 0; i < pool; ++i) instances.push_back(std::make_unique<Instance>(shape));

    Scheduler sched(cfg, closed ? on_complete_closed : on_complete_open, &sh);
    sh.sched = &sched;

    const double expected_s = closed ? 0 : load.seconds;
    Watchdog     dog("a scheduler run", std::chrono::seconds(hang_seconds() + static_cast<int>(expected_s)));

    Result r;
    r.max_searchers = sched.max_searchers();
    sched.start();
    const double cpu0 = process_cpu_seconds(ctx.clk.ns_per_tick());
    const double me0  = thread_cpu_seconds(ctx.clk.ns_per_tick());
    const auto   t0   = TscClock::now();

    if (closed) {
        sh.issued.store(static_cast<std::uint64_t>(pool), std::memory_order_relaxed);
        for (auto& g : instances) {
            rearm(sh, *g, rng, TscClock::now());
            sched.submit(*g);
        }
        r.submitted = load.instances;
        while (sh.finished.load(std::memory_order_acquire) < pool) std::this_thread::sleep_for(std::chrono::microseconds(200));
    } else {
        for (auto& g : instances) sh.free.push_back(g.get());
        const double interval = 1e9 / load.rate;  // ns
        const auto   start    = TscClock::now() + ctx.clk.ticks(1e6);
        const auto   n        = static_cast<std::uint64_t>(load.rate * load.seconds);
        for (std::uint64_t k = 0; k < n; ++k) {
            const auto due = start + ctx.clk.ticks(interval * static_cast<double>(k));
            // Sleep while the next arrival is far off, spin for the last stretch:
            // a sleep returns up to a timer tick late, and lateness here would be
            // charged to the event as latency it did not suffer in the scheduler.
            for (;;) {
                const auto now = TscClock::now();
                if (now >= due) break;
                if (ctx.clk.to_ns(due - now) > 2.5e6) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                else KEYSTONE_PAUSE();
            }
            Instance* g = nullptr;
            {
                std::lock_guard lk(sh.mu);
                if (!sh.free.empty()) {
                    g = sh.free.back();
                    sh.free.pop_back();
                }
            }
            if (!g) {
                ++r.rejected;
                continue;
            }
            rearm(sh, *g, rng, due);
            ++r.submitted;
            if (!sched.submit(*g)) std::abort();
        }
        for (;;) {
            {
                std::lock_guard lk(sh.mu);
                if (static_cast<int>(sh.free.size()) == pool) break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    const auto t1 = TscClock::now();
    // The submitter's own time is not the scheduler's: it spins between arrivals.
    const double me1 = thread_cpu_seconds(ctx.clk.ns_per_tick());
    const double cpu1 = process_cpu_seconds(ctx.clk.ns_per_tick());
    sched.stop();
    r.st     = sched.totals();
    r.wall_s = ctx.clk.to_ns(t1 - t0) * 1e-9;
    r.cpu_s  = (cpu1 - cpu0) - (me1 - me0);
    return r;
}

}  // namespace keystone::bench
