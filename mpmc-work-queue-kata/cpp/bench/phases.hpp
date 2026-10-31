#pragma once
//
// The ten phases, one per translation unit, plus the deliberate-bug mode. Each
// prints its own results and registers failures via harness check().

#include "harness.hpp"

#include <string_view>

namespace turnstile::bench {

// Calibrates the clock and reads the topology, silently. Every phase needs it,
// including when phase 0 is not selected.
Context make_context();

// What one pause instruction costs on a fast core right now. A pause is a fixed
// number of cycles, so this is a thermometer: on a laptop under sustained load it
// rises as the clock drops, and main prints it before every phase.
double clock_indicator_ns(const Context& ctx);

// 0. The instrument: clocks and their skew across cores, the topology, what a
//    pause costs on each kind of core, what a cache line costs to move, and
//    what a wake costs — with nobody asleep, and with somebody.
void phase_instrument(const Context& ctx);

// 1. The brief's queue, broken on purpose: an overwrite with no consumer at all,
//    then loss, duplication and torn reads under real threads.
void phase_brief(const Context& ctx);

// 2. Invariants, for every arm: exactly-once, nothing torn, per-producer order
//    per consumer, and the accounting identity — including a 4-slot ring that
//    wraps every few operations.
void phase_invariants(const Context& ctx);

// 3. Does it meet the brief? Saturated throughput, and enqueue latency at the
//    brief's 5 M/s — with 48 threads on this machine's 32 CPUs, and without.
void phase_throughput(const Context& ctx);

// 4. Full: the capacity in milliseconds, a consumer deficit, reject against
//    block, and what p99 enqueue becomes when consumers cannot keep up.
void phase_backpressure(const Context& ctx);

// 5. Wakeups: what a push costs with consumers asleep, and a race engineered
//    to lose a wakeup — against the fenced protocol and with the fence removed.
void phase_wakeup(const Context& ctx);

// 6. Preemption inside the window: how long the window really is with 48
//    threads on 32 CPUs, and what one stalled producer or consumer does to
//    everyone else.
void phase_preemption(const Context& ctx);

// 7. Contention and fairness: retries per operation as producers are added,
//    tickets against CAS, and padded against packed slots.
void phase_contention(const Context& ctx);

// 8. Shutdown: close under load with blocked producers and a full queue, and a
//    close while a producer is stalled inside its window.
void phase_shutdown(const Context& ctx);

// 9. The reference outline's alternatives, beside the queue: its epoch wake, one
//    SPSC ring per producer, and consumers that spin — at the brief's rate,
//    asleep, idle, and where one ring per producer breaks and where it wins.
void phase_outline(const Context& ctx);

// --bug <name>: one deliberate bug from the C++ track's step C5, under the stress
// meant to expose it. Returns 1 if the stress detected it, 0 if it passed.
int run_bug(const Context& ctx, std::string_view name);

// Phase 5's engineered race against Bug::no_fence, for --bug no-fence. Returns
// the number of wakeups lost.
int lost_wakeups_without_fence(const Context& ctx);

}  // namespace turnstile::bench
