// Phase 1 — correctness under stress, and producer-slot churn.
//
// These are the assertions that make the rest of the numbers worth reading: if
// ordering or accounting is wrong, a fast pipeline is just a fast way to be
// wrong.

#include "harness.hpp"
#include "phases.hpp"

#include "event_pipeline.hpp"
#include "sink.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace weir::bench {

void test_correctness(ScanPolicy scan) {
    const char* name = (scan == ScanPolicy::Bitmap) ? "bitmap" : "full-scan";
    constexpr std::uint32_t kProducers = 16;
    constexpr std::uint32_t kPerProducer = 200000;

    PipelineConfig cfg;
    cfg.scan = scan;
    cfg.full = FullPolicy::SpinThenDrop;
    EventPipeline pipe(cfg);

    // Per-producer ordering check, run on the consumer thread.
    std::array<std::uint64_t, kMaxProducers> last{};
    std::array<bool, kMaxProducers> seen{};
    std::atomic<std::uint64_t> order_violations{0};
    std::atomic<std::uint64_t> received{0};
    pipe.on_event_ = [&](const Event& e) {
        const std::uint32_t i = e.producer_id;
        // Gaps are legal (a drop), going backwards is not.
        if (seen[i] && e.seq <= last[i]) order_violations.fetch_add(1, std::memory_order_relaxed);
        seen[i] = true;
        last[i] = e.seq;
        received.fetch_add(1, std::memory_order_relaxed);
    };

    CountingSink sink;
    pipe.start(sink);

    std::vector<std::thread> producers;
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&pipe] {
            auto h = pipe.register_producer();
            if (!h.valid()) return;
            for (std::uint32_t s = 1; s <= kPerProducer; ++s)
                h.push(make_event(h.id(), s, now_ns()));
        });
    }
    for (auto& t : producers) t.join();
    pipe.stop(StopMode::Drain);

    const auto st = pipe.stats();
    std::printf("  [%s] pushed=%llu dropped=%llu received=%llu order_violations=%llu\n",
                name,
                static_cast<unsigned long long>(st.pushed),
                static_cast<unsigned long long>(st.dropped),
                static_cast<unsigned long long>(received.load()),
                static_cast<unsigned long long>(order_violations.load()));

    check(order_violations.load() == 0, std::string(name) + ": per-producer order preserved");
    check(st.pushed + st.dropped == std::uint64_t(kProducers) * kPerProducer,
          std::string(name) + ": every push accounted for (accepted + dropped)");
    check(received.load() == st.pushed,
          std::string(name) + ": drain shutdown lost nothing (received == pushed)");
    check(pipe.drain_completed(), std::string(name) + ": drain finished within deadline");
    check(st.high_water <= kRingCapacity, std::string(name) + ": ring depth stayed bounded");
    check(sink.batches() > 0 && sink.bytes() == received.load() * sizeof(Event),
          std::string(name) + ": sink byte accounting matches");
}

void test_producer_lifetime() {
    // Producers that come and go: slots must be recycled, and a ring must never
    // be reused before the consumer has drained the retired producer's events.
    PipelineConfig cfg;
    EventPipeline pipe(cfg);
    CountingSink sink;
    std::atomic<std::uint64_t> received{0};
    pipe.on_event_ = [&](const Event&) { received.fetch_add(1, std::memory_order_relaxed); };
    pipe.start(sink);

    constexpr int kWaves = 40, kThreads = 8, kPer = 500;
    for (int w = 0; w < kWaves; ++w) {
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&] {
                auto h = pipe.register_producer();
                if (!h.valid()) return;
                for (int s = 1; s <= kPer; ++s) h.push(make_event(h.id(), std::uint32_t(s), 0));
            });   // handle dtor retires the slot here, while the consumer may still be draining
        }
        for (auto& t : ts) t.join();
    }
    pipe.stop(StopMode::Drain);

    const auto st = pipe.stats();
    std::printf("  churn: %d waves x %d producers, pushed=%llu received=%llu\n",
                kWaves, kThreads,
                static_cast<unsigned long long>(st.pushed),
                static_cast<unsigned long long>(received.load()));
    check(received.load() == st.pushed, "lifetime: retired producers' events all drained");
    check(st.dropped == 0, "lifetime: slot recycling did not lose capacity");
}

}  // namespace weir::bench
