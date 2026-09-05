// Phase 0 — the instrument.
//
// Nothing here is a result. It is the set of facts every later number depends
// on, established first so that a later number cannot quietly be an artifact of
// one of them.
//
// Three things get established:
//
//   1. The clock's smallest observable tick. Everything here is timed per
//      million packets and divided, so tick granularity is not the hazard it
//      was in the mpsc kata's Go track — but a clock that advanced a thousand
//      times a second would still invalidate the shorter runs, and asserting
//      that is cheaper than assuming it.
//
//   2. The empty-loop floor: what a pass costs when the dispatch does nothing.
//      Every reported figure includes this, and a floor comparable to the
//      differences being reported would mean the benchmark is measuring its own
//      loop. This matters far more than tick granularity.
//
//   3. That the 100 handler bodies are actually distinct. Phase 4's entire
//      instruction-cache argument is unmeasurable if identical-code folding
//      merged them into one function, and that failure is invisible: the
//      program is correct, the numbers are plausible, and the effect being
//      measured is simply not there. Comparing the function addresses detects
//      it directly.

#include "generated.hpp"
#include "harness.hpp"
#include "packet.hpp"
#include "phases.hpp"
#include "strategy.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace switchyard::bench {
namespace {

// Not a dispatch mechanism: the cost of the pass itself.
struct NullArm {
    Result dispatch(const Packet& p) const { return Result{p.flags, p.protocol}; }
};

// Smallest observable tick: call the clock in a tight loop and count how many
// *distinct* values come back, not how fast it returns. A clock can be cheap to
// read and still advance only a thousand times a second.
double clock_tick_ns() {
    std::vector<std::int64_t> v;
    v.reserve(1 << 16);
    const auto deadline = Clock::now() + std::chrono::milliseconds(5);
    while (Clock::now() < deadline && v.size() < v.capacity())
        v.push_back(Clock::now().time_since_epoch().count());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() < 2) return 0.0;
    std::int64_t smallest = v[1] - v[0];
    for (std::size_t i = 2; i < v.size(); ++i)
        smallest = std::min(smallest, v[i] - v[i - 1]);
    return static_cast<double>(smallest) * 1e9 *
           static_cast<double>(Clock::period::num) / static_cast<double>(Clock::period::den);
}

// Distinctness, tested by behaviour rather than by address.
//
// The first version of this compared &mix<N,48> for all N and asserted 100
// distinct pointers. That measurement is worthless on this target: a PE binary
// hands back the address of a 64-byte jump thunk, so the pointers differ by 64
// whether or not the bodies behind them were folded together — it reported a
// 6 KiB span for a set that llvm-size puts at over 100 KiB.
//
// Calling them and comparing results has no such failure mode. Identical-code
// folding merges functions that compute the same thing; 100 distinct outputs on
// one input means 100 functions survived.
template <int... I>
std::vector<std::uint64_t> body_results(const Packet& p, std::integer_sequence<int, I...>) {
    return {mix<I, kFatRounds>(p)...};
}

}  // namespace

void phase_instrument() {
    // Before anything is timed. On a hybrid CPU an unpinned thread migrates
    // between performance and efficiency cores mid-run, and the resulting
    // numbers are not comparable to each other, let alone across arms.
    pin_to_one_core();

    std::printf("  clock          %-24s tick %.1f ns\n", "steady_clock", clock_tick_ns());

    std::vector<std::uint16_t> keys;
    for (int n = 0; n < kHandlers; ++n) keys.push_back(key_of(n));
    PacketStream stream(Distribution::Zipf, keys, 0xC0FFEE);

    // Process-level warm-up, before any measurement at all.
    //
    // measure() already discards one pass per arm, and that is not enough: the
    // FIRST run of a freshly linked binary came out roughly three times slower
    // than every subsequent one, uniformly across every arm, which made two
    // arms doing identical work differ by 1.75x. The per-arm warm-up cannot fix
    // it because the cost is per *process* — faulting in the code pages, and a
    // core that has not yet been asked to go fast.
    //
    // Half a second of the heaviest arm, discarded. Everything after this is
    // measured on a warm core with the working set resident.
    {
        FatSet       set;
        const auto   hs = set.all();
        const auto   until = Clock::now() + std::chrono::milliseconds(500);
        Checksum     sink;
        ScanUnordered burn(hs);
        while (Clock::now() < until)
            for (std::size_t i = 0; i < stream.size(); ++i) sink.feed(burn.dispatch(stream.data()[i]));
        if (sink.value() == 1) std::printf(" ");
        std::printf("  warm-up        %-24s 500 ms, discarded\n", "scan over 100 handlers");
    }

    const Measured floor = measure([] { return NullArm{}; }, stream.data(), stream.size());
    std::printf("  loop floor     %-24s %.2f ns/packet\n", "null dispatch", floor.ns_per_packet);

    // Distinctness of the handler bodies.
    auto vals = body_results(stream.data()[0], std::make_integer_sequence<int, kHandlers>{});
    std::sort(vals.begin(), vals.end());
    const auto distinct =
        static_cast<std::size_t>(std::unique(vals.begin(), vals.end()) - vals.begin());

    std::printf("  handler bodies %-24s %zu of %d distinct on one packet\n", "mix<N,48>",
                distinct, kHandlers);
    std::printf("  code footprint %-24s see phase 5; measured by the linker, not from here\n",
                "mix<N,48> x 100");

    check(distinct == static_cast<std::size_t>(kHandlers),
          "phase 0: handler bodies are distinct (identical-code folding did not merge them)");
    check(floor.ns_per_packet < 5.0,
          "phase 0: the empty-loop floor is small against the figures that follow");
}

}  // namespace switchyard::bench
