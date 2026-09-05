// Phase 2 — the scan, costed.
//
// The profile in the brief says `dispatch` is expensive. Every one of the four
// proposed alternatives changes how `process` is *called*. This phase asks
// whether the call is where the time is, by sweeping the one number the brief
// does give — the handler count — and watching what the loop does with it.
//
// The distribution is uniform here, which is the assumption the brief's framing
// implicitly makes and the one phase 3 goes on to attack. Taking it at face
// value first is deliberate: it is the case the option list was chosen for.

#include "generated.hpp"
#include "harness.hpp"
#include "packet.hpp"
#include "phases.hpp"
#include "strategy.hpp"

#include <cstdio>
#include <vector>

namespace switchyard::bench {

void phase_scan() {
    FatSet set;

    std::printf("  %-9s | %9s %9s | %8s %9s | %8s\n", "handlers", "scan", "table", "probes",
                "scan", "table");
    std::printf("  %-9s | %9s %9s | %8s %9s | %8s\n", "", "ns/pkt", "ns/pkt", "/pkt",
                "% budget", "speedup");

    for (int n : {10, 25, 50, 100}) {
        std::vector<std::uint16_t> keys;
        keys.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) keys.push_back(key_of(i));

        PacketStream stream(Distribution::Uniform, keys, 0xABCD);
        const auto   hs = set.first(n);

        const auto     m     = measure_interleaved(stream.data(), stream.size(),
                                                   [&] { return ScanUnordered(hs); },
                                                   [&] { return TableCompact(hs); });
        const Measured scan  = m[0];
        const Measured table = m[1];

        ScanUnordered counter(hs);
        const double  probes = count_probes(counter, stream.data(), stream.size());

        check(scan.checksum == table.checksum,
              "phase 2: scan and table agree at every handler count");

        std::printf("  %-9d | %9.1f %9.1f | %8.1f %8.1f%% | %7.1fx\n", n, scan.ns_per_packet,
                    table.ns_per_packet, probes, 100.0 * scan.ns_per_packet / kBudgetNs,
                    scan.ns_per_packet / table.ns_per_packet);
    }

    std::printf("\n  budget is %.1f ns/packet at %.1f M packets/s on one core.\n", kBudgetNs,
                kTargetPacketsPerSec / 1e6);
    std::printf("  probes/pkt is how many matches() calls ran before the handler was found;\n"
                "  it is counted in a separate untimed pass, never on the measured path.\n");
}

}  // namespace switchyard::bench
