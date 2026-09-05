// Phase 3 — the number the brief withholds.
//
// The brief gives a handler count and never gives a traffic distribution, and
// the second is the one that decides whether any of this was worth building. A
// linear scan over a list ordered by frequency costs about as many probes as
// the traffic is skewed; if 95 % of packets match within the first two
// handlers, the scan is already an O(1) dispatch wearing an O(n) loop.
//
// So this is the phase that can kill the rest of the answer, and it is written
// to be able to. `scan-ordered` is given every possible advantage: it is sorted
// by the *true* frequencies of the very stream it is then measured on, which is
// better than any real system could do, because a real system has to learn the
// distribution from traffic that has already gone past. `scan-mtf` is what
// learning it actually looks like.
//
// What is held fixed across every row: the handler set, the packet pool size,
// the residual predicates, the repetition count, and the compiler seeing all
// arms in one translation unit. What varies is the distribution and the arm.

#include "generated.hpp"
#include "harness.hpp"
#include "packet.hpp"
#include "phases.hpp"
#include "strategy.hpp"

#include <cstdio>
#include <vector>

namespace switchyard::bench {
namespace {

void row(const char* dist, const char* arm, const Measured& m, double probes) {
    std::printf("  %-8s %-15s | %9.1f %8.1f | %8.1f%% %9.1f\n", dist, arm, m.ns_per_packet,
                probes, 100.0 * m.ns_per_packet / kBudgetNs, 1000.0 / m.ns_per_packet);
}

}  // namespace

void phase_distribution() {
    FatSet set;
    const auto hs = set.all();

    std::vector<std::uint16_t> keys;
    keys.reserve(kHandlers);
    for (int n = 0; n < kHandlers; ++n) keys.push_back(key_of(n));

    std::printf("  %-8s %-15s | %9s %8s | %9s %9s\n", "traffic", "arm", "ns/pkt", "probes",
                "% budget", "M/s");

    for (Distribution d : {Distribution::Uniform, Distribution::Zipf, Distribution::Hot95,
                           Distribution::Worst}) {
        PacketStream stream(d, keys, 0xD15C0);
        const Packet* p = stream.data();
        const std::size_t n = stream.size();
        const char* dn = name_of(d);

        const auto m = measure_interleaved(p, n,
                                           [&] { return ScanUnordered(hs); },
                                           [&] { return ScanOrdered(hs, stream.frequencies()); },
                                           [&] { return ScanMoveToFront(hs); },
                                           [&] { return TableDirect16(hs); },
                                           [&] { return TableCompact(hs); });
        const Measured unord = m[0], ord = m[1], mtf = m[2], t16 = m[3], tc = m[4];

        check(ord.checksum == unord.checksum && mtf.checksum == unord.checksum &&
                  t16.checksum == unord.checksum && tc.checksum == unord.checksum,
              std::string(dn) + ": every arm agrees with the brief's loop");

        ScanUnordered   c1(hs);
        ScanOrdered     c2(hs, stream.frequencies());
        ScanMoveToFront c3(hs);
        TableDirect16   c4(hs);
        TableCompact    c5(hs);

        row(dn, "scan-unordered", unord, count_probes(c1, p, n));
        row(dn, "scan-ordered",   ord,   count_probes(c2, p, n));
        row(dn, "scan-mtf",       mtf,   count_probes(c3, p, n));
        row(dn, "table-direct16", t16,   count_probes(c4, p, n));
        row(dn, "table-compact",  tc,    count_probes(c5, p, n));
        std::printf("\n");
    }

    TableDirect16 big(hs);
    TableCompact  small(hs);
    std::printf("  table footprint: direct16 %zu KiB (65536 slots, %d used),"
                " compact %zu KiB\n",
                big.bytes() / 1024, kHandlers, small.bytes() / 1024);
    std::printf("  read the rows against each other, not against the budget: what decides\n"
                "  whether the table is worth building is the gap between scan-ordered and\n"
                "  table-compact, and that gap is a function of the traffic, not the code.\n");
}

}  // namespace switchyard::bench
