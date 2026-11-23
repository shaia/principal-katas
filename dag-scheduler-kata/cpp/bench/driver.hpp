#pragma once
//
// Runs one configuration of the scheduler against one graph shape, under one
// kind of load, and returns what happened.
//
// Two kinds of load:
//
//   closed — a fixed number of instances in flight, each resubmitted the moment
//            it completes. Measures capacity: how many instances per second the
//            scheduler can finish when it never waits for work.
//   open   — instances arrive on a schedule, at a fixed rate, whether or not the
//            previous ones have finished. Measures latency the way a user sees
//            it: from when the event was *due*, not from when the submitter got
//            round to it, so that a stall is charged to every event it delayed
//            (coordinated omission). An event that arrives when the pool is
//            exhausted is rejected and counted: bounded memory has to go
//            somewhere, and here it goes to admission.

#include "graph.hpp"
#include "harness.hpp"
#include "scheduler.hpp"

#include <cstdint>

namespace keystone::bench {

struct Load {
    enum Kind { closed, open } kind = closed;
    int           in_flight = 4;    // closed
    std::uint64_t instances = 0;    // closed: total completions
    double        rate      = 0;    // open: events per second
    double        seconds   = 1.0;  // open: how long arrivals continue
    int           pool      = 256;  // open: instances that may be in flight at once
};

struct Faults {
    double        probability = 0;      // per instance
    std::uint32_t node        = kNone;  // a fixed failing node, else uniform over the interior
};

struct Result {
    Stats         st;
    double        wall_s    = 0;
    double        cpu_s     = 0;  // process CPU time used during the run, all threads
    std::uint64_t submitted = 0;
    std::uint64_t rejected  = 0;
    int           max_searchers = 0;

    double per_sec() const { return wall_s > 0 ? static_cast<double>(st.completed) / wall_s : 0; }
    double cores_busy() const { return wall_s > 0 ? cpu_s / wall_s : 0; }
};

// The configuration and graph the correctness stresses share (phase 1 and
// --bug): chosen to maximize joins whose predecessors ran on different workers,
// because no other join tests publication at all. Every ready successor goes
// through a deque, idle workers steal without sleeping, and every body costs
// 300 ns so that a worker cannot finish a whole layer before a thief arrives.
// Few instances are in flight: a worker with an instance of its own to finish
// never steals, and a join whose predecessors all ran on one worker tests
// nothing.
Config stress_config();
Load   stress_load();
Shape  stress_shape(const Context& ctx, std::uint64_t seed);

Result run(const Context& ctx, const Shape& shape, const Config& cfg, const Load& load, bool verify,
           const Faults& faults = {}, std::uint64_t seed = 1);

}  // namespace keystone::bench
