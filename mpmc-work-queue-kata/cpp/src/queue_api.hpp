#pragma once
//
// What every queue in this project returns and counts, so that the benchmark
// drives all of them through one shape and the arms differ only in the queue.

#include "histogram.hpp"

#include <cstdint>

namespace turnstile {

enum class Push : std::uint8_t { ok, full, closed, timeout };
enum class Pop : std::uint8_t { ok, empty, drained, timeout };

// Per thread, passed in by the caller. Nothing here is shared, so counting costs
// a register increment rather than a contended line.
struct OpStats {
    std::uint64_t retries   = 0;  // lost a CAS, or arrived at a position already taken
    std::uint64_t parks     = 0;
    std::uint64_t backstops = 0;  // parks that ended by the backstop, the epoch never having moved
    std::uint64_t wakes     = 0;  // wakes this thread issued
    std::uint64_t holes     = 0;  // parks taken while a producer held the head reserved
    Histogram*    window    = nullptr;

    void merge(const OpStats& o) noexcept {
        retries += o.retries, parks += o.parks, backstops += o.backstops, wakes += o.wakes, holes += o.holes;
    }
};

}  // namespace turnstile
