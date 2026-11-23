#pragma once
//
// Where idle workers sleep: an event count.
//
// Adapted from the mpmc kata's, which derives it at length; the argument in
// brief. A waiter reads the epoch (its key), announces itself in waiters_,
// re-checks for work, and only then blocks — and only while the epoch still
// equals the key. A notifier publishes its work, then reads waiters_, and if
// anyone is there advances the epoch and wakes one.
//
// Each side stores to one location and then loads another: the store-buffering
// shape, which is the one reordering x86 performs and the one release/acquire
// cannot forbid. A seq_cst fence on each side does. The two fences fall in one
// total order; if the waiter's comes first, the notifier sees it in waiters_,
// and if the notifier's comes first, the waiter's re-check sees the work.
//
// pending_ coalesces wakes: one in flight at a time, because a woken thread
// takes tens of microseconds to run and is still counted in waiters_ until it
// does, and every push in that window would otherwise issue another wake.
//
// The fences are invisible to ThreadSanitizer. Nothing here publishes plain
// data — the work itself is published by the deque's release/acquire — so what
// TSan cannot see in this file is a lost wakeup, never a data race. The
// scheduler counts lost wakeups directly instead (see Stats::rescued).

#include "platform.hpp"

#include <atomic>
#include <cstdint>

namespace keystone {

class EventCount {
public:
    using Key = std::uint32_t;

    Key prepare_wait() noexcept {
        const Key key = epoch_.load(std::memory_order_acquire);
        waiters_.fetch_add(1, std::memory_order_acq_rel);
        pending_.store(false, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);  // announce, then re-check
        return key;
    }

    void cancel_wait() noexcept { waiters_.fetch_sub(1, std::memory_order_relaxed); }

    // Sleeps until the epoch moves past `key`, or `until`. False on timeout.
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

    // After publishing work. The fence is the caller's when `fenced` is false:
    // the scheduler fences once and then reads two things.
    bool notify_one(bool fenced = true) noexcept {
        if (fenced) std::atomic_thread_fence(std::memory_order_seq_cst);  // publish, then look
        if (waiters_.load(std::memory_order_acquire) == 0) return false;
        if (pending_.load(std::memory_order_relaxed) || pending_.exchange(true, std::memory_order_acq_rel))
            return false;
        epoch_.fetch_add(1, std::memory_order_release);
        wake_one(epoch_);
        return true;
    }

    void notify_all() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        epoch_.fetch_add(1, std::memory_order_release);
        wake_all(epoch_);
    }

    std::uint32_t waiters() const noexcept { return waiters_.load(std::memory_order_relaxed); }

private:
    alignas(kCacheLine) std::atomic<std::uint32_t> waiters_{0};
    std::atomic<bool>                              pending_{false};
    alignas(kCacheLine) std::atomic<std::uint32_t> epoch_{0};
};

}  // namespace keystone
