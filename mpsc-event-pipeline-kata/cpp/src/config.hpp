#pragma once
//
// The pipeline's tuning surface and its readouts, kept together: these are the
// knobs an operator turns and the numbers they watch while turning them.

#include "event.hpp"
#include "platform.hpp"
#include "spsc_ring.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace weir {

// What a producer does when its ring is full. Both terminate: "producers must
// not block indefinitely" is a hard requirement, so the only question is how
// long we try, never whether we eventually give up.
//
// Overwrite-oldest is deliberately absent — it cannot be done safely in a plain
// SPSC ring, because the producer would overwrite a slot the consumer may be
// mid-read. It needs sequence-numbered slots and reader validation, which is a
// different data structure.
enum class FullPolicy {
    DropNewest,     // fail immediately, count it — right when staleness beats stalling
    SpinThenDrop,   // bounded spin first, for bursty traffic the consumer will absorb
};

// How the consumer finds rings with work in them. FullScan is the naive O(N)
// version; Bitmap is the follow-up's answer. Both are kept so the benchmark can
// A/B them in one binary with everything else held identical.
enum class ScanPolicy { FullScan, Bitmap };

// Drain is lossless but bounded by a deadline; Abort discards and returns fast.
enum class StopMode { Drain, Abort };

inline constexpr std::size_t kMaxProducers = 64;    // one bitmap word
inline constexpr std::size_t kRingCapacity = 4096;  // per producer; bounds memory at N x this
inline constexpr std::size_t kDrainBatch   = 512;   // per ring, per pass — fairness cap
inline constexpr std::size_t kStageEvents  = 2048;  // staging buffer, in events

using Ring = SpscRing<Event, kRingCapacity>;

// Everything the pipeline exposes about itself. Each mechanism in the design
// can fail silently, so each one is counted:
//
//   pushed/dropped   drops must never be silent — a pipeline that quietly
//                    discards telemetry is worse than one that fails loudly
//   high_water       deepest a ring ever got; proves memory stayed bounded
//   rings_probed     scan work done, against...
//   passes           ...consumer iterations, giving probes-per-pass: the scan
//                    width the active bitmap exists to shrink
//   rings_empty      how much of that probing found nothing
//   bitmap_writes    writes to the shared line; coalescing is working only if
//                    this stays negligible next to the event count
//   notifies/parks   wakeup traffic — a notify per push would be a syscall
//                    per event, the failure mode worth watching for
struct PipelineStats {
    std::uint64_t pushed = 0, dropped = 0, consumed = 0;
    std::uint64_t rings_probed = 0, rings_empty = 0, passes = 0;
    std::uint64_t bitmap_writes = 0, notifies = 0, parks = 0;
    std::uint64_t high_water = 0;
};

struct PipelineConfig {
    FullPolicy full = FullPolicy::DropNewest;
    ScanPolicy scan = ScanPolicy::Bitmap;
    std::uint32_t spin_before_park = 2000;   // consumer idle spins before yielding
    std::uint32_t push_spin        = 200;    // SpinThenDrop budget
    Nanos park_timeout{std::chrono::microseconds(200)};  // missed-wakeup backstop
};

}  // namespace weir
