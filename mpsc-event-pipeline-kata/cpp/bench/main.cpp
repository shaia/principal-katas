// mpsc-event-pipeline-kata — high-throughput in-process event pipeline.
//
// Runs the four verification phases in order and exits non-zero if any
// invariant was violated. See ../solution.md for the design rationale.

#include "harness.hpp"
#include "phases.hpp"

#include "config.hpp"
#include "event.hpp"

#include <cstdio>
#include <thread>

int main() {
    using namespace weir;
    using namespace weir::bench;

    std::printf("hardware_concurrency = %u, %zu producer slots x %zu events x %zu B"
                " = %zu KiB of ring storage\n\n",
                std::thread::hardware_concurrency(), kMaxProducers, kRingCapacity, sizeof(Event),
                kMaxProducers * kRingCapacity * sizeof(Event) / 1024);

    std::printf("== 1. correctness under stress ==\n");
    test_correctness(ScanPolicy::FullScan);
    test_correctness(ScanPolicy::Bitmap);
    test_producer_lifetime();

    std::printf("\n== 2. throughput ==\n");
    test_throughput();

    std::printf("\n== 3. backpressure and shutdown ==\n");
    test_backpressure();

    std::printf("\n== 4. follow-up: full scan vs active bitmap ==\n");
    test_scan_ab();

    const int failures = failure_count();
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
