#pragma once
//
// 100 handlers with a controlled, *distinct* code footprint.
//
// Two properties matter and neither is incidental:
//
//   1. Each instantiation must be big enough that 100 of them do not fit in
//      L1i. Phase 4's whole argument is that a dispatch-mechanism benchmark
//      with one resident handler ranks the mechanisms differently from a
//      service with 100 of them, and that argument is unmeasurable if the
//      handler set fits in cache.
//
//   2. No two instantiations may have identical machine code, or identical-code
//      folding will merge them and property 1 silently evaporates. Every round
//      below mixes in a constant derived from N, so the bodies differ. Phase 0
//      prints the measured .text of the set rather than trusting this comment.

#include "handler.hpp"
#include "platform.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace switchyard {

// ~1 KiB of code each at 48 rounds; 100 of them is comfortably past a 32 KiB
// L1i. The tiny variant exists so phase 4 can separate the cost of *making the
// call* from the cost of the callee's footprint — with 2 rounds the callee is
// smaller than the call sequence that reaches it.
inline constexpr int kFatRounds  = 48;
inline constexpr int kTinyRounds = 2;

// Which handlers have a predicate that is not a pure function of the key. Step 2
// asks for the property every matches() must have for a table to be legal;
// these are the ones that do not have it.
inline constexpr bool residual_of(int n) { return (n % 10) == 7; }

// Handlers with a residual predicate deliberately share a small set of keys, so
// a table slot for one of those keys holds a genuine short candidate list
// rather than a single-element special case. That list is where the complexity
// the table removed goes to live, and step 10 makes an argument about it.
inline constexpr std::uint16_t key_of(int n) {
    return residual_of(n) ? static_cast<std::uint16_t>(900 + (n % 3))
                          : static_cast<std::uint16_t>(n);
}

// The per-handler work. SWY_NOINLINE because the subject under measurement is
// what a call costs and how much room the callees take; a compiler that inlines
// the callee into the dispatch loop has answered a different question, and
// would answer it differently per mechanism.
template <int N, int Rounds>
SWY_NOINLINE std::uint64_t mix(const Packet& p) {
    std::uint64_t h = 0xcbf29ce484222325ULL ^
                      (static_cast<std::uint64_t>(N) * 0x9E3779B97F4A7C15ULL);
    [&]<int... I>(std::integer_sequence<int, I...>) {
        ((h = (h ^ (static_cast<std::uint64_t>(p.payload[(I * 7 + N) % 24]) +
                    static_cast<std::uint64_t>(N + I))) *
              (0x100000001b3ULL + 2u * static_cast<std::uint64_t>(I))),
         ...);
    }(std::make_integer_sequence<int, Rounds>{});
    return h;
}

template <int N, int Rounds>
struct GeneratedT final : Declaring {
    static constexpr std::uint16_t kKey      = key_of(N);
    static constexpr bool          kResidual = residual_of(N);

    bool matches(const Packet& p) const override {
        if (p.protocol != kKey) return false;
        // The clause that makes this handler unfactorable: a payload byte, not
        // the key. No table can select on it.
        //
        // The residual handlers sharing a key are given DISJOINT ranges, so at
        // most one handler matches any packet and every arm can be compared
        // against every other by checksum. Overlapping predicates are a real
        // hazard and a real finding — reordering the list would then change the
        // answer — but they are asserted separately in phase 1 rather than left
        // to contaminate the performance arms.
        if constexpr (kResidual) return (p.payload[0] & 3) == ((N / 10) & 3);
        else return true;
    }

    Result process(const Packet& p) override {
        return Result{mix<N, Rounds>(p), kKey};
    }

    Registration declare() const override { return Registration{kKey, kResidual}; }
};

template <int N> using Generated     = GeneratedT<N, kFatRounds>;
template <int N> using GeneratedTiny = GeneratedT<N, kTinyRounds>;

// All kHandlers instances, in registration order. Registration order is a
// semantic: the brief's loop returns the *first* match, so this vector's order
// is a behaviour every other arm has to reproduce.
template <int Rounds>
class HandlerSet {
  public:
    HandlerSet() {
        owned_.reserve(kHandlers);
        build(std::make_integer_sequence<int, kHandlers>{});
        all_.reserve(owned_.size());
        for (auto& p : owned_) all_.push_back(p.get());
    }

    // A prefix of the set. Phase 2 sweeps the handler count rather than
    // assuming the one that flatters the argument.
    std::vector<Declaring*> first(int n) const {
        return std::vector<Declaring*>(all_.begin(), all_.begin() + n);
    }
    const std::vector<Declaring*>& all() const { return all_; }

  private:
    template <int... I>
    void build(std::integer_sequence<int, I...>) {
        (owned_.emplace_back(std::make_unique<GeneratedT<I, Rounds>>()), ...);
    }

    std::vector<std::unique_ptr<Declaring>> owned_;
    std::vector<Declaring*>                 all_;
};

using FatSet  = HandlerSet<kFatRounds>;
using TinySet = HandlerSet<kTinyRounds>;

}  // namespace switchyard
