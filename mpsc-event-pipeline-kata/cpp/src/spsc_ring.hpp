#pragma once
//
// Bounded single-producer / single-consumer ring buffer.
//
// This is where the memory ordering lives. Two release/acquire pairs, one per
// direction, and no atomic read-modify-write on either side.

#include "platform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace weir {

// One producer, one consumer, no atomic RMW on either side.
//
// Indices are free-running (never wrapped); only the slot lookup masks. That
// makes full/empty unambiguous without burning a slot: size == tail - head.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && std::has_single_bit(Capacity),
                  "power of two so the mask replaces a modulo");
    static constexpr std::size_t kMask = Capacity - 1;

public:
    // --- producer side -----------------------------------------------------

    bool try_push(const T& value) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);  // sole writer
        if (t - cached_head_ == Capacity) {
            // Believed full. Only now pay for the consumer's line; acquire pairs
            // with the consumer's release store of head_, so a slot it has
            // finished reading is safe for us to overwrite.
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t - cached_head_ == Capacity) return false;
        }
        slots_[t & kMask] = value;
        // Release: publishes the slot write to whoever acquires tail_.
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Producer's own view of occupancy. Cheap and slightly stale (cached_head_
    // may lag), which is fine for the high-water statistic. Producer-only: the
    // cached index is private to that thread.
    std::uint64_t producer_size_hint() const noexcept {
        return tail_.load(std::memory_order_relaxed) - cached_head_;
    }

    // --- consumer side -----------------------------------------------------

    // Copies out at most max_items and frees their slots with a single release
    // store, so a 512-event drain costs one cross-core write, not 512.
    std::size_t pop_batch(T* out, std::size_t max_items) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);  // sole writer
        if (h == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (h == cached_tail_) return 0;
        }
        const std::size_t n = std::min<std::size_t>(max_items, cached_tail_ - h);
        for (std::size_t i = 0; i < n; ++i) out[i] = slots_[(h + i) & kMask];
        // Release: the slot reads above must not sink past this store, or the
        // producer could overwrite a slot we are still reading. This is the
        // pairing people forget — it is not just a liveness hint.
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // Authoritative emptiness check for the consumer. Refreshes the cache, so
    // it is the right call to use after clearing an active-bitmap bit.
    bool empty_now() noexcept {
        cached_tail_ = tail_.load(std::memory_order_acquire);
        return head_.load(std::memory_order_relaxed) == cached_tail_;
    }

    // --- observer side -----------------------------------------------------

    // Safe from a thread that is neither the producer nor the consumer: it
    // reads only the two atomics, never the cached copies (those are private
    // to their owning thread, and reading them from elsewhere is a race).
    std::uint64_t size_now() const noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        return t - h;
    }

private:
    // Three separate lines: the producer writes tail_, the consumer writes
    // head_, and neither should ever invalidate the other's line or the slots.
    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t cached_head_{0};   // producer-private, non-atomic on purpose

    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
    std::uint64_t cached_tail_{0};   // consumer-private

    alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

}  // namespace weir
