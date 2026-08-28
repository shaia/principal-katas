#pragma once
//
// The four verification phases, one per translation unit. Each prints its own
// results and registers failures via harness check().

#include "config.hpp"

namespace weir::bench {

// 1. Correctness under stress: per-producer ordering, exact accounting, and
//    lossless drain shutdown. Run under both scan policies.
void test_correctness(ScanPolicy scan);

// 1b. Producer churn: slots recycled across many waves, asserting a retired
//     producer's events are never lost to slot reuse.
void test_producer_lifetime();

// 2. Throughput: offered vs sustained rates, and what the notification
//    mechanism costs a producer.
void test_throughput();

// 3. Backpressure and shutdown against a deliberately slow sink.
void test_backpressure();

// 4. The follow-up's A/B: full scan vs active bitmap.
void test_scan_ab();

}  // namespace weir::bench
