#pragma once
//
// A bounded Chase–Lev work-stealing deque.
//
// The owner pushes and pops at the bottom, LIFO, with no read-modify-write on
// the common path. Thieves take from the top, FIFO, with a CAS on `top_`. The
// one race that needs arbitration is the last element, wanted by the owner's
// pop and a thief's steal at once; both settle it with the same CAS.
//
// Bounded, because "bounded ready memory" is a requirement and a deque that
// grows is a heap allocation, and a retired buffer to reclaim, on the path
// that runs when the system is busiest. A push into a full deque fails, and the
// scheduler runs that task inline instead: the overflow policy is work-first.
//
// Orderings. The textbook C11 version (Lê, Pop, Cohen, Zappa Nardelli, PPoPP
// 2013) uses standalone fences. This one has none, for one reason: the
// ThreadSanitizer this project checks with does not model standalone fences,
// and a protocol it cannot see is a protocol it cannot vouch for. The fences are
// replaced by seq_cst operations on the locations they ordered, which on x86-64
// costs the same — the owner's pop pays one locked instruction either way.
//
// The second difference matters more. A deque of pointers is a *publication*
// channel, not just a queue of integers: the thief must see everything the
// owner wrote into the task before pushing it. So the slot is stored with
// release and loaded with acquire, and the payload's visibility does not depend
// on which store to `bottom_` a thief happened to read.

#include "platform.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace keystone {

template <typename T>
class WsDeque {
public:
    explicit WsDeque(std::size_t capacity_pow2 = 4096)
        : mask_(static_cast<std::int64_t>(capacity_pow2) - 1),
          buf_(std::make_unique<std::atomic<T*>[]>(capacity_pow2)) {
        if ((capacity_pow2 & (capacity_pow2 - 1)) != 0) std::abort();
    }

    // Owner only. False when full: the caller keeps the task.
    //
    // `relaxed_publish` is the C++ track's deliberate bug: the slot and the new
    // bottom stored relaxed, so a thief can take the pointer without the writes
    // made to the task before it was pushed.
    bool push(T* x, bool relaxed_publish = false) noexcept {
        const auto b = bottom_.load(std::memory_order_relaxed);
        const auto t = top_.load(std::memory_order_acquire);
        if (b - t > mask_) return false;
        if (relaxed_publish) {
            buf_[static_cast<std::size_t>(b & mask_)].store(x, std::memory_order_relaxed);
            bottom_.store(b + 1, std::memory_order_relaxed);
        } else {
            buf_[static_cast<std::size_t>(b & mask_)].store(x, std::memory_order_release);
            bottom_.store(b + 1, std::memory_order_release);
        }
        return true;
    }

    // Owner only. LIFO: the task pushed last, whose inputs are warmest.
    T* pop() noexcept {
        const auto b = bottom_.load(std::memory_order_relaxed) - 1;
        // Claim slot b before looking at top: a store then a load of a different
        // location, the store-buffering shape, which x86 reorders and only
        // sequential consistency forbids. Under it, a thief that has not yet
        // seen this store must have its CAS on top_ ordered before our load.
        bottom_.store(b, std::memory_order_seq_cst);
        auto t = top_.load(std::memory_order_seq_cst);
        if (t > b) {  // was empty
            bottom_.store(b + 1, std::memory_order_release);
            return nullptr;
        }
        T* x = buf_[static_cast<std::size_t>(b & mask_)].load(std::memory_order_acquire);
        if (t == b) {  // the last element: race the thieves for it
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                x = nullptr;
            bottom_.store(b + 1, std::memory_order_release);
        }
        return x;
    }

    enum class Steal { ok, empty, lost };

    // Any thread. FIFO: the oldest task, the one least likely to be in the
    // owner's cache and most likely to have work behind it.
    Steal steal(T*& out) noexcept {
        auto       t = top_.load(std::memory_order_seq_cst);
        const auto b = bottom_.load(std::memory_order_seq_cst);
        if (t >= b) return Steal::empty;
        T* x = buf_[static_cast<std::size_t>(t & mask_)].load(std::memory_order_acquire);
        // If the CAS succeeds, nobody else took slot t, so the owner has not
        // reused it either: it cannot push past top_ + capacity.
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return Steal::lost;
        out = x;
        return Steal::ok;
    }

    // A racy hint, for a worker deciding whether to sleep. Never trusted for
    // correctness: the sleep protocol re-checks after announcing itself.
    bool looks_empty() const noexcept {
        return top_.load(std::memory_order_seq_cst) >= bottom_.load(std::memory_order_seq_cst);
    }
    std::int64_t size_hint() const noexcept {
        const auto d = bottom_.load(std::memory_order_relaxed) - top_.load(std::memory_order_relaxed);
        return d < 0 ? 0 : d;
    }

private:
    // top_ is written by thieves, bottom_ by the owner on every push and pop: one
    // line each, or every push would invalidate every thief's copy of top_.
    alignas(kCacheLine) std::atomic<std::int64_t> top_{0};
    alignas(kCacheLine) std::atomic<std::int64_t> bottom_{0};
    alignas(kCacheLine) const std::int64_t mask_;
    std::unique_ptr<std::atomic<T*>[]> buf_;
};

}  // namespace keystone
