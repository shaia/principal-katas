#pragma once
//
// turnstile::Queue — the redesign. A bounded MPMC ring in which every slot
// carries its own sequence number, stamped with the lap.
//
// The answer to the kata's central question is in the layout: a reservation
// lives in a shared counter, and a publication lives in the slot. For the slot
// that holds position p = lap·N + i:
//
//   seq == p          free for the producer that reserves p
//   seq == p + 1      published — the consumer that claims p may read it
//   seq == p + N      released — free for the producer of p + N, one lap on
//
// "Reserved but not yet published" is not a value of seq at all. It is seq == p
// while the tail has already passed p, visible only by reading both — which is
// why a consumer at the head cannot tell a hole from an empty queue without
// looking at the producers' counter, and why it only looks before parking.
//
// Neither side reads the other side's counter to decide full or empty. A
// producer's "full" is a slot still holding the previous lap; a consumer's
// "empty" is a slot not yet holding this one. So the tail's line is written
// only by producers and the head's only by consumers, and the line that
// genuinely has to move between them is the slot's own, with the event in it.
//
// Two release/acquire pairs run through each slot, in opposite directions: the
// producer's store of p + 1 publishes the event to the consumer, and the
// consumer's store of p + N hands the empty slot to the next lap's producer —
// so that producer cannot construct into storage this lap's consumer is still
// reading.
//
// A reservation is a debt, and every path out of push pays it. A CAS
// reservation cannot happen after a close, because the closed bit lives in the
// word it operates on. A ticket can, because a fetch_add cannot be refused — so
// a ticket that lands after the close pays its slot with a tombstone: the
// publication store with kDead set and no object behind it, which consumers
// claim, skip and release. That keeps the consumers' count of positions owed
// exact, which is what lets them know when they are finished.

#include "eventcount.hpp"
#include "platform.hpp"
#include "queue_api.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>

namespace turnstile {

// How a blocking push claims a position. try_push always uses CAS; the two can
// share one queue, because they operate on the same word and both leave a
// position claimed by exactly one producer.
//
// cas:    look, then claim — compare-and-swap the tail after checking the slot is
//         free. A blocked push can give up at its deadline.
// ticket: claim, then look — one fetch_add that never fails, and a position that
//         cannot be handed back. A blocked push waits for its slot however long
//         that takes; it has no deadline to honour.
enum class Reserve : std::uint8_t { cas, ticket };

// The four deliberate bugs of the C++ track's step C5, compiled in on request
// and nowhere else. Each is one line of the correct protocol made wrong.
enum class Bug : std::uint8_t { none, relaxed_seq_load, relaxed_seq_store, no_fence, no_full_check };

struct Options {
    Reserve reserve        = Reserve::cas;
    bool    padded         = true;   // one slot per cache line
    bool    measure_window = false;  // record reserve→publish ticks in OpStats::window
    bool    backoff        = false;  // pause after losing a race on a counter, longer each time
    // The two wake economies the measurements forced, switchable so that phases 4
    // and 5 can run the first versions beside the fixes in the same process.
    bool    coalesce_wakes   = true;  // one wake in flight per event count
    bool    batch_full_wakes = true;  // consumers wake blocked producers once per batch
    Bug     bug            = Bug::none;
};

// Test seams. Each is called at a point where the real queue has no reason to
// wait, so a hook that sleeps reproduces a thread descheduled exactly there.
struct NoHooks {
    void reserved(std::uint64_t) noexcept {}  // producer: after reserving, before publishing
    void claimed(std::uint64_t) noexcept {}   // consumer: after claiming, before releasing
    void parking() noexcept {}                // consumer: about to announce itself as a sleeper
};

template <typename T, std::size_t N, Options O = Options{}, typename Hooks = NoHooks>
class Queue {
    static_assert(std::has_single_bit(N) && N >= 2, "positions map to slots by masking");
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
                      std::is_nothrow_destructible_v<T>,
                  "a reservation is a debt: nothing between reserve and publish may throw");

    static constexpr std::uint64_t kMask   = N - 1;
    static constexpr std::uint64_t kClosed = 1;                         // low bit of the tail word
    static constexpr std::uint64_t kDead   = std::uint64_t{1} << 63;   // a tombstone's publication

    static constexpr auto kSeqLoad =
        O.bug == Bug::relaxed_seq_load ? std::memory_order_relaxed : std::memory_order_acquire;
    static constexpr auto kSeqStore =
        O.bug == Bug::relaxed_seq_store ? std::memory_order_relaxed : std::memory_order_release;
    static constexpr bool kFenced = O.bug != Bug::no_fence;

    // Spin before parking, in pause instructions. Counted, not timed: phase 0
    // reports what one costs on each kind of core, and the two differ.
    static constexpr int kSpins = 128;

    // Consumers wake a producer blocked on full once per this many releases, not
    // on every one. A blocked producer's re-check succeeds as soon as any slot is
    // free, so waking it per slot made every pop a wake, and each pop paid for it —
    // phase 4 runs both, side by side. Batching cannot lose an event, but it can
    // delay one: a wake can land on a producer whose slot is not yet free while
    // the slot that matters is released without one. Two things bound that — a
    // producer that gets in after parking passes the wake on (chain_full_wake),
    // and the backstop — so the worst case is the next batch boundary or the
    // backstop.
    static constexpr std::uint64_t kFullWakeEvery = !O.batch_full_wakes ? 1 : N >= 128 ? 64 : N / 2;

    // A ticket holder whose slot is still occupied sleeps this long between
    // checks. It cannot be woken for its own slot without a wake on every
    // release; it only waits on a full queue, whose latency the consumers already
    // set. WaitOnAddress rounds it up to a millisecond.
    static constexpr Nanos kTicketPoll = std::chrono::microseconds(200);
    static constexpr int   kYields     = 64;  // between the spin and the timer; see await_turn

    // Truncated exponential backoff after a lost race: 1, 2, 4 … 32 pauses. Fewer
    // threads arriving at a line together means fewer of them lose, and the price
    // is paid by the loser — which is the fairness question of step 7.
    static void back_off(int& lost) noexcept {
        if constexpr (O.backoff) {
            const int n = 1 << std::min(lost++, 5);
            for (int i = 0; i < n; ++i) TURNSTILE_PAUSE();
        } else {
            (void)lost;
        }
    }

    struct alignas(O.padded ? kCacheLine : alignof(std::atomic<std::uint64_t>)) Slot {
        std::atomic<std::uint64_t> seq;
        alignas(T) std::byte       storage[sizeof(T)];
    };

public:
    static constexpr std::size_t kSlotBytes = sizeof(Slot);

    // `backstop` bounds every park. It is defence in depth, not the wakeup
    // mechanism: phase 5 counts how often it is the thing that woke a consumer.
    explicit Queue(Nanos backstop = std::chrono::milliseconds(50), Hooks hooks = {})
        : slots_(new Slot[N]), backstop_(backstop), hooks_(hooks) {
        // Writing every sequence also faults in every page of the ring, here, on
        // the constructing thread — so no producer takes a first-touch page fault
        // inside its window. The design cannot avoid this: no slot's initial
        // sequence is zero.
        for (std::uint64_t i = 0; i < N; ++i) slots_[i].seq.store(i, std::memory_order_relaxed);
    }

    ~Queue() {
        // Single-threaded by contract. The live objects are exactly the published,
        // unclaimed slots, and the sequences say which those are; a tombstone's
        // sequence carries kDead, so it is never mistaken for one.
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed) >> 1;
        for (std::uint64_t pos = head_.load(std::memory_order_relaxed); pos != tail; ++pos) {
            Slot& s = slot(pos);
            if (s.seq.load(std::memory_order_acquire) == pos + 1) std::destroy_at(object(s));
        }
    }

    Queue(const Queue&)            = delete;
    Queue& operator=(const Queue&) = delete;

    // ---------------------------------------------------------- producers

    // Reject on full. Moves from v only if it returns ok.
    Push try_push(T&& v, OpStats& st) noexcept {
        std::uint64_t word = tail_.load(std::memory_order_relaxed);
        int           lost = 0;
        for (;;) {
            if (word & kClosed) return Push::closed;
            const std::uint64_t pos = word >> 1;
            const std::uint64_t seq = slot(pos).seq.load(kSeqLoad) & ~kDead;
            const auto          dif = static_cast<std::int64_t>(seq - pos);
            if (dif == 0 || (O.bug == Bug::no_full_check && dif < 0)) {
                // relaxed: the reservation orders nothing, because no thread reads
                // anything on account of having seen it. The slot carries the data.
                // And because the closed bit lives in this word, a close and a
                // reservation are one decision: this fails if the queue closed.
                if (tail_.compare_exchange_weak(word, word + 2, std::memory_order_relaxed))
                    return publish(pos, std::move(v), st);
                ++st.retries;
                back_off(lost);
            } else if (dif < 0) {
                return Push::full;  // the slot still holds the previous lap
            } else {
                ++st.retries;       // another producer took pos; we read a stale tail
                back_off(lost);
                word = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    // Block on full. With Reserve::cas, until `until`, then timeout; with
    // Reserve::ticket, until the slot frees. Returns closed once the queue closes.
    Push push(T&& v, Clock::time_point until, OpStats& st) noexcept {
        if constexpr (O.reserve == Reserve::ticket) {
            (void)until;
            // The pre-check spares a closed queue a ticket in the common case. The
            // race it leaves — a close between this load and the fetch_add — is
            // paid for below, with a tombstone.
            if (tail_.load(std::memory_order_relaxed) & kClosed) return Push::closed;
            const std::uint64_t word = tail_.fetch_add(2, std::memory_order_relaxed);
            const std::uint64_t pos  = word >> 1;
            await_turn(pos, st);
            if (word & kClosed) {
                slot(pos).seq.store((pos + 1) | kDead, std::memory_order_release);
                return Push::closed;  // owed, not accepted: paid, and refused
            }
            return publish(pos, std::move(v), st);
        } else {
            bool parked = false;
            for (;;) {
                Push r = try_push(std::move(v), st);
                if (r != Push::full) {
                    if (parked && r == Push::ok) chain_full_wake(st);
                    return r;
                }
                const auto key = not_full_.prepare_wait();
                r              = try_push(std::move(v), st);
                if (r != Push::full) {
                    not_full_.cancel_wait();
                    if (parked && r == Push::ok) chain_full_wake(st);
                    return r;
                }
                const auto now = Clock::now();
                if (now >= until) {
                    not_full_.cancel_wait();
                    return Push::timeout;
                }
                ++st.parks;
                parked = true;
                not_full_.wait(key, std::min(until, now + backstop_));
            }
        }
    }

    // ---------------------------------------------------------- consumers

    // Never reads the tail: "empty" is decided by the head slot alone.
    Pop try_pop(T& out, OpStats& st) noexcept {
        std::uint64_t pos  = head_.load(std::memory_order_relaxed);
        int           lost = 0;
        for (;;) {
            const std::uint64_t raw = slot(pos).seq.load(kSeqLoad);
            const auto          dif = static_cast<std::int64_t>((raw & ~kDead) - (pos + 1));
            if (dif == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    if (!(raw & kDead)) return consume(pos, out, st);
                    release(pos, st);  // a tombstone: nothing to read, give the slot back
                    ++pos;
                    continue;
                }
                ++st.retries;
                back_off(lost);
            } else if (dif < 0) {
                return Pop::empty;  // not yet published for this lap — empty, or a hole
            } else {
                ++st.retries;
                back_off(lost);
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    // Spin, then park, until an event arrives, `until` passes, or the queue is
    // closed and every position ever reserved has been claimed.
    Pop pop(T& out, Clock::time_point until, OpStats& st) noexcept {
        for (;;) {
            if (try_pop(out, st) == Pop::ok) return Pop::ok;

            // A producer mid-window publishes within nanoseconds unless it has
            // been descheduled, and a park costs microseconds: spin first.
            for (int i = 0; i < kSpins && !head_published(); ++i) TURNSTILE_PAUSE();
            if (try_pop(out, st) == Pop::ok) return Pop::ok;

            // Only now, about to sleep, look at the producers' counter. After a
            // close nobody parks: what remains is finite, and every position below
            // the tail is owed by a producer that already holds it, so yield and
            // retry until every one has been claimed — or until `until`.
            if (const std::uint64_t word = tail_.load(std::memory_order_acquire); word & kClosed) {
                if (head_.load(std::memory_order_relaxed) == word >> 1) return Pop::drained;
                if (Clock::now() >= until) return Pop::timeout;
                std::this_thread::yield();
                continue;
            }

            hooks_.parking();
            const auto key = not_empty_.prepare_wait();
            // The re-check. It is exact for the slot it reads; the review of this
            // file noted the one formal gap — a stale head_ could lead it to read
            // an older slot than the one just published — which no x86 or AArch64
            // execution exhibits, and which the backstop bounds regardless.
            if (try_pop(out, st) == Pop::ok) {
                not_empty_.cancel_wait();
                return Pop::ok;
            }
            const std::uint64_t word = tail_.load(std::memory_order_acquire);
            if (word & kClosed) {  // closed since the check above: handled at the top
                not_empty_.cancel_wait();
                continue;
            }
            if ((word >> 1) > head_.load(std::memory_order_relaxed)) ++st.holes;  // reserved, unpublished

            const auto now = Clock::now();
            if (now >= until) {
                not_empty_.cancel_wait();
                return Pop::timeout;
            }
            ++st.parks;
            const auto wake_by = std::min(until, now + backstop_);
            if (!not_empty_.wait(key, wake_by) && Clock::now() < until) ++st.backstops;

            // The first pop after a wake may be the front of a burst. If there is
            // more behind it, wake the next sleeper rather than leaving that to
            // whichever producer publishes next — so a burst costs producers one
            // wake, not one per sleeper.
            if (try_pop(out, st) == Pop::ok) {
                if (head_published() && not_empty_.template notify_one<kFenced>()) ++st.wakes;
                return Pop::ok;
            }
        }
    }

    // ---------------------------------------------------------- shutdown

    // After this returns, no CAS reservation can succeed, and every ticket taken
    // from now on is paid with a tombstone and refused. Positions reserved before
    // it are still owed and will be published; consumers drain until they have
    // claimed every position below the tail.
    void close() noexcept {
        tail_.fetch_or(kClosed, std::memory_order_acq_rel);
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool closed() const noexcept { return tail_.load(std::memory_order_acquire) & kClosed; }

    // Diagnostics, racy by nature.
    std::uint64_t reserved() const noexcept { return tail_.load(std::memory_order_relaxed) >> 1; }
    std::uint64_t claimed() const noexcept { return head_.load(std::memory_order_relaxed); }
    std::uint32_t sleepers() const noexcept { return not_empty_.waiters(); }

    Hooks& hooks() noexcept { return hooks_; }

private:
    Slot& slot(std::uint64_t pos) noexcept { return slots_[pos & kMask]; }
    static T* object(Slot& s) noexcept { return std::launder(reinterpret_cast<T*>(s.storage)); }

    bool head_published() noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        return (slot(h).seq.load(std::memory_order_acquire) & ~kDead) == h + 1;
    }

    // The window. Everything between the reservation and the store of pos + 1 is
    // time during which every consumer behind this position waits — so nothing in
    // it may block, allocate or throw, and T is built by the caller beforehand.
    Push publish(std::uint64_t pos, T&& v, OpStats& st) noexcept {
        [[maybe_unused]] std::uint64_t t0 = 0;
        if constexpr (O.measure_window) t0 = TscClock::now();
        Slot& s = slot(pos);
        std::construct_at(reinterpret_cast<T*>(s.storage), std::move(v));
        hooks_.reserved(pos);
        s.seq.store(pos + 1, kSeqStore);  // publication: pairs with the consumer's acquire
        if constexpr (O.measure_window) st.window->record(TscClock::now() - t0);
        if (not_empty_.template notify_one<kFenced>()) ++st.wakes;
        return Push::ok;
    }

    Pop consume(std::uint64_t pos, T& out, OpStats& st) noexcept {
        Slot& s   = slot(pos);
        T*    obj = object(s);
        out       = std::move(*obj);
        std::destroy_at(obj);
        hooks_.claimed(pos);
        release(pos, st);
        return Pop::ok;
    }

    void release(std::uint64_t pos, OpStats& st) noexcept {
        slot(pos).seq.store(pos + N, kSeqStore);  // hand the slot to the producer one lap on
        if (pos % kFullWakeEvery != 0) return;
        // A blocked CAS producer can use any free slot, so waking one is enough. A
        // blocked ticket holder needs its own slot, and one arbitrary wake usually
        // reaches the wrong holder: on a small ring, waking one at a time left most
        // of them to their timers and phase 2 took eight minutes. So tickets wake
        // all the waiters — once per batch, and only if there are any.
        if constexpr (O.reserve == Reserve::ticket) {
            if (not_full_.template notify_all_if_waiting<kFenced>()) ++st.wakes;
        } else {
            if (not_full_.template notify_one<kFenced>()) ++st.wakes;
        }
    }

    // A producer that parked on full and has now got in may not be the only one
    // waiting, and it may have only one event to push. If there is room behind it,
    // pass the wake on.
    void chain_full_wake(OpStats& st) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed) >> 1;
        if ((slot(t).seq.load(std::memory_order_acquire) & ~kDead) == t && not_full_.template notify_one<kFenced>())
            ++st.wakes;
    }

    // A ticket holder waiting for the previous lap to vacate its slot: spin, then
    // yield, then sleep and re-check. The batched release wake in release() wakes
    // every ticket waiter; the timer only catches a holder whose slot freed between
    // two batch boundaries with no further release coming. The yield rung keeps a
    // few-microsecond wait — the usual one on a small ring — off a timer that
    // WaitOnAddress rounds up to a millisecond.
    void await_turn(std::uint64_t pos, OpStats& st) noexcept {
        Slot& s = slot(pos);
        for (int i = 0; i < kSpins; ++i) {
            if (s.seq.load(kSeqLoad) == pos) return;
            TURNSTILE_PAUSE();
        }
        for (int i = 0; i < kYields; ++i) {
            if (s.seq.load(kSeqLoad) == pos) return;
            std::this_thread::yield();
        }
        while (s.seq.load(kSeqLoad) != pos) {
            const auto key = not_full_.prepare_wait();
            if (s.seq.load(kSeqLoad) == pos) {
                not_full_.cancel_wait();
                return;
            }
            ++st.parks;
            not_full_.wait(key, Clock::now() + kTicketPoll);
        }
    }

    // One writer kind per line: producers CAS the tail, consumers CAS the head.
    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};  // (next position << 1) | closed
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};  // next position to claim
    // Read by everyone, written by nobody after construction.
    alignas(kCacheLine) std::unique_ptr<Slot[]> slots_;
    Nanos                                       backstop_;
    Hooks                                       hooks_;
    BasicEventCount<O.coalesce_wakes>           not_empty_;
    BasicEventCount<O.coalesce_wakes>           not_full_;
};

}  // namespace turnstile
