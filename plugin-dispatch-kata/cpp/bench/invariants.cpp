// Phase 1 — invariants.
//
// Two separate jobs, and the second is the interesting one.
//
// First: every arm must return byte-identical results for every packet in the
// stream. Without that, a faster arm is not faster at the same job, and the
// whole comparison is worthless. The equality is expressed as a checksum over
// the result stream, which is also what keeps a dead-code eliminator from
// deleting the single-threaded dispatch loop — one mechanism, two jobs.
//
// Second: the brief's loop returns the FIRST match. That makes the order of the
// handler list a behaviour someone can depend on, and it is the one property a
// table cannot reproduce, because a table has no order at all. Where two
// predicates overlap, reordering the list by frequency is therefore not a
// semantics-preserving change — which matters, because reordering the list is
// exactly what phase 3 measures as the cheap competitor to the rewrite. This
// phase asserts the hazard exists rather than mentioning it in prose.

#include "generated.hpp"
#include "harness.hpp"
#include "packet.hpp"
#include "phases.hpp"
#include "strategy.hpp"

#include <cstdio>
#include <memory>
#include <vector>

namespace switchyard::bench {
namespace {

template <typename Arm>
std::uint64_t checksum_of(Arm& arm, const PacketStream& s) {
    Checksum sum;
    for (std::size_t i = 0; i < s.size(); ++i) sum.feed(arm.dispatch(s.data()[i]));
    return sum.value();
}

// Two handlers that match exactly the same packets. Nothing about the brief
// forbids this; it is what happens the moment two protocols are distinguished
// by something other than the key.
struct Overlapping final : Declaring {
    explicit Overlapping(std::uint16_t id) : id_(id) {}
    bool   matches(const Packet& p) const override { return p.protocol == 3000; }
    Result process(const Packet&) override { return Result{id_, id_}; }
    Registration declare() const override { return Registration{3000, true}; }
    std::uint16_t id_;
};

}  // namespace

void phase_invariants() {
    FatSet set;
    const auto hs = set.all();

    std::vector<std::uint16_t> keys;
    keys.reserve(kHandlers);
    for (int n = 0; n < kHandlers; ++n) keys.push_back(key_of(n));

    // Checked against the most skewed realistic stream, because that is the one
    // where the arms disagree most about which handler is reached first.
    PacketStream stream(Distribution::Zipf, keys, 0x5EED);

    ScanUnordered   ref(hs);
    const std::uint64_t want = checksum_of(ref, stream);

    ScanOrdered     ordered(hs, stream.frequencies());
    ScanMoveToFront mtf(hs);
    TableDirect16   direct(hs);
    TableCompact    compact(hs);

    check(checksum_of(ordered, stream) == want,
          "scan-ordered: agrees with the brief's loop on every packet");
    check(checksum_of(mtf, stream) == want,
          "scan-mtf: agrees with the brief's loop on every packet");
    check(checksum_of(direct, stream) == want,
          "table-direct16: agrees with the brief's loop on every packet");
    check(checksum_of(compact, stream) == want,
          "table-compact: agrees with the brief's loop on every packet");

    // The compact table's mapping is the low bits of the key. That happens to
    // be collision-free over the registered set; a real system needs chaining.
    // Asserted rather than assumed, because a collision would make this arm
    // return the wrong handler quickly, which a benchmark reports as a speedup.
    check(compact.injective(),
          "table-compact: the key mapping is collision-free over the registered keys");

    std::printf("  %zu packets, %d handlers, checksum %016llx\n", stream.size(), kHandlers,
                static_cast<unsigned long long>(want));

    // The ordering hazard, demonstrated.
    Overlapping a(1), b(2);
    Packet p{};
    p.protocol = 3000;

    ScanUnordered forward(std::vector<Declaring*>{&a, &b});
    ScanUnordered reverse(std::vector<Declaring*>{&b, &a});
    const Result rf = forward.dispatch(p);
    const Result rr = reverse.dispatch(p);

    check(rf.handler == 1 && rr.handler == 2,
          "first-match-wins: list order decides which of two overlapping handlers runs");
    std::printf("  overlapping predicates: registration order -> handler %u,"
                " reversed -> handler %u\n",
                rf.handler, rr.handler);
}

}  // namespace switchyard::bench
