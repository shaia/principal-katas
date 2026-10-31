// --bug <name> — the C++ track's step C5, one deliberate bug at a time.
//
// Each bug is one line of the correct protocol made wrong, selected by
// Options::bug, and each is run under the stress meant to expose it:
//
//   relaxed-seq-load, relaxed-seq-store, no-full-check, none:
//       eight producers and eight consumers on a 64-slot ring, 500k events each,
//       checked for exactly-once delivery, torn reads and per-producer order.
//       A 64-slot ring wraps every few dozen operations, so a slot changes hands
//       between laps thousands of times a second.
//   no-fence:
//       phase 5's engineered race, the only stress here that aims at the
//       store-buffering window.
//
// Exit status 1 means this stress detected the bug, 0 that it ran clean. Under
// ThreadSanitizer the sanitizer's own report is the other column of the table: it
// prints races as it finds them and exits 66 if it found any. A bug that makes
// the queue livelock is detected by the watchdog, which exits 1.
//
// `none` is the control. A stress that went red on the correct queue would make
// every other row meaningless.

#include "arms.hpp"
#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <memory>
#include <string_view>

namespace turnstile::bench {
namespace {

template <Bug B>
int stress(const Context& ctx) {
    // A clean run takes a few seconds, ten or so under ThreadSanitizer.
    Watchdog wd("the stress", std::chrono::seconds(30), /*exit_code=*/1);
    auto     q = std::make_unique<Queue<Event, 64, Options{.bug = B}>>();
    RunSpec  s;
    s.producers = 8;
    s.consumers = 8;
    s.events    = 500'000 / kScale;
    s.mode      = PushMode::block;
    const RunResult r = run(*q, s, ctx);
    std::printf("  8 x 8 on a 64-slot ring, %llu events each: accepted %llu, consumed %llu, torn %llu, out of order %llu\n",
                static_cast<unsigned long long>(s.events),
                static_cast<unsigned long long>(r.accepted), static_cast<unsigned long long>(r.consumed),
                static_cast<unsigned long long>(r.torn), static_cast<unsigned long long>(r.disorder));
    return r.exactly_once && r.torn == 0 && r.disorder == 0 ? 0 : 1;
}

}  // namespace

int run_bug(const Context& ctx, std::string_view name) {
    std::printf("bug: %.*s\n", static_cast<int>(name.size()), name.data());
    if (name == "none") return stress<Bug::none>(ctx);
    if (name == "relaxed-seq-load") return stress<Bug::relaxed_seq_load>(ctx);
    if (name == "relaxed-seq-store") return stress<Bug::relaxed_seq_store>(ctx);
    if (name == "no-full-check") return stress<Bug::no_full_check>(ctx);
    if (name == "no-fence") return lost_wakeups_without_fence(ctx) > 0 ? 1 : 0;
    std::printf("unknown bug; one of: none relaxed-seq-load relaxed-seq-store no-full-check no-fence\n");
    return 2;
}

}  // namespace turnstile::bench
