#pragma once
//
// The graph, split in two along the line that decides what may be shared.
//
// A Shape is the DAG itself: edges in both directions, each node's cost, and
// its rank. It is immutable once built and shared by every run of it, so the
// scheduler reads successor lists without any synchronization at all — which
// matters, because the last thing a task does is walk its successor list, and
// by then the run it belongs to may already be finishing on another core.
//
// An Instance is one run of a Shape: a dependency counter per node, a poison
// flag per node, the payload each node produces, and the bookkeeping that lets
// the benchmark check every invariant afterwards. Instances are pooled and
// reset, never allocated on the hot path, so "bounded memory" is a property of
// the pool size rather than of the arrival rate.

#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace keystone {

inline constexpr std::uint32_t kNone = UINT32_MAX;

struct Shape {
    std::uint32_t n      = 0;
    std::uint32_t source = 0;  // the single root: the instance enters the scheduler through it
    std::uint32_t sink   = 0;  // the single leaf: its completion is the instance's completion

    // Compressed adjacency, both directions.
    std::vector<std::uint32_t> succ_off, succ, pred_off, pred;
    // Each successor list again, ascending by rank: the order a priority
    // scheduler resolves edges in, so that the last ready successor it finds is
    // the one with the most work behind it.
    std::vector<std::uint32_t> succ_by_rank;
    std::vector<std::uint64_t> work;  // body cost, in TSC ticks
    // Bottom level: the costliest path from this node to the sink, inclusive, in
    // ticks. A node with a larger rank has more work behind it that cannot start
    // until it finishes, which is what "on the critical path" means once the
    // graph is wider than one chain.
    std::vector<std::uint64_t> rank;

    std::span<const std::uint32_t> successors(std::uint32_t i) const noexcept {
        return {succ.data() + succ_off[i], succ.data() + succ_off[i + 1]};
    }
    std::span<const std::uint32_t> successors_by_rank(std::uint32_t i) const noexcept {
        return {succ_by_rank.data() + succ_off[i], succ_by_rank.data() + succ_off[i + 1]};
    }
    std::span<const std::uint32_t> predecessors(std::uint32_t i) const noexcept {
        return {pred.data() + pred_off[i], pred.data() + pred_off[i + 1]};
    }
    std::uint32_t in_degree(std::uint32_t i) const noexcept { return pred_off[i + 1] - pred_off[i]; }
    std::uint64_t total_work() const noexcept {
        std::uint64_t s = 0;
        for (auto w : work) s += w;
        return s;
    }
    std::uint64_t critical_path() const noexcept { return rank[source]; }
    std::size_t   edges() const noexcept { return succ.size(); }
};

// Builds a Shape from an edge list. Nodes must already be numbered in a
// topological order (every edge goes from a lower index to a higher one); the
// generators below all produce them that way, and the builder checks it.
class ShapeBuilder {
public:
    std::uint32_t add(std::uint64_t work_ticks) {
        work_.push_back(work_ticks);
        return static_cast<std::uint32_t>(work_.size() - 1);
    }
    void edge(std::uint32_t from, std::uint32_t to) { edges_.push_back({from, to}); }

    // Adds a sink after every node that has no successor, and returns the shape.
    // Node 0 must be the only node with no predecessor.
    Shape build(std::uint64_t sink_work = 0) {
        const auto n0 = static_cast<std::uint32_t>(work_.size());
        std::vector<bool> has_succ(n0, false);
        for (auto [f, t] : edges_) has_succ[f] = true;
        const std::uint32_t sink = add(sink_work);
        for (std::uint32_t i = 0; i < n0; ++i)
            if (!has_succ[i]) edges_.push_back({i, sink});

        Shape s;
        s.n      = static_cast<std::uint32_t>(work_.size());
        s.source = 0;
        s.sink   = sink;
        s.work   = work_;
        std::sort(edges_.begin(), edges_.end());
        edges_.erase(std::unique(edges_.begin(), edges_.end()), edges_.end());
        s.succ_off.assign(s.n + 1, 0);
        s.pred_off.assign(s.n + 1, 0);
        for (auto [f, t] : edges_) {
            if (f >= t) std::abort();  // not topologically numbered: a cycle is possible
            ++s.succ_off[f + 1];
            ++s.pred_off[t + 1];
        }
        for (std::uint32_t i = 0; i < s.n; ++i) {
            s.succ_off[i + 1] += s.succ_off[i];
            s.pred_off[i + 1] += s.pred_off[i];
        }
        s.succ.resize(edges_.size());
        s.pred.resize(edges_.size());
        std::vector<std::uint32_t> si(s.succ_off.begin(), s.succ_off.end() - 1);
        std::vector<std::uint32_t> pi(s.pred_off.begin(), s.pred_off.end() - 1);
        for (auto [f, t] : edges_) {
            s.succ[si[f]++] = t;
            s.pred[pi[t]++] = f;
        }
        for (std::uint32_t i = 1; i < s.n; ++i)
            if (s.in_degree(i) == 0) std::abort();  // a second root
        s.rank.assign(s.n, 0);
        for (std::uint32_t i = s.n; i-- > 0;) {
            std::uint64_t best = 0;
            for (auto j : s.successors(i)) best = std::max(best, s.rank[j]);
            // +1 so that zero-cost nodes still rank by depth.
            s.rank[i] = best + s.work[i] + 1;
        }
        s.succ_by_rank = s.succ;
        for (std::uint32_t i = 0; i < s.n; ++i)
            std::stable_sort(s.succ_by_rank.begin() + s.succ_off[i], s.succ_by_rank.begin() + s.succ_off[i + 1],
                             [&](std::uint32_t a, std::uint32_t b) { return s.rank[a] < s.rank[b]; });
        return s;
    }

private:
    std::vector<std::uint64_t>                            work_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges_;
};

// ---------------------------------------------------------------- instances

struct Instance;

// One node of one run. The deques carry pointers to these.
struct alignas(kCacheLine) NodeState {
    // Predecessors not yet resolved. The last one to decrement it owns the node.
    std::atomic<std::uint32_t> pending{0};
    // Set by a predecessor that failed or was skipped, before its decrement.
    // Atomic only because several predecessors may set it; it is read by the
    // owner after the last decrement, and is visible then for the same reason
    // the payload is — or is not, under the brief's ordering.
    std::atomic<std::uint8_t> poisoned{0};
    // The payload: deliberately a plain variable. Successors read it with no
    // atomic of their own, so the only thing that makes those reads safe is the
    // happens-before edge the dependency protocol builds. That is the property
    // this whole kata is about, and a race detector can only judge it if the
    // payload is not itself atomic.
    std::uint64_t out = 0;
    // Bookkeeping for the invariant checks: how many times the body ran, and how
    // many times the node was resolved (ran, failed, or was skipped).
    std::atomic<std::uint32_t> runs{0};
    std::atomic<std::uint32_t> resolved{0};
    // Which worker ran the body: plain, like the payload, and read only by the
    // thread that completes the instance, to count joins whose predecessors ran
    // on different cores — the only joins that test publication at all.
    std::uint16_t ran_on = 0;
    Instance*     inst   = nullptr;
    std::uint32_t              id   = 0;
};

inline std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 31;
    x *= 0x7fb5d329728ea185ULL;
    x ^= x >> 27;
    x *= 0x81dadef4bc2dd44dULL;
    x ^= x >> 33;
    return x;
}

struct Instance {
    const Shape*           shape = nullptr;
    std::vector<NodeState> nodes;
    // What each node's payload must be, computed at reset from the generation:
    // a successor compares what it read against this, so a read that raced with
    // the write, or happened before it, is counted rather than silently used.
    std::vector<std::uint64_t> expect;
    std::uint64_t              gen          = 0;
    std::uint64_t              due_tsc      = 0;      // when the event was due to arrive
    std::uint32_t              fail_node    = kNone;  // the node whose body fails in this run
    bool                       verify       = true;
    std::atomic<bool>          cancelled{false};      // fail-fast: set by the first failure
    std::atomic<std::uint64_t> failed_at_tsc{0};
    std::uint32_t              pool_slot = 0;

    explicit Instance(const Shape& s) : shape(&s), nodes(s.n), expect(s.n, 0) {
        for (std::uint32_t i = 0; i < s.n; ++i) {
            nodes[i].inst = this;
            nodes[i].id   = i;
        }
    }

    // Single-threaded: called only when nobody else can hold a pointer into this
    // instance — before its first submission, or by the thread that completed its
    // sink. The stores are plain; the injection queue's lock publishes them.
    void reset(std::uint64_t generation, std::uint32_t failing, bool check) {
        gen       = generation;
        fail_node = failing;
        verify    = check;
        cancelled.store(false, std::memory_order_relaxed);
        failed_at_tsc.store(0, std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < shape->n; ++i) {
            auto& nd = nodes[i];
            nd.pending.store(shape->in_degree(i), std::memory_order_relaxed);
            nd.poisoned.store(0, std::memory_order_relaxed);
            nd.out    = 0;
            nd.ran_on = 0;
            nd.runs.store(0, std::memory_order_relaxed);
            nd.resolved.store(0, std::memory_order_relaxed);
        }
        if (check) {
            for (std::uint32_t i = 0; i < shape->n; ++i) expect[i] = payload_of(i);
            // The failing node and everything downstream of it. Ids are a
            // topological order, so one forward pass propagates it.
            doomed.assign(shape->n, 0);
            if (failing != kNone) {
                doomed[failing] = 1;
                for (std::uint32_t i = failing; i < shape->n; ++i)
                    if (doomed[i])
                        for (auto j : shape->successors(i)) doomed[j] = 1;
            }
        }
    }

    // Under the poison policy a node's body runs exactly when nothing upstream of
    // it failed; the failing node itself runs (and fails). Under fail-fast
    // cancellation that is an upper bound.
    std::uint32_t expected_runs(std::uint32_t i) const noexcept { return doomed[i] && i != fail_node ? 0 : 1; }
    std::vector<std::uint8_t> doomed;

    // A node's payload: a hash of the generation, its id, and its predecessors'
    // payloads — so a successor that read a stale or torn input computes a wrong
    // value as well as noticing the input was wrong.
    std::uint64_t payload_of(std::uint32_t i) const noexcept {
        std::uint64_t h = mix(gen * 0x9e3779b97f4a7c15ULL + i);
        for (auto p : shape->predecessors(i)) h = mix(h ^ expect[p]);
        return h;
    }
};

// ---------------------------------------------------------------- generators

// A random layered DAG: `layers` layers of up to `width` nodes, each node with
// one to `fanin` predecessors in the layer above. Diamonds everywhere, uneven
// fan-in, so that the last predecessor to arrive is different every run.
inline Shape random_dag(std::uint32_t layers, std::uint32_t width, std::uint32_t fanin, std::uint64_t work_ticks,
                        std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    ShapeBuilder    b;
    const auto      src = b.add(work_ticks);
    std::vector<std::uint32_t> prev{src};
    for (std::uint32_t l = 0; l < layers; ++l) {
        const auto                 w = 1 + static_cast<std::uint32_t>(rng() % width);
        std::vector<std::uint32_t> cur;
        for (std::uint32_t k = 0; k < w; ++k) {
            const auto id = b.add(work_ticks);
            const auto np = 1 + static_cast<std::uint32_t>(rng() % fanin);
            for (std::uint32_t e = 0; e < np; ++e) b.edge(prev[rng() % prev.size()], id);
            cur.push_back(id);
        }
        prev = std::move(cur);
    }
    return b.build(work_ticks);
}

// N independent leaves under one source, joined by the sink — or, with
// `group` > 1, the same total work fused into N/group leaves. `tree` puts a
// layer of sqrt(N) partial joins between the leaves and the sink, so that no
// single counter takes N decrements.
inline Shape fan_out(std::uint32_t leaves, std::uint64_t leaf_ticks, std::uint32_t group = 1, bool tree = false) {
    ShapeBuilder b;
    const auto   src = b.add(0);
    const auto   n   = leaves / group;
    std::vector<std::uint32_t> ls;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto id = b.add(leaf_ticks * group);
        b.edge(src, id);
        ls.push_back(id);
    }
    if (tree) {
        std::uint32_t k = 1;
        while (k * k < n) ++k;
        for (std::uint32_t i = 0; i < n; i += k) {
            const auto j = b.add(0);
            for (std::uint32_t m = i; m < std::min(n, i + k); ++m) b.edge(ls[m], j);
        }
    }
    return b.build(0);
}

// A chain of `n` tasks: nothing to balance, so every nanosecond between the end
// of one and the start of the next is the scheduler's.
inline Shape chain(std::uint32_t n, std::uint64_t ticks) {
    ShapeBuilder b;
    auto         prev = b.add(ticks);
    for (std::uint32_t i = 1; i < n; ++i) {
        const auto id = b.add(ticks);
        b.edge(prev, id);
        prev = id;
    }
    return b.build(0);
}

// The event graph the latency phases use: a source, then two branches that
// run in parallel and meet at the sink —
//
//   a critical chain of `chain_len` steps of `chain_ticks` each, and
//   a fan-out of `leaves` short tasks of `leaf_ticks` each, in `group`-sized
//   fused tasks, behind one fan-out node.
//
// The chain is the critical path whenever chain_len * chain_ticks exceeds what
// the leaves cost spread over the workers; the leaves are most of the work.
// A scheduler that does not know which is which runs whichever it popped last.
inline Shape event_graph(std::uint32_t chain_len, std::uint64_t chain_ticks, std::uint32_t leaves,
                         std::uint64_t leaf_ticks, std::uint32_t group = 1) {
    ShapeBuilder b;
    const auto   src  = b.add(0);
    auto         prev = src;
    for (std::uint32_t i = 0; i < chain_len; ++i) {
        const auto id = b.add(chain_ticks);
        b.edge(prev, id);
        prev = id;
    }
    const auto fan = b.add(0);
    b.edge(src, fan);
    for (std::uint32_t i = 0; i < leaves / group; ++i) {
        const auto id = b.add(leaf_ticks * group);
        b.edge(fan, id);
    }
    return b.build(0);
}

// A pipeline that spawns side work at every step: `steps` chain steps of
// `step_ticks`, each making `leaves` side tasks of `leaf_ticks` ready beside the
// next step, and all of them joined at the sink. Every step is a decision
// between the critical path and work that can wait.
//
// The builder numbers each next step before the current step's leaves, so a
// scheduler that resolves edges in id order meets the next step first and the
// leaves after it. Nothing about that order is chosen to help or hurt; it is
// what a builder that creates "the next stage, then this stage's helpers"
// does, and a scheduler without ranks inherits whatever order it was given.
inline Shape pipeline_graph(std::uint32_t steps, std::uint64_t step_ticks, std::uint32_t leaves,
                            std::uint64_t leaf_ticks) {
    ShapeBuilder b;
    const auto   src  = b.add(0);
    auto         prev = b.add(step_ticks);
    b.edge(src, prev);
    for (std::uint32_t i = 1; i <= steps; ++i) {
        std::uint32_t next = kNone;
        if (i < steps) {
            next = b.add(step_ticks);
            b.edge(prev, next);
        }
        for (std::uint32_t k = 0; k < leaves; ++k) b.edge(prev, b.add(leaf_ticks));
        prev = next;
    }
    return b.build(0);
}

}  // namespace keystone
