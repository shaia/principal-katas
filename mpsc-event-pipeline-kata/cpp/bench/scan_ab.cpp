// Phase 4 — the follow-up's question: does the active bitmap actually improve
// tail latency?
//
// The methodology is as much the answer as the numbers are, so the reasoning
// behind each choice is inline:
//
//   - A/B in one binary, same code, same machine, alternating runs.
//   - Vary exactly one thing (registration), not two (registration + load).
//   - Open-loop load, latency measured from the *intended* send time, because
//     a closed loop hides the very stalls we are looking for.
//   - Percentiles, never means. Repetitions, because a single p99.9 is noise.
//   - Report mechanism counters beside latency, so a tail that moved for an
//     unrelated reason cannot be credited to this change.

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

struct AbResult {
    Percentiles lat;
    double probes_per_pass;     // scan width: what the bitmap actually shrinks
    double empty_ratio;
    double drop_pct;            // if this is non-zero the run was overloaded
    std::uint64_t bitmap_writes, notifies, parks;
};

// Open-loop load generation. Each producer has a fixed schedule of send times
// and latency is measured from the *intended* time, not the actual one. A
// closed loop (push as fast as possible) lets a stalled consumer throttle its
// own producers, which hides exactly the stall we are trying to measure —
// coordinated omission, and it always lies in the flattering direction.
AbResult run_ab(ScanPolicy scan, std::uint32_t active, std::uint32_t idle_registered,
                std::uint32_t rate_per_producer, std::chrono::milliseconds duration) {
    PipelineConfig cfg;
    cfg.scan = scan;
    cfg.full = FullPolicy::DropNewest;
    EventPipeline pipe(cfg);

    const std::size_t cap = static_cast<std::size_t>(rate_per_producer) *
                            static_cast<std::size_t>(duration.count()) / 1000 * active + 1024;
    // Preallocated, and only ever touched by the consumer thread, so the
    // measured path neither allocates nor locks. Sampled 1-in-N because a
    // clock read per event is itself ~25 ns on this platform — at multi-M
    // events/s the instrument would become a meaningful part of the consumer's
    // budget and slow down the thing it is measuring.
    constexpr std::uint64_t kSampleEvery = 4;
    std::vector<std::int64_t> samples;
    samples.reserve(cap / kSampleEvery + 1024);
    std::uint64_t seen = 0;
    pipe.on_event_ = [&](const Event& e) {
        if (++seen % kSampleEvery) return;
        if (e.stamp_ns != 0 && samples.size() < samples.capacity())
            samples.push_back(now_ns() - e.stamp_ns);
    };

    CountingSink sink;
    pipe.start(sink);

    // Registered-but-idle producers: they hold slots the full-scan consumer
    // must probe on every pass, which is the cost the bitmap removes.
    std::vector<ProducerHandle> idle_handles;
    for (std::uint32_t i = 0; i < idle_registered; ++i)
        idle_handles.push_back(pipe.register_producer());

    std::atomic<std::uint32_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (std::uint32_t p = 0; p < active; ++p) {
        ts.emplace_back([&] {
            auto h = pipe.register_producer();
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) WEIR_PAUSE();

            const auto start = Clock::now();
            const auto period = Nanos(1'000'000'000ULL / rate_per_producer);
            std::uint32_t seq = 0;
            for (;;) {
                const auto intended = start + period * seq;
                if (intended - start > duration) break;
                // Spin-paced, not sleep-paced: sleep_for on Windows rounds up
                // to the system timer tick (~1-15 ms), which is three orders of
                // magnitude coarser than the periods we are pacing at and would
                // swamp every latency number below. Spinning costs a core per
                // producer, which is why the caller caps the active count to
                // what the machine actually has.
                while (Clock::now() < intended) WEIR_PAUSE();
                ++seq;
                // The timestamp is the intended time, so a producer that falls
                // behind schedule still reports the full delay.
                h.push(make_event(h.id(), seq,
                                  std::chrono::duration_cast<Nanos>(
                                      intended.time_since_epoch()).count()));
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != active) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    pipe.stop(StopMode::Drain);

    const auto st = pipe.stats();
    // Drop the first 10% as warm-up.
    if (samples.size() > 100) samples.erase(samples.begin(), samples.begin() + samples.size() / 10);

    AbResult r;
    r.lat = percentiles(samples);
    // Scan width: rings inspected per consumer pass. Per-event normalization
    // would be confounded — a cheaper pass lets the consumer spin through more
    // passes per second, so probes-per-event stays flat while the per-pass cost
    // the optimization targets has actually fallen.
    r.probes_per_pass = st.passes ? static_cast<double>(st.rings_probed) /
                                    static_cast<double>(st.passes) : 0.0;
    r.empty_ratio = st.rings_probed ? static_cast<double>(st.rings_empty) /
                                      static_cast<double>(st.rings_probed) : 0.0;
    r.drop_pct = (st.pushed + st.dropped) ? 100.0 * static_cast<double>(st.dropped) /
                                            static_cast<double>(st.pushed + st.dropped) : 0.0;
    r.bitmap_writes = st.bitmap_writes;
    r.notifies = st.notifies;
    r.parks = st.parks;
    return r;
}

}  // namespace

void test_scan_ab() {
    // The variable under test is how many *idle registered* rings the consumer
    // has to wade through, so that is the only thing that varies: the active
    // producer count is pinned at 8 (the follow-up's premise) and registration
    // is swept from 8 to 64. Sweeping the active count instead would change CPU
    // contention and scan width together and confound the two.
    //
    // Open-loop pacing costs a spinning core per active producer, so 8 active
    // plus the consumer has to fit comfortably in the machine, or the numbers
    // describe the scheduler rather than the design.
    const std::uint32_t cores = std::max(4u, std::thread::hardware_concurrency());
    const std::uint32_t active = std::min(8u, std::max(2u, cores / 2));

    constexpr std::uint32_t kRate = 100000;                 // events/s/producer
    constexpr auto kDur = std::chrono::milliseconds(250);
    constexpr int kReps = 5;

    std::printf("  Held fixed: %u active producers at %u events/s each. Varying: how many\n"
                "  idle producers are also registered, i.e. how many empty rings the\n"
                "  full-scan consumer must probe on every pass.\n\n"
                "  Prediction: full-scan probe count grows with registration while the\n"
                "  bitmap stays flat. Whether that shows up in the tail is the actual\n"
                "  question — the mechanism working is not the same as the tail moving.\n\n",
                active, kRate);
    std::printf("  %-10s %-6s | %7s %8s %8s | %11s %8s %6s\n",
                "registered", "scan", "p50 us", "p99 us", "p99.9",
                "probes/pass", "bmp-wr", "drop%");

    for (std::uint32_t registered : {8u, 16u, 32u, 64u}) {
        if (registered < active) continue;
        const std::uint32_t idle = registered - active;
        for (ScanPolicy scan : {ScanPolicy::FullScan, ScanPolicy::Bitmap}) {
            // Several reps: a single-run p99.9 is noise.
            std::vector<AbResult> reps;
            for (int r = 0; r < kReps; ++r) reps.push_back(run_ab(scan, active, idle, kRate, kDur));
            std::printf("  %-10u %-6s | %7.1f %8.1f %8.1f | %11.1f %8.0f %5.1f%%\n",
                        registered, scan == ScanPolicy::Bitmap ? "bitmap" : "full",
                        median_of(reps, [](const AbResult& r) { return r.lat.p50; }),
                        median_of(reps, [](const AbResult& r) { return r.lat.p99; }),
                        median_of(reps, [](const AbResult& r) { return r.lat.p999; }),
                        median_of(reps, [](const AbResult& r) { return r.probes_per_pass; }),
                        median_of(reps, [](const AbResult& r) { return r.bitmap_writes; }),
                        median_of(reps, [](const AbResult& r) { return r.drop_pct; }));
        }
    }

    std::printf("\n  'probes/pass' is rings inspected per consumer pass — the scan width this\n"
                "  optimization exists to shrink. 'bmp-wr' counts writes to the shared bitmap\n"
                "  line over the whole run: coalescing is working only if it stays negligible\n"
                "  next to the event count. A tail that moves while probes/pass stays flat\n"
                "  means the win came from somewhere else and should not be credited here.\n");
}

}  // namespace weir::bench
