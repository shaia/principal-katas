// One dispatch mechanism, alone in a binary.
//
// Code size cannot be measured from inside a binary that contains all four
// mechanisms, so each is linked into its own executable over the same 100
// handlers and llvm-size reads the sections at build time. `none` contains the
// handler set and no dispatch machinery at all, so the differences between the
// others and it are the machinery rather than the handlers.
//
// The dispatch result is printed so the linker cannot discard what it reaches.

#include "mechanism.hpp"

#include <cstdio>

int main() {
    using namespace switchyard;

    Packet p{};
    p.protocol = key_of(3);
    for (int i = 0; i < 24; ++i) p.payload[i] = static_cast<std::uint8_t>(i * 7 + 1);

#if defined(SWY_SIZE_MECH_EMPTY)
    // No handlers and no dispatch: the cost of being a C++ program at all, so
    // the handler set's own footprint can be separated from the runtime's.
    const Result r = Result{p.protocol, p.protocol};
    const char* which = "empty";
#elif defined(SWY_SIZE_MECH_NONE)
    HandlerSet<kFatRounds> set;
    const Result r = set.all()[3]->process(p);
    const char* which = "none";
#elif defined(SWY_SIZE_MECH_VIRTUAL)
    Selector sel;
    MechVirtual<kFatRounds> m(sel);
    const Result r = m.dispatch(p);
    const char* which = MechVirtual<kFatRounds>::name();
#elif defined(SWY_SIZE_MECH_ERASURE)
    Selector sel;
    MechErasure<kFatRounds> m(sel);
    const Result r = m.dispatch(p);
    const char* which = MechErasure<kFatRounds>::name();
#elif defined(SWY_SIZE_MECH_VARIANT)
    Selector sel;
    MechVariant<kFatRounds> m(sel);
    const Result r = m.dispatch(p);
    const char* which = MechVariant<kFatRounds>::name();
#else
#error "size_probe.cpp needs one of SWY_SIZE_MECH_{NONE,VIRTUAL,ERASURE,VARIANT}"
#endif

    std::printf("%s %llu %u\n", which, static_cast<unsigned long long>(r.value), r.handler);
    return 0;
}
