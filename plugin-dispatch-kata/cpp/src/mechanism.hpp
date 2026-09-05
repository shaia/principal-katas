#pragma once
//
// The brief's four options, isolated.
//
// Every arm here performs the SAME selection — one masked index into one small
// array — and differs only in how it reaches process() afterwards. Selection is
// therefore common-mode and cancels, which is the only way a comparison of
// dispatch *mechanisms* means anything.
//
// Deliberately absent: the residual-predicate logic from strategy.hpp. It is
// identical across all four arms, it is phase 3's subject, and including it
// here would put a matches() call in front of the thing being measured.

#include "generated.hpp"
#include "packet.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace switchyard {

// Selection shared by every mechanism arm: protocol -> dense handler index.
class Selector {
  public:
    static constexpr std::size_t   kBits = 10;
    static constexpr std::size_t   kSize = std::size_t{1} << kBits;
    static constexpr std::uint16_t kMask = static_cast<std::uint16_t>(kSize - 1);
    static constexpr std::uint16_t kNone = 0xFFFFu;

    Selector() {
        idx_.fill(kNone);
        key_.fill(0xFFFFu);
        // First handler claiming a key wins it, which is the scan's
        // first-match-wins rule applied at build time instead of per packet.
        for (int n = 0; n < kHandlers; ++n) {
            const std::uint16_t k = key_of(n);
            const std::size_t   b = k & kMask;
            if (idx_[b] == kNone) {
                idx_[b] = static_cast<std::uint16_t>(n);
                key_[b] = k;
            }
        }
    }

    std::uint16_t select(const Packet& p) const {
        const std::size_t b = p.protocol & kMask;
        return key_[b] == p.protocol ? idx_[b] : kNone;
    }

  private:
    std::array<std::uint16_t, kSize> idx_{};
    std::array<std::uint16_t, kSize> key_{};
};

// ------------------------------------------------------- 0. the zero point

// Not a dispatch mechanism: one handler, called directly, no indirection of any
// kind. It exists to price what dispatching *at all* costs, so the other three
// are reported against a floor rather than against each other alone.
template <int Rounds>
class MechDirect {
  public:
    explicit MechDirect(const Selector&) {}
    Result dispatch(const Packet& p) { return Result{mix<0, Rounds>(p), key_of(0)}; }
    static const char* name() { return "direct-call"; }
};

// --------------------------------------------------------- 1. keep virtual

template <int Rounds>
class MechVirtual {
  public:
    explicit MechVirtual(const Selector& s) : sel_(s) {
        for (auto* h : set_.all()) handlers_.push_back(h);
    }
    Result dispatch(const Packet& p) {
        const std::uint16_t i = sel_.select(p);
        if (i == Selector::kNone) return Result::unsupported();
        // Two dependent loads before the call: the object's vtable pointer,
        // then the slot in the vtable.
        return handlers_[i]->process(p);
    }
    static const char* name() { return "virtual"; }

  private:
    const Selector&          sel_;
    HandlerSet<Rounds>       set_;
    std::vector<Declaring*>  handlers_;
};

// ------------------------------------------------------ 4. custom erasure

// What a hand-rolled type-erased handler actually is: a pointer to the object
// and a pointer to the function, side by side. The saving over `virtual` is one
// dependent load — the vtable pointer is not fetched from the object, because
// the function pointer is already in the table next to it.
template <int Rounds>
class MechErasure {
  public:
    struct Erased {
        void* obj;
        Result (*fn)(void*, const Packet&);
    };

    explicit MechErasure(const Selector& s) : sel_(s) {
        build(std::make_integer_sequence<int, kHandlers>{});
    }

    Result dispatch(const Packet& p) {
        const std::uint16_t i = sel_.select(p);
        if (i == Selector::kNone) return Result::unsupported();
        const Erased& e = table_[i];
        return e.fn(e.obj, p);
    }
    static const char* name() { return "erasure"; }

  private:
    template <int... I>
    void build(std::integer_sequence<int, I...>) {
        (table_.push_back(Erased{nullptr, &thunk<I>}), ...);
    }
    template <int N>
    static Result thunk(void*, const Packet& p) {
        return Result{mix<N, Rounds>(p), key_of(N)};
    }

    const Selector&     sel_;
    std::vector<Erased> table_;
};

// --------------------------------------------------- 2. std::variant + visit

// A closed set of alternatives, fixed at compile time. That property is the
// whole verdict on this option and it has nothing to do with the number below:
// a variant cannot name a type that arrives from a shared library at runtime,
// and the brief says handlers do. It is measured anyway, because "it is also
// slower" and "it cannot express the requirement" are different claims and only
// one of them survives a faster machine.
template <int Rounds, int... I>
auto variant_type(std::integer_sequence<int, I...>) -> std::variant<GeneratedT<I, Rounds>...>;

template <int Rounds>
using HVariant = decltype(variant_type<Rounds>(std::make_integer_sequence<int, kHandlers>{}));

template <int Rounds>
class MechVariant {
  public:
    explicit MechVariant(const Selector& s) : sel_(s) {
        build(std::make_integer_sequence<int, kHandlers>{});
    }

    Result dispatch(const Packet& p) {
        const std::uint16_t i = sel_.select(p);
        if (i == Selector::kNone) return Result::unsupported();
        return std::visit([&p](auto& h) { return h.process(p); }, objs_[i]);
    }
    static const char* name() { return "variant"; }

  private:
    template <int... I>
    void build(std::integer_sequence<int, I...>) {
        (objs_.push_back(HVariant<Rounds>{std::in_place_type<GeneratedT<I, Rounds>>}), ...);
    }

    const Selector&                sel_;
    std::vector<HVariant<Rounds>>  objs_;
};

}  // namespace switchyard
