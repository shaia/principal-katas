// Phase 2 — throughput, and what the notification mechanism costs a producer.

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
namespace {

struct ThroughputResult { double offered_mps, accepted_pct, sustained_mps, ns_per_push; };

ThroughputResult throughput_run(std::uint32_t producers, ScanPolicy scan) {
    constexpr std::uint32_t kPerProducer = 500000;
    PipelineConfig cfg;
    cfg.scan = scan;
    EventPipeline pipe(cfg);
    CountingSink sink;
    pipe.start(sink);

    std::atomic<std::uint32_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (std::uint32_t p = 0; p < producers; ++p) {
        ts.emplace_back([&pipe, &ready, &go] {
            auto h = pipe.register_producer();
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) WEIR_PAUSE();
            for (std::uint32_t s = 1; s <= kPerProducer; ++s)
                h.push(make_event(h.id(), s, 0));
        });
    }
    while (ready.load(std::memory_order_acquire) != producers) std::this_thread::yield();
    const auto t0 = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    const auto t1 = Clock::now();
    pipe.stop(StopMode::Drain);
    const auto t2 = Clock::now();

    // Two different windows, deliberately. Offered is measured over the
    // producers' own window; sustained is consumed events over the whole run
    // *including the drain*, because the consumer is still working after the
    // last producer exits. Dividing consumed events by the producer window
    // would credit the pipeline with a rate it never held.
    const double push_secs = std::chrono::duration<double>(t1 - t0).count();
    const double all_secs = std::chrono::duration<double>(t2 - t0).count();
    const auto st = pipe.stats();
    check(st.pushed > 0, "throughput run produced events");
    check(st.consumed == st.pushed, "throughput: consumer drained everything accepted");
    return {static_cast<double>(st.pushed + st.dropped) / push_secs / 1e6,
            100.0 * static_cast<double>(st.pushed) /
                static_cast<double>(st.pushed + st.dropped),
            static_cast<double>(st.consumed) / all_secs / 1e6,
            push_secs * 1e9 / kPerProducer};
}

}  // namespace

void test_throughput() {
    // One untimed run first: the first pipeline in the process pays for page
    // faults on 8 MiB of ring storage and for thread startup, and folding that
    // into the 1-producer number would make it look slower than 4 producers.
    throughput_run(4, ScanPolicy::Bitmap);

    constexpr int kReps = 3;
    auto median_run = [](std::uint32_t p, ScanPolicy s) {
        std::vector<ThroughputResult> v;
        for (int i = 0; i < kReps; ++i) v.push_back(throughput_run(p, s));
        std::sort(v.begin(), v.end(), [](const ThroughputResult& a, const ThroughputResult& b) {
            return a.ns_per_push < b.ns_per_push; });
        return v[v.size() / 2];
    };

    std::printf("  %-11s | %9s %9s %10s | %9s %9s\n",
                "producers", "offered", "accepted", "sustained", "ns/push", "ns/push");
    std::printf("  %-11s | %9s %9s %10s | %9s %9s\n",
                "", "M/s", "%", "M/s", "no-signal", "+bitmap");
    for (std::uint32_t producers : {1u, 4u, 16u, 32u}) {
        // Same workload under both scan policies. FullScan never calls signal(),
        // so the difference in ns/push is what the notification mechanism —
        // the seq_cst fence and the shared-line load — costs a producer.
        const auto plain = median_run(producers, ScanPolicy::FullScan);
        const auto bmp = median_run(producers, ScanPolicy::Bitmap);
        std::printf("  %-11u | %9.1f %8.1f%% %10.1f | %9.1f %9.1f\n",
                    producers, bmp.offered_mps, bmp.accepted_pct, bmp.sustained_mps,
                    plain.ns_per_push, bmp.ns_per_push);
    }
}

}  // namespace weir::bench
