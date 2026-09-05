// Phase 4 — the brief's four options, with selection held constant.
//
// Phase 3 removed the scan. What is left is one call per packet, and this is
// where the question the brief actually asked finally gets answered — against
// the post-table number rather than the one that made it look important.
//
// Every arm here performs the identical selection (one masked array index) and
// differs only in how it reaches process(). Selection is common-mode and
// cancels. The residual-predicate logic from phase 3 is deliberately absent for
// the same reason: it is identical across arms, and it would put a matches()
// call in front of the thing being measured.
//
// The three columns are the point:
//
//   hot        one protocol, so one handler, whose code stays resident. This is
//              what a dispatch microbenchmark measures, and it is the number
//              blog posts report.
//   mixed      the real distribution across 100 handlers whose combined code
//              does not fit in L1i. This is what a service does.
//   mixed-tiny the same, with handlers small enough that the callee is smaller
//              than the call sequence reaching it — so the column isolates the
//              mechanism from the footprint.
//
// hot vs mixed is the instruction-cache cost, measured as a time delta because
// this platform has no hardware counters to read it from. That substitution is
// discussed in solution.md section 12; it is arguably the better measurement,
// since it reports the effect in the unit the budget is denominated in.

#include "harness.hpp"
#include "mechanism.hpp"
#include "packet.hpp"
#include "phases.hpp"

#include <cstdio>
#include <vector>

namespace switchyard::bench {

void phase_mechanism() {
    Selector sel;

    std::vector<std::uint16_t> keys;
    keys.reserve(kHandlers);
    for (int n = 0; n < kHandlers; ++n) keys.push_back(key_of(n));

    // One protocol only: the handler stays in L1i and its branch target is
    // perfectly predicted.
    PacketStream hot(Distribution::Worst, std::vector<std::uint16_t>{key_of(0)}, 0x404);
    // All 100, skewed the way real protocol traffic is.
    PacketStream mixed(Distribution::Zipf, keys, 0x404);

    std::printf("  %-12s | %8s %8s %10s | %9s\n", "mechanism", "hot", "mixed", "mixed-tiny",
                "i-cache");
    std::printf("  %-12s | %8s %8s %10s | %9s\n", "", "ns/pkt", "ns/pkt", "ns/pkt", "cost ns");

    auto report = [&](const char* nm, const Measured& h, const Measured& m, const Measured& t) {
        std::printf("  %-12s | %8.2f %8.2f %10.2f | %9.2f\n", nm, h.ns_per_packet,
                    m.ns_per_packet, t.ns_per_packet, m.ns_per_packet - h.ns_per_packet);
    };

    // All twelve cells in one interleaved set. The i-cache column is a
    // difference between two of them, so they have to be measured under one set
    // of conditions or the subtraction reintroduces exactly the drift the
    // interleaving removes.
    const Packet* hp = hot.data();
    const Packet* mp = mixed.data();
    const std::size_t hn = hot.size(), mn = mixed.size();

    const auto r = measure_arms(
        ArmOver{hp, hn, [&] { return MechDirect<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechDirect<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechDirect<kTinyRounds>(sel); }},
        ArmOver{hp, hn, [&] { return MechVirtual<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechVirtual<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechVirtual<kTinyRounds>(sel); }},
        ArmOver{hp, hn, [&] { return MechErasure<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechErasure<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechErasure<kTinyRounds>(sel); }},
        ArmOver{hp, hn, [&] { return MechVariant<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechVariant<kFatRounds>(sel); }},
        ArmOver{mp, mn, [&] { return MechVariant<kTinyRounds>(sel); }});

    report(MechDirect<kFatRounds>::name(),  r[0], r[1], r[2]);
    report(MechVirtual<kFatRounds>::name(), r[3], r[4], r[5]);
    report(MechErasure<kFatRounds>::name(), r[6], r[7], r[8]);
    report(MechVariant<kFatRounds>::name(), r[9], r[10], r[11]);

    // A mechanism that is faster at a different job is not faster.
    check(r[7].checksum == r[4].checksum, "erasure: agrees with virtual on every packet");
    check(r[10].checksum == r[4].checksum, "variant: agrees with virtual on every packet");

    std::printf("\n  direct-call dispatches to nothing: it is the floor, not an option.\n"
                "  variant is measured for completeness only. A closed set of alternatives\n"
                "  cannot name a type that arrives from a shared library at runtime, and the\n"
                "  brief says handlers do — which disqualifies it whatever this table says.\n");
}

}  // namespace switchyard::bench
