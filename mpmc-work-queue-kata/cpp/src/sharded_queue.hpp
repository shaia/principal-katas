#pragma once
//
// One SPSC ring per producer in place of one shared ring — the experiment the
// reference outline proposes: "32 producer-local SPSC queues against one shared
// MPMC queue". The brief's 65,536 slots are split evenly, so 32 producers get
// 2,048 each. Ring p is written only by producer p and read only by consumer
// p % C, which owns P / C rings and serves them round-robin.
//
// What that removes. No producer shares a line with another: a push is a load,
// a store into the slot, and one read-modify-write of a line shared only with its
// own consumer. And there is no hole — a producer descheduled before publishing
// delays its own ring and nobody else's, because nobody else's events are queued
// behind it.
//
// What it gives up is the shared head. A consumer serves only its own rings, so
// work cannot move to an idle consumer, and capacity cannot move to a busy
// producer: a ring's 2,048 slots are all one source has, however empty the other
// 63,488 are. Phase 9 measures both sides of that.
//
// The ring is Kata 1's SpscRing (mpsc-event-pipeline-kata/cpp/src/spsc_ring.hpp):
// free-running indices, each side caching the other's, two release/acquire pairs.
// One thing changes, for shutdown. The tail word carries a closed bit, as
// turnstile::Queue's does, and the producer publishes by compare-and-swap on it
// rather than by a plain store. close() sets the bit with fetch_or, so a
// publication and a close are one decision: a push the close got ahead of fails,
// instead of landing in a ring whose consumer has already finished with it. A
// plain store would be cheaper, and would race the fetch_or.

#include "eventcount.hpp"
#include "mpmc_queue.hpp"
#include "platform.hpp"
#include "queue_api.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>

namespace turnstile {

// Hooks are told positions encoded as pos * P + ring, so that one StallControl
// position names one ring — and so one producer or one consumer — at about the
// moment the shared queue reaches the same overall position.
template <typename T, std::size_t N, int P, int C, typename Hooks = NoHooks>
class ShardedQueue {
    static_assert(P > 0 && C > 0 && P % C == 0, "every consumer owns the same number of rings");
    static constexpr std::size_t kRing = N / static_cast<std::size_t>(P);
    static_assert(std::has_single_bit(kRing) && kRing >= 2 && kRing * static_cast<std::size_t>(P) == N,
                  "the brief's slots, split evenly into power-of-two rings");
    static_assert(std::is_nothrow_move_assignable_v<T>, "nothing between the checks and the publication may throw");

    static constexpr std::uint64_t kMask   = kRing - 1;
    static constexpr std::uint64_t kClosed = 1;
    static constexpr int           kOwned  = P / C;
    // As in Queue: spin before parking, and wake a blocked producer once per batch.
    static constexpr int           kSpins         = 128;
    static constexpr std::uint64_t kFullWakeEvery = kRing >= 128 ? 64 : kRing / 2;

    struct Ring {
        // The producer's line: the tail word, and its private copy of the head.
        alignas(kCacheLine) std::atomic<std::uint64_t> tail{0};  // (next position << 1) | closed
        std::uint64_t cached_head = 0;
        // The consumer's line, and its private copy of the tail.
        alignas(kCacheLine) std::atomic<std::uint64_t> head{0};
        std::uint64_t cached_tail = 0;
        EventCount    not_full;  // the ring's one producer, blocked on full
        // Value-initialized, so the constructor faults every page in, as Queue's does.
        alignas(kCacheLine) std::array<T, kRing> slots{};
    };

    struct alignas(kCacheLine) Consumer {
        EventCount not_empty;
        int        next = 0;  // which of its rings to try first; private to the consumer
    };

public:
    static constexpr int kRingProducers = P;  // the producer ids this queue accepts
    static constexpr int kRingConsumers = C;  // and the consumer ids

    explicit ShardedQueue(Nanos backstop = std::chrono::milliseconds(50), Hooks hooks = {})
        : rings_(new Ring[P]), consumers_(new Consumer[C]), backstop_(backstop), hooks_(hooks) {}

    ShardedQueue(const ShardedQueue&)            = delete;
    ShardedQueue& operator=(const ShardedQueue&) = delete;

    // ---------------------------------------------------------- producers

    // Reject on full. Moves from v if it returns ok, and may if a close refused it.
    Push try_push(T&& v, OpStats& st, int producer) noexcept {
        assert(producer >= 0 && producer < P);
        Ring&         r    = rings_[producer];
        std::uint64_t word = r.tail.load(std::memory_order_relaxed);  // only this thread advances it
        if (word & kClosed) return Push::closed;
        const std::uint64_t pos = word >> 1;
        if (pos - r.cached_head == kRing) {
            // Believed full. Only now read the consumer's line; acquire pairs with its
            // release of head, so the slot it has finished reading is ours to reuse.
            r.cached_head = r.head.load(std::memory_order_acquire);
            if (pos - r.cached_head == kRing) return Push::full;
        }
        r.slots[pos & kMask] = std::move(v);
        hooks_.reserved(pos * P + static_cast<std::uint64_t>(producer));
        // Publication: release pairs with the consumer's acquire of the tail. It
        // fails only if close() set the bit since the load above, and then the slot
        // just written lies past the final tail, where nobody will read it.
        if (!r.tail.compare_exchange_strong(word, word + 2, std::memory_order_release, std::memory_order_relaxed))
            return Push::closed;
        if (consumers_[producer % C].not_empty.notify_one()) ++st.wakes;
        return Push::ok;
    }

    // Block on full, until `until`. Returns closed once the queue closes.
    Push push(T&& v, Clock::time_point until, OpStats& st, int producer) noexcept {
        Ring& r = rings_[producer];
        for (;;) {
            Push res = try_push(std::move(v), st, producer);
            if (res != Push::full) return res;
            const auto key = r.not_full.prepare_wait();
            res            = try_push(std::move(v), st, producer);
            if (res != Push::full) {
                r.not_full.cancel_wait();
                return res;
            }
            const auto now = Clock::now();
            if (now >= until) {
                r.not_full.cancel_wait();
                return Push::timeout;
            }
            ++st.parks;
            r.not_full.wait(key, std::min(until, now + backstop_));
        }
    }

    // ---------------------------------------------------------- consumers

    // One event from any of this consumer's rings, or empty. Never waits.
    Pop try_pop(T& out, OpStats& st, int consumer) noexcept {
        assert(consumer >= 0 && consumer < C);
        return try_pop(out, st, consumers_[consumer], consumer) ? Pop::ok : Pop::empty;
    }

    // Spin, then park, until an event arrives on one of this consumer's rings,
    // `until` passes, or every ring it owns is closed and empty.
    Pop pop(T& out, Clock::time_point until, OpStats& st, int consumer) noexcept {
        assert(consumer >= 0 && consumer < C);
        Consumer& me = consumers_[consumer];
        for (;;) {
            if (try_pop(out, st, me, consumer)) return Pop::ok;
            for (int i = 0; i < kSpins && !any_published(consumer); ++i) TURNSTILE_PAUSE();
            if (try_pop(out, st, me, consumer)) return Pop::ok;

            // After a close nobody parks: a closed ring's tail is final, so what is
            // left is finite. close() sets the rings' bits one at a time; until it
            // has reached all of this consumer's, keep looking.
            if (any_closed(consumer)) {
                if (drained(consumer)) return Pop::drained;
                if (Clock::now() >= until) return Pop::timeout;
                std::this_thread::yield();
                continue;
            }

            hooks_.parking();
            const auto key = me.not_empty.prepare_wait();
            if (try_pop(out, st, me, consumer)) {
                me.not_empty.cancel_wait();
                return Pop::ok;
            }
            if (any_closed(consumer)) {
                me.not_empty.cancel_wait();
                continue;
            }
            const auto now = Clock::now();
            if (now >= until) {
                me.not_empty.cancel_wait();
                return Pop::timeout;
            }
            ++st.parks;
            if (!me.not_empty.wait(key, std::min(until, now + backstop_)) && Clock::now() < until) ++st.backstops;
        }
    }

    // ---------------------------------------------------------- shutdown

    // After this returns, no push can succeed. Events published before it are
    // still delivered; each consumer finishes when its rings are closed and empty.
    void close() noexcept {
        for (int p = 0; p < P; ++p) rings_[p].tail.fetch_or(kClosed, std::memory_order_acq_rel);
        for (int c = 0; c < C; ++c) consumers_[c].not_empty.notify_all();
        for (int p = 0; p < P; ++p) rings_[p].not_full.notify_all();
    }

    // The last ring close() reaches: once it is closed, all of them are.
    bool closed() const noexcept { return rings_[P - 1].tail.load(std::memory_order_acquire) & kClosed; }

    // Diagnostics, racy by nature: sums over the rings, read one at a time.
    std::uint64_t reserved() const noexcept {
        std::uint64_t n = 0;
        for (int p = 0; p < P; ++p) n += rings_[p].tail.load(std::memory_order_relaxed) >> 1;
        return n;
    }
    std::uint64_t claimed() const noexcept {
        std::uint64_t n = 0;
        for (int p = 0; p < P; ++p) n += rings_[p].head.load(std::memory_order_relaxed);
        return n;
    }

    Hooks& hooks() noexcept { return hooks_; }

private:
    // The rings consumer c owns are c, c + C, c + 2C, ...
    static int ring_of(int consumer, int k) noexcept { return consumer + C * k; }

    // One event from the first of this consumer's rings that has one, starting
    // after the ring it last took from, so that a busy ring cannot starve the rest.
    bool try_pop(T& out, OpStats& st, Consumer& me, int consumer) noexcept {
        for (int k = 0; k < kOwned; ++k) {
            const int i = (me.next + k) % kOwned;
            if (pop_ring(ring_of(consumer, i), out, st)) {
                me.next = (i + 1) % kOwned;
                return true;
            }
        }
        return false;
    }

    bool pop_ring(int p, T& out, OpStats& st) noexcept {
        Ring&               r = rings_[p];
        const std::uint64_t h = r.head.load(std::memory_order_relaxed);  // only this thread advances it
        if (h == r.cached_tail) {
            // Acquire pairs with the producer's publication.
            r.cached_tail = r.tail.load(std::memory_order_acquire) >> 1;
            if (h == r.cached_tail) return false;
        }
        out = std::move(r.slots[h & kMask]);
        hooks_.claimed(h * P + static_cast<std::uint64_t>(p));
        // Release: the read above must not sink past this store, or the producer
        // could overwrite the slot while it is still being read.
        r.head.store(h + 1, std::memory_order_release);
        if (h % kFullWakeEvery == 0 && r.not_full.notify_one()) ++st.wakes;
        return true;
    }

    bool any_published(int consumer) const noexcept {
        for (int k = 0; k < kOwned; ++k) {
            const Ring& r = rings_[ring_of(consumer, k)];
            if ((r.tail.load(std::memory_order_relaxed) >> 1) != r.head.load(std::memory_order_relaxed)) return true;
        }
        return false;
    }

    bool any_closed(int consumer) const noexcept {
        for (int k = 0; k < kOwned; ++k)
            if (rings_[ring_of(consumer, k)].tail.load(std::memory_order_acquire) & kClosed) return true;
        return false;
    }

    // Every ring this consumer owns is closed, so its tail is final, and every
    // position below that tail has been taken.
    bool drained(int consumer) const noexcept {
        for (int k = 0; k < kOwned; ++k) {
            const Ring&         r = rings_[ring_of(consumer, k)];
            const std::uint64_t w = r.tail.load(std::memory_order_acquire);
            if (!(w & kClosed) || r.head.load(std::memory_order_relaxed) != (w >> 1)) return false;
        }
        return true;
    }

    std::unique_ptr<Ring[]>     rings_;
    std::unique_ptr<Consumer[]> consumers_;
    Nanos                       backstop_;
    Hooks                       hooks_;
};

}  // namespace turnstile
