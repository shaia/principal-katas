#pragma once
//
// The pipeline: a registry of per-producer SPSC rings and the single consumer
// that drains them into a Sink.
//
// Three protocols live here and each is subtle enough to deserve naming:
//
//   producer lifetime   Free -> Active -> Retiring -> Free, where only the
//                       consumer may publish Free (see reclaim_retired)
//   wakeup handshake    a Dekker pair between signal() and clear_empty_bits(),
//                       needing the design's only seq_cst
//   shutdown            two phases, two modes, always bounded by a deadline

#include "config.hpp"
#include "event.hpp"
#include "platform.hpp"
#include "sink.hpp"
#include "spsc_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace weir {

class EventPipeline;

// Move-only ticket for one producer slot. The dtor retires the slot; the slot
// is not reusable until the consumer has drained it and released it.
class ProducerHandle {
public:
    ProducerHandle() = default;
    ProducerHandle(const ProducerHandle&) = delete;
    ProducerHandle& operator=(const ProducerHandle&) = delete;
    ProducerHandle(ProducerHandle&& o) noexcept
        : pipe_(std::exchange(o.pipe_, nullptr)), id_(o.id_) {}
    ProducerHandle& operator=(ProducerHandle&& o) noexcept {
        if (this != &o) { release(); pipe_ = std::exchange(o.pipe_, nullptr); id_ = o.id_; }
        return *this;
    }
    ~ProducerHandle() { release(); }

    bool valid() const noexcept { return pipe_ != nullptr; }
    std::uint32_t id() const noexcept { return id_; }

    inline bool push(const Event& e) noexcept;  // defined after EventPipeline

private:
    friend class EventPipeline;
    ProducerHandle(EventPipeline* p, std::uint32_t id) : pipe_(p), id_(id) {}
    inline void release() noexcept;

    EventPipeline* pipe_{nullptr};
    std::uint32_t  id_{0};
};

class EventPipeline {
public:
    using Config = PipelineConfig;

    explicit EventPipeline(Config cfg) : cfg_(cfg) {
        // All ring storage allocated once, up front. Nothing on the hot path
        // ever touches the allocator.
        for (auto& s : slots_) s.ring = std::make_unique<Ring>();
        stage_.resize(kStageEvents);
    }

    ~EventPipeline() { stop(StopMode::Abort); }

    EventPipeline(const EventPipeline&) = delete;
    EventPipeline& operator=(const EventPipeline&) = delete;

    template <Sink S>
    void start(S& sink) {
        running_.store(true, std::memory_order_relaxed);
        accepting_.store(true, std::memory_order_release);
        consumer_ = std::thread([this, &sink] { consume(sink); });
    }

    // Idempotent. Drain waits (bounded) for the rings to empty; Abort does not.
    void stop(StopMode mode, Nanos deadline = std::chrono::seconds(5)) {
        if (!consumer_.joinable()) return;
        // Producers see this and start failing pushes, so the rings can only
        // shrink from here. This must come first: draining a queue that is
        // still being filled is a race you can lose indefinitely.
        accepting_.store(false, std::memory_order_release);
        if (mode == StopMode::Drain) {
            const auto until = Clock::now() + deadline;
            // Bounded: a wedged sink must never make shutdown hang forever.
            while (Clock::now() < until && !all_quiesced()) {
                wake_consumer();
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            drain_deadline_met_ = all_quiesced();
        }
        running_.store(false, std::memory_order_release);
        wake_consumer();
        consumer_.join();
    }

    // False if a Drain hit its deadline and degraded to an Abort.
    bool drain_completed() const noexcept { return drain_deadline_met_; }

    [[nodiscard]] ProducerHandle register_producer() {
        for (std::uint32_t i = 0; i < kMaxProducers; ++i) {
            auto expected = SlotState::Free;
            // acquire on success: everything the previous owner and the
            // consumer's release of this slot did is visible before we reuse it.
            if (slots_[i].state.compare_exchange_strong(expected, SlotState::Active,
                                                        std::memory_order_acquire,
                                                        std::memory_order_relaxed)) {
                slots_[i].pushed.store(0, std::memory_order_relaxed);
                slots_[i].dropped.store(0, std::memory_order_relaxed);
                slots_[i].high_water.store(0, std::memory_order_relaxed);
                return ProducerHandle(this, i);
            }
        }
        return {};  // registry full — caller checks valid()
    }

    PipelineStats stats() const {
        PipelineStats s;
        s.pushed  = retired_pushed_.load(std::memory_order_relaxed);
        s.dropped = retired_dropped_.load(std::memory_order_relaxed);
        for (const auto& sl : slots_) {
            s.pushed  += sl.pushed.load(std::memory_order_relaxed);
            s.dropped += sl.dropped.load(std::memory_order_relaxed);
            s.high_water = std::max(s.high_water, sl.high_water.load(std::memory_order_relaxed));
        }
        s.consumed      = consumed_.load(std::memory_order_relaxed);
        s.rings_probed  = rings_probed_.load(std::memory_order_relaxed);
        s.rings_empty   = rings_empty_.load(std::memory_order_relaxed);
        s.passes        = passes_.load(std::memory_order_relaxed);
        s.bitmap_writes = bitmap_writes_.load(std::memory_order_relaxed);
        s.notifies      = notifies_.load(std::memory_order_relaxed);
        s.parks         = parks_.load(std::memory_order_relaxed);
        return s;
    }

    // Test hook: invoked by the consumer for each event, in order per producer.
    // Production code would not pay for a std::function on this path.
    std::function<void(const Event&)> on_event_;

private:
    friend class ProducerHandle;

    // Free -> Active (register) -> Retiring (handle dtor) -> Free (consumer,
    // only after a final drain). The consumer is the only one who may publish
    // Free, which is what keeps a dying producer from having its ring reused
    // while the consumer is still reading it.
    enum class SlotState : std::uint8_t { Free, Active, Retiring };

    struct alignas(kCacheLine) ProducerSlot {
        std::unique_ptr<Ring> ring;
        std::atomic<SlotState> state{SlotState::Free};
        // Producer-owned counters: relaxed, and in the producer's own line.
        std::atomic<std::uint64_t> pushed{0};
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<std::uint64_t> high_water{0};
        std::byte pad[kCacheLine]{};
    };

    // --- producer hot path -------------------------------------------------

    bool push(std::uint32_t id, const Event& e) noexcept {
        if (!accepting_.load(std::memory_order_acquire)) return false;
        ProducerSlot& slot = slots_[id];
        Ring& ring = *slot.ring;

        bool ok = ring.try_push(e);
        if (!ok && cfg_.full == FullPolicy::SpinThenDrop) {
            // Bounded spin, then give up. Never an unbounded wait.
            for (std::uint32_t i = 0; i < cfg_.push_spin && !ok; ++i) {
                WEIR_PAUSE();
                ok = ring.try_push(e);
            }
        }
        if (!ok) {
            slot.dropped.fetch_add(1, std::memory_order_relaxed);
            return false;   // loss is counted, never silent
        }
        slot.pushed.fetch_add(1, std::memory_order_relaxed);

        const std::uint64_t depth = ring.producer_size_hint();
        if (depth > slot.high_water.load(std::memory_order_relaxed))
            slot.high_water.store(depth, std::memory_order_relaxed);

        if (cfg_.scan == ScanPolicy::Bitmap) signal(id);
        return true;
    }

    // Tell the consumer this ring is non-empty — as rarely as possible.
    //
    // The bit is already set almost always (a streaming producer sets it once
    // and then never again), so the common case is a relaxed load of a line
    // that is Shared in every core's cache: an L1 hit, no coherence traffic.
    // Writes happen only on an empty->non-empty transition.
    void signal(std::uint32_t id) noexcept {
        const std::uint64_t bit = std::uint64_t{1} << id;

        // This fence is load-bearing and cannot be moved below the check.
        //
        // We have just published tail_ with a release store, and are about to
        // decide, from the bitmap, that the consumer already knows about us.
        // x86 is TSO, which reorders exactly one pair — StoreLoad — so without
        // a barrier this load may execute before the publish drains the store
        // buffer. Then: we read a set bit, skip the fetch_or, and return; the
        // consumer concurrently sees the ring empty (our store not yet
        // visible), clears the bit, re-checks, still sees empty, and parks. The
        // event is now published with its bit clear, so the consumer will not
        // even probe this ring, and it sits there until this producer happens
        // to push again — which it may never do.
        //
        // Release/acquire cannot express StoreLoad; this is the one ordering
        // TSO does not give away for free, and the only seq_cst in the design.
        // Checking the bit first and fencing only on the slow path — the
        // obvious optimization — reintroduces the bug exactly, because it is
        // the *unfenced check itself* that is unsound.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (active_.load(std::memory_order_relaxed) & bit) return;

        const std::uint64_t prev = active_.fetch_or(bit, std::memory_order_release);
        bitmap_writes_.fetch_add(1, std::memory_order_relaxed);

        // Coalesced notify: only the producer that took the whole bitmap from
        // zero wakes the consumer, so a burst across 8 producers costs one
        // notify_one rather than eight. A notify per push would be a syscall
        // per event.
        if (prev == 0 && parked_.load(std::memory_order_seq_cst)) {
            std::lock_guard lk(park_mu_);
            park_cv_.notify_one();
            notifies_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void retire(std::uint32_t id) noexcept {
        // Release so the consumer, on seeing Retiring, also sees our final
        // pushes. The slot stays unusable until the consumer publishes Free.
        slots_[id].state.store(SlotState::Retiring, std::memory_order_release);
        retire_pending_.store(true, std::memory_order_release);
        if (cfg_.scan == ScanPolicy::Bitmap) signal(id);  // make sure it gets drained
        wake_consumer();
    }

    // --- consumer ----------------------------------------------------------

    template <Sink S>
    void consume(S& sink) {
        std::uint32_t idle = 0;
        std::uint32_t rotate = 0;   // fairness: where each pass starts

        while (running_.load(std::memory_order_acquire)) {
            const std::size_t n = (cfg_.scan == ScanPolicy::Bitmap) ? drain_bitmap(sink, rotate)
                                                                    : drain_full(sink, rotate);
            passes_.fetch_add(1, std::memory_order_relaxed);
            rotate = (rotate + 1) & (kMaxProducers - 1);

            if (n > 0) { idle = 0; continue; }

            // Everything below runs only on a pass that found no work, so none
            // of it is on the hot path. Reclaiming retired slots is an O(64)
            // scan; doing it unconditionally would pay the full-scan cost on
            // every iteration and quietly cancel out the bitmap.
            if (retire_pending_.exchange(false, std::memory_order_acquire)) reclaim_retired();

            if (++idle < cfg_.spin_before_park) { WEIR_PAUSE(); continue; }
            if (idle < cfg_.spin_before_park * 2) { std::this_thread::yield(); continue; }

            // Clear stale bits only here, on the way to sleep — not on every
            // empty pass. A lightly loaded pipeline goes briefly empty between
            // arrivals, and clearing there would have the consumer clear a bit
            // the producer immediately sets again: write churn on the shared
            // line, precisely proportional to throughput, which is the thing
            // this design exists to avoid. Deferring it to the park path makes
            // bitmap writes proportional to idle transitions instead. It also
            // has to happen before park() checks the bitmap for emptiness, or a
            // stale bit would keep the consumer spinning forever.
            if (cfg_.scan == ScanPolicy::Bitmap) clear_empty_bits();

            // Defense in depth: one unconditional full scan immediately before
            // sleeping. The bitmap protocol above is believed correct, but the
            // cost of being wrong is an event stranded in a ring nobody probes,
            // and that is not a failure worth being clever about. Going to
            // sleep is the only moment where being wrong becomes unbounded, and
            // it is also the moment we can afford an O(64) scan.
            if (drain_full(sink, rotate) > 0) { idle = 0; continue; }
            park();
            idle = 0;
        }

        // Final pass: whatever is published and reachable still goes out, so a
        // Drain shutdown loses nothing. Full scan regardless of policy — at
        // shutdown, correctness beats the scan cost.
        for (int pass = 0; pass < 2; ++pass) {
            std::uint32_t r = 0;
            while (drain_full(sink, r) > 0) r = (r + 1) & (kMaxProducers - 1);
        }
        flush(sink);
        reclaim_retired();
        // A real socket sink would ::shutdown(fd, SHUT_WR) here so the peer
        // sees a clean EOF instead of a reset.
    }

    // O(64) every pass, however few producers are live. This is the naive
    // version the follow-up asks us to fix; kept so the benchmark can A/B it,
    // and used as the pre-sleep safety net above.
    template <Sink S>
    std::size_t drain_full(S& sink, std::uint32_t rotate) {
        std::size_t total = 0;
        for (std::uint32_t k = 0; k < kMaxProducers; ++k) {
            const std::uint32_t i = (k + rotate) & (kMaxProducers - 1);
            if (slots_[i].state.load(std::memory_order_relaxed) == SlotState::Free) continue;
            rings_probed_.fetch_add(1, std::memory_order_relaxed);
            const std::size_t n = drain_ring(sink, i);
            if (n == 0) rings_empty_.fetch_add(1, std::memory_order_relaxed);
            total += n;
        }
        if (total > 0) flush(sink);
        return total;
    }

    // O(active). std::rotr moves the scan's starting bit each pass so producer
    // 0 does not get drained first every time — under sustained overload a
    // fixed LSB-first order starves the high indices.
    template <Sink S>
    std::size_t drain_bitmap(S& sink, std::uint32_t rotate) {
        const std::uint64_t snapshot = active_.load(std::memory_order_acquire);
        std::uint64_t m = std::rotr(snapshot, static_cast<int>(rotate));
        std::size_t total = 0;

        while (m != 0) {
            const int off = std::countr_zero(m);
            m &= m - 1;
            const std::uint32_t i = (static_cast<std::uint32_t>(off) + rotate) & (kMaxProducers - 1);

            rings_probed_.fetch_add(1, std::memory_order_relaxed);
            const std::size_t n = drain_ring(sink, i);
            if (n == 0) rings_empty_.fetch_add(1, std::memory_order_relaxed);
            total += n;
        }
        if (total > 0) flush(sink);
        return total;
    }

    // A set bit means "maybe non-empty". The asymmetry is the whole point: a
    // false positive costs one wasted probe, a false negative loses a wakeup.
    // So bits stay set while there is work, and a busy producer's bit is
    // written once and then left alone — which is what keeps the bitmap line
    // read-mostly.
    void clear_empty_bits() noexcept {
        std::uint64_t m = active_.load(std::memory_order_acquire);
        while (m != 0) {
            const int i = std::countr_zero(m);
            m &= m - 1;
            const std::uint64_t bit = std::uint64_t{1} << i;
            if (!slots_[i].ring->empty_now()) continue;

            // The other half of the Dekker pair in signal(): clear first, then
            // look again. If a producer published before our clear, the
            // re-check sees its event; if it publishes after, it sees the
            // cleared bit and sets it itself. fetch_and is an RMW and so
            // already a full barrier — no explicit fence needed on this side.
            active_.fetch_and(~bit, std::memory_order_seq_cst);
            bitmap_writes_.fetch_add(1, std::memory_order_relaxed);
            if (!slots_[i].ring->empty_now() ||
                slots_[i].state.load(std::memory_order_acquire) == SlotState::Retiring) {
                active_.fetch_or(bit, std::memory_order_release);
                bitmap_writes_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    template <Sink S>
    std::size_t drain_ring(S& sink, std::uint32_t i) {
        std::size_t got = 0;
        while (got < kDrainBatch) {
            if (staged_ == stage_.size()) flush(sink);
            const std::size_t room = std::min(stage_.size() - staged_, kDrainBatch - got);
            const std::size_t n = slots_[i].ring->pop_batch(stage_.data() + staged_, room);
            if (n == 0) break;
            // Fire the hook on this batch *before* the next iteration can
            // flush and reset staged_ — otherwise the range is gone.
            if (on_event_) for (std::size_t k = 0; k < n; ++k) on_event_(stage_[staged_ + k]);
            staged_ += n;
            got += n;
        }
        consumed_.fetch_add(got, std::memory_order_relaxed);
        return got;
    }

    // One sink call per batch: the syscall a real socket would make is
    // amortized over up to kStageEvents events. Event is trivially copyable and
    // stage_ is contiguous, so the batch is handed over as a view — no
    // serialization copy on the way out.
    template <Sink S>
    void flush(S& sink) {
        if (staged_ == 0) return;
        sink.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(stage_.data()), staged_ * sizeof(Event)));
        staged_ = 0;
    }

    // Only the consumer publishes Free, and only after the ring is empty.
    void reclaim_retired() noexcept {
        for (std::uint32_t i = 0; i < kMaxProducers; ++i) {
            if (slots_[i].state.load(std::memory_order_acquire) != SlotState::Retiring) continue;
            if (!slots_[i].ring->empty_now()) continue;
            // Roll the departing producer's counters into the lifetime totals
            // before the slot is reusable — register_producer() resets the
            // per-slot ones, so without this a recycled slot erases its
            // predecessor's history and the accounting stops adding up.
            retired_pushed_.fetch_add(slots_[i].pushed.exchange(0, std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            retired_dropped_.fetch_add(slots_[i].dropped.exchange(0, std::memory_order_relaxed),
                                       std::memory_order_relaxed);
            active_.fetch_and(~(std::uint64_t{1} << i), std::memory_order_relaxed);
            slots_[i].state.store(SlotState::Free, std::memory_order_release);
        }
    }

    // Called from the stopping thread, which is neither producer nor consumer,
    // so it must use size_now() — producer_size_hint() reads the producer's
    // private cached index and would be a race here.
    bool all_quiesced() noexcept {
        for (auto& s : slots_) {
            if (s.state.load(std::memory_order_acquire) == SlotState::Free) continue;
            if (s.ring->size_now() != 0) return false;
        }
        return true;
    }

    void park() {
        std::unique_lock lk(park_mu_);
        parked_.store(true, std::memory_order_seq_cst);
        // Re-check after arming, or a producer that signalled between our last
        // scan and setting parked_ would find parked_ false, skip the notify,
        // and leave us asleep on an already-non-empty ring.
        if (active_.load(std::memory_order_seq_cst) == 0 &&
            running_.load(std::memory_order_acquire)) {
            parks_.fetch_add(1, std::memory_order_relaxed);
            // Always timed: a bounded worst case beats trusting the wakeup path
            // to be perfect.
            park_cv_.wait_for(lk, cfg_.park_timeout);
        }
        parked_.store(false, std::memory_order_seq_cst);
    }

    void wake_consumer() {
        std::lock_guard lk(park_mu_);
        park_cv_.notify_all();
    }

    Config cfg_;
    std::array<ProducerSlot, kMaxProducers> slots_{};

    // The push-path line. Both of these are read by every producer on every
    // push and written only on transitions, so co-locating them is deliberate:
    // a push touches one line, not two. Read sharing is free (the line sits
    // Shared in every core); only writes cost.
    //
    // What matters is not that they are together, it is what they are kept
    // *away* from — see the next group.
    alignas(kCacheLine) std::atomic<std::uint64_t> active_{0};
    std::atomic<bool> accepting_{false};

    // The consumer's written state, deliberately on its own line.
    //
    // This separation was missing and it was a real false-sharing bug, found
    // while porting this design to Go and checking the layout with
    // `clang -Xclang -fdump-record-layouts`. Only running_ carried the
    // alignas, so accepting_, parked_, retire_pending_ and park_mu_ all fell in
    // behind it on one line — the same line every producer read on every push.
    // retire_pending_ is the worst of them: the consumer exchange()s it, an RMW
    // that takes the line *exclusive*, on every idle pass, invalidating it in
    // all 64 producer cores. A design whose §3 is about false sharing contained
    // false sharing, in the one place a layout dump would find it and a test
    // never would.
    alignas(kCacheLine) std::atomic<bool> running_{false};
    std::atomic<bool> parked_{false};
    std::atomic<bool> retire_pending_{false};
    std::mutex park_mu_;
    std::condition_variable park_cv_;

    // Consumer-local; no other thread touches these.
    std::vector<Event> stage_;
    std::size_t staged_{0};
    std::thread consumer_;
    bool drain_deadline_met_{false};

    alignas(kCacheLine) std::atomic<std::uint64_t> consumed_{0};
    std::atomic<std::uint64_t> retired_pushed_{0};   // counters of departed producers
    std::atomic<std::uint64_t> retired_dropped_{0};
    std::atomic<std::uint64_t> rings_probed_{0};
    std::atomic<std::uint64_t> rings_empty_{0};
    std::atomic<std::uint64_t> passes_{0};
    std::atomic<std::uint64_t> bitmap_writes_{0};
    std::atomic<std::uint64_t> notifies_{0};
    std::atomic<std::uint64_t> parks_{0};
};

inline bool ProducerHandle::push(const Event& e) noexcept {
    return pipe_ && pipe_->push(id_, e);
}

inline void ProducerHandle::release() noexcept {
    if (pipe_) { pipe_->retire(id_); pipe_ = nullptr; }
}

}  // namespace weir
