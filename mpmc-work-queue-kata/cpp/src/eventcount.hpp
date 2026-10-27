#pragma once
//
// The sleep half of the queue: an event count.
//
// With one consumer there would be one sleeper to wake. Here sixteen consumers
// sleep and thirty-two producers decide, on every push, whether to wake one. So
// the decision must cost a fence and a load when nobody is asleep — which at
// full rate is nearly always — and the sleep itself must accept a deadline,
// because a blocking push has one and C++20's atomic::wait cannot take one.
//
// The protocol. A waiter reads the epoch (its key), announces itself in
// waiters_, re-checks the condition it is about to wait for, and only then
// blocks — and only while the epoch still equals the key. A notifier publishes
// its change, then reads waiters_, and if anyone is there advances the epoch
// before waking.
//
// Each side stores to one location and then loads the other's: the
// store-buffering shape, which is the one reordering x86 performs and the one
// release/acquire cannot forbid. A seq_cst fence on each side forbids it. The
// two fences fall in one total order; if the waiter's comes first the notifier
// sees it in waiters_, and if the notifier's comes first the waiter's re-check
// sees the change. Either way nobody sleeps through a publication.
//
// One wake in flight. A woken thread takes tens of microseconds to run, and
// until it does it is still counted in waiters_ — so without more, every push in
// that window issues another wake, each costing the producer several
// microseconds. The first version of this did exactly that; Coalesce = false
// keeps it, so that phase 5 can run the two side by side. pending_ says a wake
// is already on its way, and producers that see it skip theirs. It is
// safe because every thread that enters or leaves the wait clears pending_ and
// then fences before re-checking for itself: an event whose producer skipped the
// wake was published before that producer's fence, and so is seen by the
// re-check of whichever waiter cleared the flag after it — the same
// store-buffering argument as above, with pending_ in place of waiters_.
// Clearing on entry as well as on wake matters: a wake whose only waiter
// cancelled would otherwise leave the flag set with nobody to clear it.
//
// The epoch is a counter, not a flag. A wait that compares a value before
// blocking cannot observe a change that is undone before it looks, and a flag
// can be undone; a 32-bit counter would have to come all the way round.

#include "platform.hpp"

#include <atomic>
#include <cstdint>

namespace turnstile {

template <bool Coalesce = true>
class BasicEventCount {
public:
    using Key = std::uint32_t;

    // Waiter, before its final re-check of the condition.
    Key prepare_wait() noexcept {
        const Key key = epoch_.load(std::memory_order_acquire);
        // acq_rel rather than relaxed: a notifier whose acquire load of waiters_
        // reads this increment (or a later one in its release sequence) then
        // knows the key above was read before its own epoch increment, so the
        // key can never already include the wake meant for this waiter.
        waiters_.fetch_add(1, std::memory_order_acq_rel);
        pending_.store(false, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);  // announce, then re-check
        return key;
    }

    // Waiter, when the re-check found what it was waiting for.
    void cancel_wait() noexcept { waiters_.fetch_sub(1, std::memory_order_relaxed); }

    // Waiter: sleep until the epoch moves past `key`, or `until`. Returns false if
    // `until` arrived with the epoch never having moved. Always deregisters, and
    // leaves the caller ready to re-check: the wake it may have absorbed is no
    // longer pending, and the fence orders that before the caller's next look.
    bool wait(Key key, Clock::time_point until) noexcept {
        bool woke = true;
        while (epoch_.load(std::memory_order_acquire) == key) {
            const auto now = Clock::now();
            if (now >= until) {
                woke = false;
                break;
            }
            wait_on(epoch_, key, std::chrono::duration_cast<Nanos>(until - now));
        }
        waiters_.fetch_sub(1, std::memory_order_relaxed);
        pending_.store(false, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return woke;
    }

    // Notifier, after publishing the change a waiter may be waiting for. Returns
    // whether it issued a wake. Fenced = false exists to be measured, and broken:
    // it is the step-5 bug, the fast path with its fence removed.
    template <bool Fenced = true>
    bool notify_one() noexcept {
        if constexpr (Fenced) std::atomic_thread_fence(std::memory_order_seq_cst);  // publish, then look
        if (waiters_.load(std::memory_order_acquire) == 0) return false;
        // A wake is already on its way; whoever absorbs it re-checks after us.
        if constexpr (Coalesce) {
            if (pending_.load(std::memory_order_relaxed) || pending_.exchange(true, std::memory_order_acq_rel))
                return false;
        }
        epoch_.fetch_add(1, std::memory_order_release);
        wake_one(epoch_);
        return true;
    }

    // Everyone, but only if anyone is waiting: for waiters that each need a
    // different thing to happen, so that waking one arbitrary waiter would usually
    // wake the wrong one. Same fast path, same fence, as notify_one.
    template <bool Fenced = true>
    bool notify_all_if_waiting() noexcept {
        if constexpr (Fenced) std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiters_.load(std::memory_order_acquire) == 0) return false;
        epoch_.fetch_add(1, std::memory_order_release);
        wake_all(epoch_);
        return true;
    }

    // For close: everyone, unconditionally. Rare enough not to need the fast path.
    void notify_all() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        epoch_.fetch_add(1, std::memory_order_release);
        wake_all(epoch_);
    }

    std::uint32_t waiters() const noexcept { return waiters_.load(std::memory_order_relaxed); }

private:
    // waiters_ is read by every producer on every push and stays shared in all
    // their caches; pending_ is read only when someone is waiting, so it shares
    // that line. epoch_ is written on every wake and gets a line of its own, so
    // that a wake does not turn the next push of every producer into a miss.
    alignas(kCacheLine) std::atomic<std::uint32_t> waiters_{0};
    std::atomic<bool>                              pending_{false};
    alignas(kCacheLine) std::atomic<std::uint32_t> epoch_{0};
};

using EventCount = BasicEventCount<true>;

// The reference outline's protocol, kept to be measured beside the event count:
// every notify advances the epoch and wakes, whether or not anyone is asleep.
//
// It needs no fence, and the unconditional increment is the reason. The epoch's
// modification order is total, and the waiter reads its key before re-checking.
// If that key already includes a producer's increment, the waiter's acquire load
// synchronizes with the producer's release RMW (or with one later in its release
// sequence), so the re-check sees the publication made before it. If it does
// not, the increment comes after the key in that order, and the wait either sees
// the epoch changed or is woken by the wake that follows it — which rests on the
// OS wait comparing the word and blocking as one step, as atomic::wait's does.
// The store-buffering race the event count fences against needs a notifier that
// loads before deciding to store; this one always stores.
//
// What it costs instead is a read-modify-write of one shared line, and a wake
// call, on every push. The outline writes it with atomic::wait and notify_one;
// this uses wait_on and wake_one, because atomic::wait cannot take the backstop,
// and libstdc++'s notify skips a futex waiter its own wait did not register.
class EpochWake {
public:
    using Key = std::uint32_t;

    Key  prepare_wait() noexcept { return epoch_.load(std::memory_order_acquire); }
    void cancel_wait() noexcept {}

    bool wait(Key key, Clock::time_point until) noexcept {
        while (epoch_.load(std::memory_order_acquire) == key) {
            const auto now = Clock::now();
            if (now >= until) return false;
            wait_on(epoch_, key, std::chrono::duration_cast<Nanos>(until - now));
        }
        return true;
    }

    template <bool Fenced = true>
    bool notify_one() noexcept {
        epoch_.fetch_add(1, std::memory_order_release);
        wake_one(epoch_);
        return true;
    }

    template <bool Fenced = true>
    bool notify_all_if_waiting() noexcept {
        notify_all();
        return true;
    }

    void notify_all() noexcept {
        epoch_.fetch_add(1, std::memory_order_release);
        wake_all(epoch_);
    }

    std::uint32_t waiters() const noexcept { return 0; }  // nobody is counted

private:
    alignas(kCacheLine) std::atomic<std::uint32_t> epoch_{0};
};

}  // namespace turnstile
