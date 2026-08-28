#pragma once
//
// Shared benchmark plumbing: assertions, percentiles, and the event factory.

#include "event.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace weir::bench {

// Records a failure and prints it. The process exit code is derived from
// failure_count(), so an invariant that breaks fails the build, not just the
// eye of whoever is reading the output.
void check(bool cond, const std::string& what);
int  failure_count();

std::int64_t now_ns();

Event make_event(std::uint32_t id, std::uint32_t seq, std::int64_t stamp);

struct Percentiles { double p50, p99, p999, p9999, max; };

// Sorts in place and returns microseconds. Samples are collected into a
// preallocated buffer and only sorted afterwards; nothing allocates or locks
// inside the measured path.
Percentiles percentiles(std::vector<std::int64_t>& v);

// Median across repetitions, computed per statistic rather than by picking one
// "median run". Choosing a whole run by its p99.9 lets one noisy percentile
// drag unrelated columns along with it.
template <typename T, typename F>
double median_of(const std::vector<T>& reps, F get) {
    std::vector<double> v;
    v.reserve(reps.size());
    for (const auto& r : reps) v.push_back(static_cast<double>(get(r)));
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace weir::bench
