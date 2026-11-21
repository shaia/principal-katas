#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace dag {
// Adapted from Le et al., PPoPP 2013, Figure 1 (portable C11 variant).
// One owner; many thieves. Slots are atomic because a losing thief can read a
// reused slot. Never dereference a candidate until the reservation CAS wins.
// Fixed storage removes array reclamation. Indices never wrap: push refuses
// near the limit and the executor uses its intrusive overflow inbox instead.
template<class T, std::size_t N = 4096, bool Sequential = false>
class Deque {
    static_assert(N > 1 && (N & (N - 1)) == 0);
    alignas(64) std::atomic<std::int64_t> top_{1};
    alignas(64) std::atomic<std::int64_t> bottom_{1};
    std::array<std::atomic<T*>, N> slots_{};
    static constexpr auto relaxed = Sequential ? std::memory_order_seq_cst : std::memory_order_relaxed;
    static constexpr auto acquire = Sequential ? std::memory_order_seq_cst : std::memory_order_acquire;
public:
    explicit Deque(std::int64_t start = 1) : top_(start), bottom_(start) {}
    bool push(T* value) noexcept {
        const auto b = bottom_.load(relaxed), t = top_.load(acquire);
        if (b >= std::numeric_limits<std::int64_t>::max() - 1 || b - t >= static_cast<std::int64_t>(N)) return false;
        slots_[static_cast<std::size_t>(b) & (N - 1)].store(value, relaxed);
        // A release store is the publication equivalent of Fig.1's release
        // fence followed by a relaxed store; it is also visible to TSan.
        bottom_.store(b + 1, Sequential ? std::memory_order_seq_cst : std::memory_order_release);
        return true;
    }
    T* pop() noexcept {
        const auto b = bottom_.load(relaxed) - 1; // starts at 1, so empty pop cannot underflow
        bottom_.store(b, relaxed);
        if constexpr (!Sequential) std::atomic_thread_fence(std::memory_order_seq_cst);
        auto t = top_.load(relaxed);
        T* value = nullptr;
        if (t <= b) {
            value = slots_[static_cast<std::size_t>(b) & (N - 1)].load(relaxed);
            if (t == b) {
                if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, relaxed)) value = nullptr;
                bottom_.store(b + 1, relaxed);
            }
        } else bottom_.store(b + 1, relaxed);
        return value;
    }
    T* steal() noexcept {
        auto t = top_.load(acquire);
        if constexpr (!Sequential) std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto b = bottom_.load(acquire);
        if (t >= b) return nullptr;
        T* value = slots_[static_cast<std::size_t>(t) & (N - 1)].load(relaxed);
        return top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, relaxed) ? value : nullptr;
    }
};
}
