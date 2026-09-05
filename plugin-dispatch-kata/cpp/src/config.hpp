#pragma once
//
// Sizes the whole benchmark is built around. Every one of these is a number the
// solution quotes, so they live in one place rather than in the phase that
// happens to use them first.

#include <cstddef>
#include <cstdint>

namespace switchyard {

// The brief says "50-100 handlers". 100 is the top of that range, and phase 2
// sweeps down through it rather than assuming the count that flatters the
// argument.
inline constexpr int kHandlers = 100;

// Handler N claims protocol key N, so the keys in use are [0, kHandlers).
// Packets are also generated with keys outside that range: an unmatched packet
// costs a *full* scan, which is the worst case the brief's loop has and the one
// a benchmark of matched traffic alone would hide.
inline constexpr std::uint16_t kUnmatchedKeyBase = 4096;
inline constexpr double        kUnmatchedFraction = 0.02;

// The protocol field is 16 bits, so a directly-indexed table has this many
// slots whether or not they are used. 65536 * 8 B = 512 KiB of mostly-empty
// pointers, which is the density argument of step 2 made measurable: phase 3
// runs both this and a compact remapped table.
inline constexpr std::size_t kKeySpace16 = 1u << 16;

// Packets are generated once into this pool and replayed.
//
// 32768 x 32 B = 1 MiB, which is deliberately L2-resident. The first version of
// this used a 32 MiB pool on the theory that a pool small enough to cache hands
// every arm a free ride — and that was the wrong trade. 32 MiB sits exactly at
// this machine's 36 MiB L3 boundary, so every arm was measuring DRAM bandwidth
// with a dispatch mechanism attached, and the reported cost stopped being
// monotonic in the handler count. The subject here is dispatch; the memory
// system is somebody else's benchmark.
inline constexpr std::size_t kPacketPool = 1u << 15;

// Dispatches per timed pass, independent of the pool size: the pool is replayed
// until this many have happened. Keeps the sample count fixed while the working
// set stays small.
inline constexpr std::size_t kDispatchesPerRun = 1u << 18;

// Per-arm repetitions. Reported figures are the median per statistic, not the
// median run.
inline constexpr int kReps = 9;

// The rate the brief names. Everything is reported against the budget it
// implies: 1e9 / 3e6 = 333.3 ns per packet on one core.
inline constexpr double kTargetPacketsPerSec = 3.0e6;
inline constexpr double kBudgetNs = 1.0e9 / kTargetPacketsPerSec;

}  // namespace switchyard
