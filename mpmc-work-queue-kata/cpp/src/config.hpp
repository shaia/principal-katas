#pragma once
//
// The brief's numbers, in one place. Every one of these is quoted by the
// solution, so they live here rather than in the phase that happens to use them
// first.

#include <cstddef>
#include <cstdint>

namespace turnstile {

inline constexpr std::size_t kCapacity  = std::size_t{1} << 16;   // 65,536 slots
inline constexpr int         kProducers = 32;
inline constexpr int         kConsumers = 16;

inline constexpr double kTargetEventsPerSec = 5.0e6;
inline constexpr double kP99BudgetNs        = 2000.0;

// How long the queue lasts once the consumers stop: 65,536 slots at 5 M/s is
// 13.1 ms — about one scheduler timeslice, which is the number step 6 turns on.
inline constexpr double kCapacityMs = static_cast<double>(kCapacity) / kTargetEventsPerSec * 1e3;

// The time between consecutive operations on one shared counter at the target
// rate: 200 ns. Every contended read-modify-write is priced against this.
inline constexpr double kCounterBudgetNs = 1e9 / kTargetEventsPerSec;

// Consumer work per event in the paced runs, a stand-in for rule evaluation.
// Sixteen consumers at 5 M/s leave each 3.2 us per event; 1 us keeps them at
// about a third of capacity, so what gets measured is the queue rather than the
// consumers.
inline constexpr double kConsumerWorkNs = 1000.0;

// Repetitions per measured configuration. Figures are the median per statistic.
inline constexpr int kReps = 5;

}  // namespace turnstile
