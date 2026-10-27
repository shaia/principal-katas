#pragma once
//
// The multi-threaded run every phase is built from: P producers and C consumers
// on one queue, released together, each counting into its own state and merged
// afterwards. Nothing in the measured loops is shared except the queue.

#include "config.hpp"
#include "event.hpp"
#include "harness.hpp"
#include "histogram.hpp"
#include "platform.hpp"
#include "queue_api.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace turnstile::bench {

// mixed: even producers reject on full, odd ones block — both reservation styles
// on one tail word at once.
enum class PushMode { block, reject, mixed };

struct RunSpec {
    int producers = kProducers;
    int consumers = kConsumers;

    // Saturated when rate == 0: each producer pushes `events` as fast as it can.
    // With events == 0 a producer pushes until the queue refuses it as closed,
    // which needs close_after.
    std::uint64_t events = 250'000;

    // Paced when rate > 0: `rate` events/s in aggregate for `seconds`. Producers
    // wake on a timer and push whatever has come due — the way a reader of a
    // kernel ring buffer does — so they sleep between bursts instead of
    // spinning, and 32 of them do not occupy 32 CPUs doing nothing.
    double rate    = 0;
    double seconds = 0;

    double      consumer_work_ns = 0;
    PushMode    mode             = PushMode::block;
    std::size_t batch            = 1;  // > 1: push_batch / pop_batch, on a queue that has them
    bool        latency          = false;
    Nanos       block_timeout    = std::chrono::seconds(2);

    // Close the queue this long after the start, with producers still running,
    // instead of after they have finished.
    Nanos close_after = Nanos::zero();
};

struct RunResult {
    std::uint64_t offered = 0, accepted = 0, rejected = 0, refused_closed = 0;
    std::uint64_t consumed = 0, torn = 0, disorder = 0;
    bool          exactly_once = false;

    std::uint64_t t_go       = 0;  // TSC at release
    std::uint64_t t_close    = 0;  // TSC when close() was called
    std::uint64_t t_end      = 0;  // TSC of the last successful pop
    std::uint64_t first_full = 0;  // TSC of the first full or timeout any producer saw; 0 if none
    std::uint64_t backlog_at_close = 0;
    double        seconds          = 0;  // release to last pop
    double        rate             = 0;  // consumed / seconds

    Histogram push;   // one push call, ticks
    Histogram ready;  // from when the event could first have been pushed to accepted, ticks
    Histogram e2e;    // push start to pop, ticks (cross-core: phase 0 checks the TSC skew)
    Histogram window; // reserve to publish, ticks — only from a queue built with measure_window

    OpStats     producer_ops, consumer_ops;
    double      min_producer_rate = 0, max_producer_rate = 0;
    std::size_t producers_unfinished = 0;  // producers that ended on something other than their quota
    std::uint64_t bursts = 0;              // paced: wake-ups that found events due
};

namespace detail {

struct alignas(kCacheLine) ProducerState {
    Tally         accepted;
    std::uint64_t offered = 0, rejected = 0, refused_closed = 0;
    std::uint64_t t_done = 0, first_full = 0;
    bool          finished_quota = false;
    std::uint64_t bursts         = 0;
    Histogram     push, ready, window;
    OpStats       ops;
};

struct alignas(kCacheLine) ConsumerState {
    Tally                         consumed;
    std::uint64_t                 torn = 0, disorder = 0, t_last = 0;
    std::array<std::int64_t, 128> last_seq;
    Histogram                     e2e;
    OpStats                       ops;
};

template <typename Q>
Push push_one(Q& q, Event&& e, PushMode mode, Clock::time_point until, OpStats& st) {
    if constexpr (requires { q.try_push(std::move(e), st); }) {
        if (mode == PushMode::reject) return q.try_push(std::move(e), st);
    }
    return q.push(std::move(e), until, st);
}

template <typename Q>
constexpr bool has_batch = requires(Q& q, Event* e, OpStats& st) {
    q.push_batch(e, std::size_t{1}, Clock::now(), st);
    q.pop_batch(e, std::size_t{1}, Clock::now(), st);
};

inline void account(ProducerState& s, Push r, std::uint32_t id, std::uint32_t seq, std::uint64_t now) {
    ++s.offered;
    switch (r) {
        case Push::ok: s.accepted.add(id, seq); break;
        case Push::closed: ++s.refused_closed; break;
        case Push::full:
        case Push::timeout:
            ++s.rejected;
            if (s.first_full == 0) s.first_full = now;
            break;
    }
}

}  // namespace detail

template <typename Q>
RunResult run(Q& q, const RunSpec& spec, const Context& ctx) {
    using namespace detail;
    const int P = spec.producers, C = spec.consumers;
    auto      ps = std::make_unique<ProducerState[]>(static_cast<std::size_t>(P));
    auto      cs = std::make_unique<ConsumerState[]>(static_cast<std::size_t>(std::max(C, 1)));

    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};
    auto              wait_go = [&] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    };

    const std::uint64_t work = spec.consumer_work_ns > 0 ? ctx.clk.ticks(spec.consumer_work_ns) : 0;
    const bool          paced = spec.rate > 0;
    const std::uint64_t interval =
        paced ? ctx.clk.ticks(1e9 * static_cast<double>(P) / spec.rate) : 0;
    const std::uint64_t duration = paced ? ctx.clk.ticks(spec.seconds * 1e9) : 0;
    std::uint64_t       t_go     = 0;
    std::atomic<std::uint64_t> t_go_shared{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(P + C));

    for (int id = 0; id < P; ++id) {
        threads.emplace_back([&, id] {
            ProducerState&      s   = ps[static_cast<std::size_t>(id)];
            const auto          pid = static_cast<std::uint32_t>(id);
            const PushMode      mode =
                spec.mode == PushMode::mixed ? (id % 2 ? PushMode::block : PushMode::reject) : spec.mode;
            std::vector<Event>  batch(spec.batch);
            s.ops.window = &s.window;
            wait_go();
            const std::uint64_t start = t_go_shared.load(std::memory_order_acquire);
            Clock::time_point   until = Clock::now() + spec.block_timeout;

            auto push_timed = [&](std::uint32_t seq, std::uint64_t ready_at) -> Push {
                const std::uint64_t t0 = TscClock::now();
                Push r = push_one(q, make_event(pid, seq, t0), mode, until, s.ops);
                const std::uint64_t t1 = TscClock::now();
                if (spec.latency) {
                    s.push.record(t1 - t0);
                    s.ready.record(t1 - std::min(ready_at, t0));
                }
                account(s, r, pid, seq, t1);
                return r;
            };

            if (!paced) {
                const std::uint64_t quota = spec.events ? spec.events : std::numeric_limits<std::uint32_t>::max();
                for (std::uint64_t i = 0; i < quota;) {
                    if ((i & 1023) == 0) until = Clock::now() + spec.block_timeout;
                    if constexpr (has_batch<Q>) {
                        if (spec.batch > 1) {
                            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(spec.batch, quota - i));
                            const std::uint64_t t0 = TscClock::now();
                            for (std::size_t k = 0; k < n; ++k)
                                batch[k] = make_event(pid, static_cast<std::uint32_t>(i + k), t0);
                            const std::size_t got = q.push_batch(batch.data(), n, until, s.ops);
                            const std::uint64_t t1 = TscClock::now();
                            if (spec.latency) s.push.record(t1 - t0);
                            for (std::size_t k = 0; k < n; ++k)
                                account(s, k < got ? Push::ok : (q.closed() ? Push::closed : Push::timeout),
                                        pid, static_cast<std::uint32_t>(i + k), t1);
                            i += n;
                            if (got < n && q.closed()) break;
                            continue;
                        }
                    }
                    const Push r = push_timed(static_cast<std::uint32_t>(i), TscClock::now());
                    ++i;
                    if (r == Push::closed) break;
                }
                s.finished_quota = spec.events != 0 && s.offered == spec.events;
            } else {
                // Staggered starts, so 32 producers do not arrive in lockstep.
                std::uint64_t next = start + interval * static_cast<std::uint64_t>(id) / static_cast<std::uint64_t>(P);
                const std::uint64_t end  = start + duration;
                std::uint64_t       woke = start;
                std::uint32_t       seq  = 0;
                bool                open = true;
                while (open && next < end) {
                    std::uint64_t now = TscClock::now();
                    if (now < next) {
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        woke = TscClock::now();
                        continue;
                    }
                    until = Clock::now() + spec.block_timeout;
                    ++s.bursts;
                    while (next <= now && next < end) {
                        const Push r = push_timed(seq++, std::max(next, woke));
                        next += interval;
                        now = TscClock::now();
                        if (r == Push::closed) { open = false; break; }
                    }
                }
                s.finished_quota = open;
            }
            s.t_done = TscClock::now();
        });
    }

    for (int id = 0; id < C; ++id) {
        threads.emplace_back([&, id] {
            ConsumerState& s = cs[static_cast<std::size_t>(id)];
            s.last_seq.fill(-1);
            std::vector<Event> buf(std::max<std::size_t>(spec.batch, 1));
            const auto forever = Clock::now() + std::chrono::hours(1);
            wait_go();

            auto take = [&](const Event& ev) {
                const std::uint64_t t = TscClock::now();
                s.t_last = t;
                if (!intact(ev) || ev.producer >= s.last_seq.size()) {
                    ++s.torn;
                    return;
                }
                if (spec.latency) s.e2e.record(t - ev.stamp);
                s.consumed.add(ev.producer, ev.seq);
                // One consumer takes positions in increasing order, and a producer's
                // events occupy increasing positions — so from any one consumer's
                // point of view, each producer's sequence must only go up.
                if (static_cast<std::int64_t>(ev.seq) <= s.last_seq[ev.producer]) ++s.disorder;
                s.last_seq[ev.producer] = ev.seq;
                if (work) burn(work);
            };

            for (;;) {
                if constexpr (has_batch<Q>) {
                    if (spec.batch > 1) {
                        const std::size_t n = q.pop_batch(buf.data(), buf.size(), forever, s.ops);
                        if (n == 0) break;
                        for (std::size_t k = 0; k < n; ++k) take(buf[k]);
                        continue;
                    }
                }
                Event     ev;
                const Pop r = q.pop(ev, forever, s.ops);
                if (r == Pop::drained) break;
                if (r == Pop::ok) take(ev);
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < P + C) std::this_thread::yield();
    t_go = TscClock::now();
    t_go_shared.store(t_go, std::memory_order_release);
    go.store(true, std::memory_order_release);

    RunResult out;
    out.t_go = t_go;
    if (spec.close_after > Nanos::zero()) {
        std::this_thread::sleep_for(spec.close_after);
        if constexpr (requires { q.reserved(); }) out.backlog_at_close = q.reserved() - q.claimed();
        out.t_close = TscClock::now();
        q.close();
        for (auto& t : threads) t.join();
    } else {
        for (int i = 0; i < P; ++i) threads[static_cast<std::size_t>(i)].join();
        if constexpr (requires { q.reserved(); }) out.backlog_at_close = q.reserved() - q.claimed();
        out.t_close = TscClock::now();
        q.close();
        for (std::size_t i = static_cast<std::size_t>(P); i < threads.size(); ++i) threads[i].join();
    }

    Tally accepted, consumed;
    out.min_producer_rate = std::numeric_limits<double>::max();
    for (int i = 0; i < P; ++i) {
        const ProducerState& s = ps[static_cast<std::size_t>(i)];
        accepted.merge(s.accepted);
        out.offered += s.offered;
        out.rejected += s.rejected;
        out.refused_closed += s.refused_closed;
        out.push.merge(s.push);
        out.ready.merge(s.ready);
        out.window.merge(s.window);
        out.producer_ops.merge(s.ops);
        if (!s.finished_quota) ++out.producers_unfinished;
        out.bursts += s.bursts;
        if (s.first_full && (out.first_full == 0 || s.first_full < out.first_full)) out.first_full = s.first_full;
        const double secs = ctx.clk.to_ns(s.t_done - t_go) / 1e9;
        const double r    = secs > 0 ? static_cast<double>(s.accepted.n) / secs : 0;
        out.min_producer_rate = std::min(out.min_producer_rate, r);
        out.max_producer_rate = std::max(out.max_producer_rate, r);
    }
    for (int i = 0; i < C; ++i) {
        const ConsumerState& s = cs[static_cast<std::size_t>(i)];
        consumed.merge(s.consumed);
        out.torn += s.torn;
        out.disorder += s.disorder;
        out.e2e.merge(s.e2e);
        out.consumer_ops.merge(s.ops);
        out.t_end = std::max(out.t_end, s.t_last);
    }
    out.accepted     = accepted.n;
    out.consumed     = consumed.n;
    out.exactly_once = C > 0 && accepted == consumed && out.torn == 0;
    out.seconds      = out.t_end > t_go ? ctx.clk.to_ns(out.t_end - t_go) / 1e9 : 0;
    out.rate         = out.seconds > 0 ? static_cast<double>(out.consumed) / out.seconds : 0;
    return out;
}

// The invariants every run must satisfy, under shared labels.
inline void check_run(const RunResult& r, const std::string& label) {
    check(r.offered == r.accepted + r.rejected + r.refused_closed, label + ": offered == accepted + rejected + refused");
    check(r.torn == 0, label + ": no event read torn or unpublished (" + std::to_string(r.torn) + ")");
    check(r.exactly_once, label + ": every accepted event consumed exactly once (accepted " +
                              std::to_string(r.accepted) + ", consumed " + std::to_string(r.consumed) + ")");
    check(r.disorder == 0, label + ": per consumer, each producer's events arrive in order (" +
                               std::to_string(r.disorder) + " out of order)");
}

}  // namespace turnstile::bench
