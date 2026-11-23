// --bug <name>: one row of the C++ track's bug x tool table.
//
// Each bug runs under the stress phase 1 uses (driver.hpp, stress_config):
// random DAGs with fan-in up to 8, every ready successor pushed, 16 workers
// stealing without sleeping, so that most joins have predecessors on different
// cores.
// "Detected" means the stress itself saw it — a payload read before it was
// written, a body run twice, an instance completed with a node unresolved, or a
// hang. Under ThreadSanitizer a race report is the other way to go red; TSan
// exits 66 when it reported one.

#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace keystone::bench {

int run_bug(const Context& ctx, std::string_view bug) {
    Config      cfg = stress_config();
    Faults      faults;
    const char* what = nullptr;
    if (bug == "none") {
        what = "the correct protocol: acq_rel decrements, release/acquire deque";
    } else if (bug == "relaxed-decrement") {
        cfg.order = DecOrder::relaxed;
        what      = "the brief: every decrement relaxed";
    } else if (bug == "release-only") {
        cfg.order = DecOrder::release_only;
        what      = "every decrement release, and the last one does not acquire";
    } else if (bug == "acquire-only") {
        cfg.order = DecOrder::acquire_only;
        what      = "every decrement acquire, and nobody releases";
    } else if (bug == "release-fence") {
        cfg.order = DecOrder::release_fence;
        what      = "NOT a bug: release decrements, an acquire fence in the last one (the shared_ptr idiom)";
    } else if (bug == "relaxed-publish") {
        cfg.relaxed_publish = true;
        what                = "the deque's push publishes nothing: slot and bottom stored relaxed";
    } else if (bug == "double-resolve") {
        cfg.double_resolve = true;
        faults.probability = 0.5;
        what               = "a failed node resolves its out-edges twice";
    } else {
        std::printf("unknown bug '%.*s'\n", static_cast<int>(bug.size()), bug.data());
        return 2;
    }
    std::printf("bug: %.*s — %s\n", static_cast<int>(bug.size()), bug.data(), what);
    // A hang is a detection. Under ThreadSanitizer a run that is printing race
    // reports with 16 threads spinning can take minutes and still finish, so
    // the limit there is long enough to tell slow from stuck.
    set_hang_policy(1, kScale > 1 ? 300 : 15);

    std::uint64_t violations = 0, bad = 0, instances = 0, steals = 0, cross = 0;
    for (int round = 0; round < 6; ++round) {
        const Shape s = stress_shape(ctx, 100 + static_cast<std::uint64_t>(round));
        const Load  load = stress_load();
        const Result r = run(ctx, s, cfg, load, true, faults, static_cast<std::uint64_t>(round) + 7);
        violations += r.st.violations;
        bad += r.st.bad_completions;
        instances += r.st.completed;
        steals += r.st.steals;
        cross += r.st.cross_joins;
    }
    std::printf("  %llu instances, %llu stolen tasks, %llu cross-worker joins: %llu inputs read unwritten or "
                "bodies run twice, %llu bad completions\n",
                static_cast<unsigned long long>(instances), static_cast<unsigned long long>(steals),
                static_cast<unsigned long long>(cross),
                static_cast<unsigned long long>(violations), static_cast<unsigned long long>(bad));
    return violations + bad > 0 ? 1 : 0;
}

}  // namespace keystone::bench
