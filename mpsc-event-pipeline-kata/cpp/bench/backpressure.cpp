// Phase 3 — backpressure and shutdown.
//
// "Producers must not block indefinitely" and "bounded memory usage" are the
// same requirement seen from two sides. This phase drives the pipeline into
// sustained overload with a sink that cannot keep up, and asserts both.

#include "harness.hpp"
#include "phases.hpp"

#include "event_pipeline.hpp"
#include "sink.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace weir::bench {

void test_backpressure() {
    constexpr std::uint32_t kProducers = 8;
    constexpr std::uint32_t kPerProducer = 100000;

    PipelineConfig cfg;
    cfg.full = FullPolicy::DropNewest;
    cfg.scan = ScanPolicy::Bitmap;
    EventPipeline pipe(cfg);
    SlowSink sink{std::chrono::microseconds(400)};   // a peer that stopped reading
    pipe.start(sink);

    std::atomic<std::int64_t> worst_push_ns{0};
    std::vector<std::thread> ts;
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        ts.emplace_back([&] {
            auto h = pipe.register_producer();
            std::int64_t worst = 0;
            for (std::uint32_t s = 1; s <= kPerProducer; ++s) {
                const auto a = Clock::now();
                h.push(make_event(h.id(), s, 0));
                const auto d = std::chrono::duration_cast<Nanos>(Clock::now() - a).count();
                worst = std::max(worst, d);
            }
            std::int64_t cur = worst_push_ns.load(std::memory_order_relaxed);
            while (worst > cur &&
                   !worst_push_ns.compare_exchange_weak(cur, worst, std::memory_order_relaxed)) {}
        });
    }
    for (auto& t : ts) t.join();

    const auto st = pipe.stats();
    std::printf("  slow sink: pushed=%llu dropped=%llu (%.1f%%) high_water=%llu"
                " worst_push=%.1f us\n",
                static_cast<unsigned long long>(st.pushed),
                static_cast<unsigned long long>(st.dropped),
                100.0 * static_cast<double>(st.dropped) /
                    static_cast<double>(st.pushed + st.dropped),
                static_cast<unsigned long long>(st.high_water),
                static_cast<double>(worst_push_ns.load()) / 1000.0);

    check(st.dropped > 0, "backpressure: a slow sink causes drops rather than blocking");
    check(st.high_water <= kRingCapacity, "backpressure: memory stayed bounded");
    check(worst_push_ns.load() < 500'000'000, "backpressure: no producer blocked indefinitely");

    const auto t0 = Clock::now();
    pipe.stop(StopMode::Abort);
    const auto abort_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("  abort shutdown returned in %.1f ms\n", abort_ms);
    check(abort_ms < 2000.0, "shutdown: Abort returns promptly under load");
}

}  // namespace weir::bench
