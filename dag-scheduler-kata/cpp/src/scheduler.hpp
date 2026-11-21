#pragma once
//
// The DAG executor.
//
// One worker thread per deque. A worker runs what it popped from its own deque
// (LIFO), else a new instance from the shared injection queue, else what it can
// steal (FIFO) from another worker; and when it finds nothing, it either keeps
// looking or sleeps, which is the policy the follow-up exercise is about.
//
// The memory-ordering contract, in one place, because it is the kata:
//
//   1. A node's body writes its payload with plain stores.
//   2. It then resolves each out-edge by decrementing the successor's counter.
//      Every decrement is a RELEASE: it publishes this predecessor's writes.
//   3. The decrement that takes the counter to zero must also ACQUIRE: it is
//      the one thread that then owns the successor, and it must have seen every
//      other predecessor's writes before it runs the successor or hands it on.
//      All the decrements of one counter form a release sequence, so one
//      acquire by the last synchronizes with every earlier release.
//   4. Handing the successor to another worker is a second publication: the
//      deque's release push and acquire steal carry it. Happens-before is
//      transitive, so the thief sees every predecessor's payload.
//   5. After its last decrement a predecessor must not touch the instance
//      again, unless that decrement made a node ready and so pinned the
//      instance alive: the instance can complete, and be reused, the moment the
//      last edge into the sink resolves.
//
// The brief's relaxed decrement breaks 2 and 3. On x86-64 that compiles to the
// same `lock` instruction as the correct version, so no test on this machine
// can see the bug; ThreadSanitizer can (solution.md §2).

#include "deque.hpp"
#include "eventcount.hpp"
#include "graph.hpp"
#include "histogram.hpp"
#include "platform.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace keystone {

// How a predecessor decrements its successor's dependency counter.
enum class DecOrder {
    relaxed,        // the brief: wrong — publishes nothing, acquires nothing
    release_only,   // wrong: publishes, but the last one does not acquire
    acquire_only,   // wrong: the last one acquires, but nobody published
    acq_rel,        // correct: every decrement both releases and acquires
    release_fence,  // correct: release always, acquire fence only in the last one
    release_load,   // correct: release always, acquire load of the counter in the last one
    seq_cst,        // correct, and stronger than needed
};

inline const char* name(DecOrder o) {
    switch (o) {
    case DecOrder::relaxed: return "relaxed";
    case DecOrder::release_only: return "release-only";
    case DecOrder::acquire_only: return "acquire-only";
    case DecOrder::acq_rel: return "acq_rel";
    case DecOrder::release_fence: return "release+fence";
    case DecOrder::release_load: return "release+load";
    case DecOrder::seq_cst: return "seq_cst";
    }
    return "?";
}

// Returns true if this decrement was the last. Every branch spells its ordering
// as a constant: GCC treats a memory_order that is not a compile-time constant
// as seq_cst, which would quietly repair the very bugs this exists to show.
inline bool resolve_edge(std::atomic<std::uint32_t>& pending, DecOrder o) noexcept {
    switch (o) {
    case DecOrder::relaxed: return pending.fetch_sub(1, std::memory_order_relaxed) == 1;
    case DecOrder::release_only: return pending.fetch_sub(1, std::memory_order_release) == 1;
    case DecOrder::acquire_only: return pending.fetch_sub(1, std::memory_order_acquire) == 1;
    case DecOrder::acq_rel: return pending.fetch_sub(1, std::memory_order_acq_rel) == 1;
    case DecOrder::seq_cst: return pending.fetch_sub(1, std::memory_order_seq_cst) == 1;
    case DecOrder::release_fence:
        // The shared_ptr idiom. Correct, and invisible to ThreadSanitizer, which
        // does not model standalone fences: it reports this as a race.
        if (pending.fetch_sub(1, std::memory_order_release) != 1) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        return true;
    case DecOrder::release_load:
        // The same contract without a fence. The acquire load reads the value
        // our own decrement wrote, which is in the release sequence headed by
        // every earlier predecessor's release decrement — so it synchronizes
        // with all of them. Only the last predecessor pays for it.
        if (pending.fetch_sub(1, std::memory_order_release) != 1) return false;
        (void)pending.load(std::memory_order_acquire);
        return true;
    }
    return false;
}

// What an idle worker does.
enum class Idle {
    greedy,    // keeps trying to steal until work appears; never sleeps
    budgeted,  // at most `max_searchers` look for work at once; the rest sleep
};

// What a node's failure does to the rest of its instance.
enum class FailPolicy {
    poison,  // its descendants are skipped; independent branches still run
    cancel,  // fail-fast: every node not yet started is skipped
};

struct Config {
    int      workers = 8;
    DecOrder order   = DecOrder::acq_rel;
    // A predecessor that makes a successor ready runs it itself, instead of
    // pushing it and popping it back: no deque traffic, no wake, warm cache.
    bool inline_continuation = true;
    // Resolve edges in ascending rank, so the successor continued inline (or
    // pushed last, and so popped first) is the one with the most work behind it.
    bool       priority      = true;
    Idle       idle          = Idle::budgeted;
    int        max_searchers = 0;  // 0: half the workers, at least one
    FailPolicy fail          = FailPolicy::poison;
    // A successor with one predecessor needs no atomic at all: program order
    // already puts its only input before it.
    bool          skip_single_pred_rmw = true;
    std::size_t   deque_capacity       = 1 << 14;
    std::uint32_t injection_interval   = 61;  // check the shared queue this often even when busy
    std::chrono::microseconds park_backstop{2000};
    // Deliberate bugs for the C++ track's tool table.
    bool relaxed_publish = false;  // the deque's push publishes nothing
    bool double_resolve  = false;  // a failed node resolves its out-edges twice
};

struct alignas(kCacheLine) Stats {
    std::uint64_t tasks = 0, skipped = 0, inline_runs = 0, pushes = 0, push_full = 0;
    std::uint64_t steal_attempts = 0, steals = 0, steal_lost = 0, injected = 0;
    std::uint64_t parks = 0, rescued = 0, wakes = 0;
    std::uint64_t violations = 0;  // a payload read before it was written, or a body run twice
    std::uint64_t bad_completions = 0;  // an instance completed with a node unresolved, or resolved twice
    std::uint64_t joins = 0, cross_joins = 0;  // joins checked; of them, with predecessors on different workers
    std::uint64_t completed = 0, failed_instances = 0;
    std::uint64_t wasted_after_failure = 0;  // bodies started after their instance had failed
    std::uint64_t busy_ticks = 0;
    Histogram     latency;          // completion - due, in TSC ticks
    Histogram     fail_to_done;     // completion - first failure, in TSC ticks

    void merge(const Stats& o) {
        tasks += o.tasks, skipped += o.skipped, inline_runs += o.inline_runs, pushes += o.pushes;
        push_full += o.push_full, steal_attempts += o.steal_attempts, steals += o.steals;
        steal_lost += o.steal_lost, injected += o.injected, parks += o.parks, rescued += o.rescued;
        wakes += o.wakes, violations += o.violations, bad_completions += o.bad_completions;
        joins += o.joins, cross_joins += o.cross_joins;
        completed += o.completed, failed_instances += o.failed_instances;
        wasted_after_failure += o.wasted_after_failure, busy_ticks += o.busy_ticks;
        latency.merge(o.latency);
        fail_to_done.merge(o.fail_to_done);
    }
};

// Called on the worker that completed an instance's sink, which is the only
// thread holding the instance at that moment: it may reset and resubmit it.
using OnComplete = void (*)(void* ctx, Instance& inst, int worker);

class Scheduler {
public:
    Scheduler(const Config& cfg, OnComplete done, void* ctx)
        : cfg_(cfg), done_(done), ctx_(ctx), workers_(static_cast<std::size_t>(cfg.workers)) {
        max_searchers_ = cfg.max_searchers > 0 ? cfg.max_searchers : std::max(1, cfg.workers / 2);
        for (auto& w : workers_) w.deque = std::make_unique<WsDeque<NodeState>>(cfg.deque_capacity);
        inject_ring_.resize(kInjectCap);
    }
    ~Scheduler() { stop(); }
    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start() {
        for (int i = 0; i < cfg_.workers; ++i) {
            workers_[static_cast<std::size_t>(i)].rng.seed(0x5eed + static_cast<unsigned>(i));
            threads_.emplace_back([this, i] { run(i); });
        }
    }

    void stop() {
        if (threads_.empty()) return;
        stop_.store(true, std::memory_order_seq_cst);
        ec_.notify_all();
        for (auto& t : threads_) t.join();
        threads_.clear();
    }

    // Any thread. Hands an instance to the scheduler through its source node.
    // False if the injection queue is full: the caller is told, not blocked.
    bool submit(Instance& inst) {
        {
            std::lock_guard lk(inject_mu_);
            if (inject_tail_ - inject_head_ == kInjectCap) return false;
            inject_ring_[inject_tail_++ % kInjectCap] = &inst.nodes[inst.shape->source];
            inject_count_.store(inject_tail_ - inject_head_, std::memory_order_relaxed);
        }
        notify_work();
        return true;
    }

    Stats totals() const {
        Stats s;
        for (const auto& w : workers_) s.merge(w.st);
        return s;
    }
    const Config& config() const noexcept { return cfg_; }
    int           max_searchers() const noexcept { return max_searchers_; }

private:
    static constexpr std::size_t kInjectCap = 1 << 16;

    struct alignas(kCacheLine) Worker {
        std::unique_ptr<WsDeque<NodeState>> deque;
        std::minstd_rand                    rng;
        std::uint32_t                       tick = 0;
        Stats                               st;
    };

    // ---------------------------------------------------------------- the loop

    void run(int self) {
        Worker& me = workers_[static_cast<std::size_t>(self)];
        while (!stop_.load(std::memory_order_relaxed)) {
            NodeState* x = nullptr;
            // Even a worker that never runs dry looks at the shared queue now and
            // then; otherwise new instances wait for an idle worker to exist.
            if (++me.tick % cfg_.injection_interval == 0) x = take_injected(me);
            if (!x) x = me.deque->pop();
            if (!x) x = take_injected(me);
            if (!x) x = cfg_.idle == Idle::greedy ? find_work_greedy(me, self) : find_work_budgeted(me, self);
            if (x) execute(me, x);
        }
    }

    NodeState* take_injected(Worker& me) {
        if (inject_count_.load(std::memory_order_relaxed) == 0) return nullptr;
        std::lock_guard lk(inject_mu_);
        if (inject_head_ == inject_tail_) return nullptr;
        NodeState* x = inject_ring_[inject_head_++ % kInjectCap];
        inject_count_.store(inject_tail_ - inject_head_, std::memory_order_relaxed);
        ++me.st.injected;
        return x;
    }

    NodeState* try_steal(Worker& me, int self, int victim) {
        if (victim == self) return nullptr;
        ++me.st.steal_attempts;
        NodeState* x = nullptr;
        switch (workers_[static_cast<std::size_t>(victim)].deque->steal(x)) {
        case WsDeque<NodeState>::Steal::ok: ++me.st.steals; return x;
        case WsDeque<NodeState>::Steal::lost: ++me.st.steal_lost; return nullptr;
        case WsDeque<NodeState>::Steal::empty: return nullptr;
        }
        return nullptr;
    }

    // The naive idle loop: random victims, a pause between attempts, a yield
    // after each sweep's worth of misses, and never a sleep. Every idle worker
    // is a thief, all the time.
    NodeState* find_work_greedy(Worker& me, int self) {
        const int w = cfg_.workers;
        for (int miss = 0;; ++miss) {
            if (stop_.load(std::memory_order_relaxed)) return nullptr;
            if (NodeState* x = take_injected(me)) return x;
            if (NodeState* x = try_steal(me, self, static_cast<int>(me.rng() % static_cast<unsigned>(w)))) return x;
            KEYSTONE_PAUSE();
            if (miss % w == w - 1) std::this_thread::yield();
        }
    }

    // The budgeted idle loop. At most max_searchers_ workers look for work at
    // once; a worker that cannot become one sleeps at once. A searcher that
    // finds work and was the last one searching wakes a sleeper to take its
    // place, because where there was one task there may be more — so wakes
    // propagate one at a time, as fast as work is found, and no faster.
    NodeState* find_work_budgeted(Worker& me, int self) {
        const int w = cfg_.workers;
        for (;;) {
            if (stop_.load(std::memory_order_relaxed)) return nullptr;
            int  s        = searching_.load(std::memory_order_relaxed);
            bool searcher = false;
            while (s < max_searchers_)
                if (searching_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel)) {
                    searcher = true;
                    break;
                }
            if (searcher) {
                NodeState* found = nullptr;
                for (int round = 0; round < kSearchRounds && !found; ++round) {
                    if ((found = take_injected(me))) break;
                    const int start = static_cast<int>(me.rng() % static_cast<unsigned>(w));
                    for (int i = 0; i < w && !found; ++i) found = try_steal(me, self, (start + i) % w);
                    if (!found)
                        for (int p = 0; p < 32; ++p) KEYSTONE_PAUSE();
                }
                const bool last = searching_.fetch_sub(1, std::memory_order_seq_cst) == 1;
                if (found) {
                    if (last && ec_.notify_one()) ++me.st.wakes;
                    return found;
                }
            }
            // Sleep: announce, re-check everything, and only then block.
            const auto key = ec_.prepare_wait();
            if (stop_.load(std::memory_order_seq_cst) || work_visible()) {
                ec_.cancel_wait();
                continue;
            }
            ++me.st.parks;
            if (!ec_.wait(key, Clock::now() + cfg_.park_backstop) && work_visible() &&
                searching_.load(std::memory_order_seq_cst) == 0)
                ++me.st.rescued;  // slept through work with nobody looking: a wake went missing
        }
    }

    bool work_visible() const {
        if (inject_count_.load(std::memory_order_seq_cst) != 0) return true;
        for (const auto& w : workers_)
            if (!w.deque->looks_empty()) return true;
        return false;
    }

    // Called after making work visible. Under the budgeted policy a sleeper is
    // woken only if nobody is already searching: a searcher will find this work,
    // and waking another would only add a thief.
    void notify_work() {
        if (cfg_.idle == Idle::greedy) return;  // nobody sleeps
        std::atomic_thread_fence(std::memory_order_seq_cst);  // publish, then look
        if (searching_.load(std::memory_order_relaxed) == 0) ec_.notify_one(false);
    }

    void push(Worker& me, NodeState* x) {
        if (me.deque->push(x, cfg_.relaxed_publish)) {
            ++me.st.pushes;
            return;
        }
        // Full: the work-first overflow. Running it here bounds the deque at the
        // cost of this worker's stack depth, which the deque size bounds in turn.
        ++me.st.push_full;
        execute(me, x);
    }

    // ---------------------------------------------------------------- one task

    void execute(Worker& me, NodeState* nd) {
        while (nd) {
            Instance&     g      = *nd->inst;
            const Shape&  s      = *g.shape;
            const auto    id     = nd->id;
            const bool    verify = g.verify;
            const bool    cancelled_before =
                cfg_.fail == FailPolicy::cancel && g.cancelled.load(std::memory_order_relaxed);
            const bool skip = nd->poisoned.load(std::memory_order_relaxed) != 0 || cancelled_before;
            bool       ok   = true;
            if (skip) {
                ++me.st.skipped;
            } else {
                ok = body(me, g, *nd);
                if (!ok) {
                    std::uint64_t zero = 0;
                    g.failed_at_tsc.compare_exchange_strong(zero, TscClock::now(), std::memory_order_relaxed);
                    g.cancelled.store(true, std::memory_order_relaxed);
                }
            }
            if (verify) nd->resolved.fetch_add(1, std::memory_order_relaxed);

            if (id == s.sink) {
                complete(me, g);
                return;
            }

            // Resolve every out-edge. Everything this node touches in the instance
            // has to happen before its last decrement — see the contract, item 5.
            const bool poison = skip || !ok;
            const auto succs  = cfg_.priority ? s.successors_by_rank(id) : s.successors(id);
            if (poison && cfg_.double_resolve)
                for (auto j : succs)
                    if (s.in_degree(j) > 1) (void)resolve_edge(g.nodes[j].pending, cfg_.order);
            NodeState* next   = nullptr;
            bool       pushed = false;
            for (auto j : succs) {
                NodeState& sn = g.nodes[j];
                if (poison) sn.poisoned.store(1, std::memory_order_relaxed);
                const bool ready = (cfg_.skip_single_pred_rmw && s.in_degree(j) == 1) ||
                                   resolve_edge(sn.pending, cfg_.order);
                if (!ready) continue;
                // Keep the latest ready successor; push the one it displaces. With
                // priority on, successors arrive in ascending rank, so what is kept
                // at the end is the one with the most work behind it.
                if (next) {
                    push(me, next);
                    if (!pushed) notify_work();  // the first push: let a thief start now
                    pushed = true;
                }
                next = &sn;
            }
            if (!next) return;  // nothing became ready: the instance may already be gone
            if (cfg_.inline_continuation) {
                ++me.st.inline_runs;
            } else {
                push(me, next);
                next = nullptr;
            }
            if (pushed || !next) notify_work();
            nd = next;
        }
    }

    bool body(Worker& me, Instance& g, NodeState& nd) {
        const Shape& s  = *g.shape;
        const auto   id = nd.id;
        ++me.st.tasks;
        if (g.cancelled.load(std::memory_order_relaxed)) ++me.st.wasted_after_failure;
        // Read every input. These are plain loads of other threads' plain stores:
        // safe only because of the edges the decrements built.
        std::uint64_t acc = 0;
        if (g.verify) {
            if (nd.runs.fetch_add(1, std::memory_order_relaxed) != 0) ++me.st.violations;
            for (auto p : s.predecessors(id))
                if (g.nodes[p].out != g.expect[p]) ++me.st.violations;
        } else {
            for (auto p : s.predecessors(id)) acc += g.nodes[p].out;
        }
        const auto w = s.work[id];
        if (w) {
            burn(w);
            me.st.busy_ticks += w;
        }
        if (id == g.fail_node) return false;
        nd.out = g.verify ? g.expect[id] : acc + 1;
        if (g.verify) nd.ran_on = static_cast<std::uint16_t>(&me - workers_.data());
        return true;
    }

    void complete(Worker& me, Instance& g) {
        const auto now = TscClock::now();
        ++me.st.completed;
        const auto failed_at = g.failed_at_tsc.load(std::memory_order_relaxed);
        if (failed_at) {
            ++me.st.failed_instances;
            me.st.fail_to_done.record(now - failed_at);
        }
        if (g.due_tsc) me.st.latency.record(now > g.due_tsc ? now - g.due_tsc : 0);
        if (g.verify) {
            const Shape& s = *g.shape;
            for (std::uint32_t i = 0; i < s.n; ++i) {
                const auto& nd = g.nodes[i];
                if (nd.resolved.load(std::memory_order_relaxed) != 1) ++me.st.bad_completions;
                const auto runs = nd.runs.load(std::memory_order_relaxed);
                const auto want = g.expected_runs(i);
                if (cfg_.fail == FailPolicy::poison ? runs != want : runs > want) ++me.st.bad_completions;
                if (runs == 1 && s.in_degree(i) > 1) {
                    ++me.st.joins;
                    const auto preds = s.predecessors(i);
                    for (auto p : preds)
                        if (g.nodes[p].ran_on != g.nodes[preds[0]].ran_on) {
                            ++me.st.cross_joins;
                            break;
                        }
                }
            }
        }
        done_(ctx_, g, static_cast<int>(&me - workers_.data()));
    }

    static constexpr int kSearchRounds = 4;

    Config      cfg_;
    OnComplete  done_;
    void*       ctx_;
    int         max_searchers_ = 1;
    std::vector<Worker>      workers_;
    std::vector<std::thread> threads_;

    alignas(kCacheLine) std::atomic<bool> stop_{false};
    alignas(kCacheLine) std::atomic<int> searching_{0};
    EventCount ec_;

    alignas(kCacheLine) std::mutex inject_mu_;
    std::vector<NodeState*>        inject_ring_;
    std::size_t                    inject_head_ = 0, inject_tail_ = 0;
    alignas(kCacheLine) std::atomic<std::size_t> inject_count_{0};
};

}  // namespace keystone
