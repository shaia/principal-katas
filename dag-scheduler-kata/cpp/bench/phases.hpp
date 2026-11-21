#pragma once
//
// The phases, one per translation unit, plus the deliberate-bug mode. Each
// prints its own results and registers failures through harness check().

#include "harness.hpp"

#include <string_view>

namespace keystone::bench {

Context make_context();

// What one pause instruction costs on a fast core right now: a fixed cycle
// count, so a reading of the clock. main prints it before every phase.
double clock_indicator_ns(const Context& ctx);

// 0. The instrument: clocks, topology, a pause and a line transfer on each kind
//    of core, the decrement under every ordering, and what a wake costs.
void phase_instrument(const Context& ctx);

// 1. The brief's relaxed decrement: every ordering, correct and broken, under a
//    stress built to expose a missing publication — and what each costs.
void phase_brief(const Context& ctx);

// 2. Invariants across the configuration matrix: exactly-once, every input seen,
//    exactly-once completion, failure semantics, no hang.
void phase_invariants(const Context& ctx);

// 3. Task size: what one task costs the scheduler, by grain and by how ready
//    successors are handed on; fusion; and a wide join.
void phase_granularity(const Context& ctx);

// 4. The critical path: event latency with and without rank priority and inline
//    continuation, as load rises — and whether anything starves.
void phase_critical_path(const Context& ctx);

// 5. Failure propagation: poison against fail-fast cancellation — how long a
//    failed instance takes to finish, and how much work it wastes.
void phase_failure(const Context& ctx);

// 6. The follow-up: 8 to 64 workers, greedy thieves against a search budget —
//    throughput, tail latency, steal attempts, and the cores spent looking.
void phase_scaling(const Context& ctx);

// --bug <name>: one deliberate bug under the stress meant to expose it. Returns
// 1 if the stress detected it, 0 if not.
int run_bug(const Context& ctx, std::string_view name);

}  // namespace keystone::bench
