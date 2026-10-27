#include "config.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <timeapi.h>
#endif

namespace {

// --phase 2,8 selects phases; the default is all of them.
bool selected(const char* list, int phase) {
    if (!list) return true;
    const std::string s = std::string(",") + list + ",";
    return s.find("," + std::to_string(phase) + ",") != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace turnstile;
    using namespace turnstile::bench;

    // Unbuffered even when redirected, so a run that is slow or hangs shows how far
    // it got. (Not line-buffered: the Windows CRT treats _IOLBF as full buffering.)
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char* phases = nullptr;
    const char* bug    = nullptr;
    for (int i = 1; i < argc; ++i) {
        const bool phase_opt = std::strcmp(argv[i], "--phase") == 0;
        const bool bug_opt   = std::strcmp(argv[i], "--bug") == 0;
        // An option without its value is an error, not a request for the default:
        // a dropped argument once ran every phase under ThreadSanitizer for hours.
        if (!(phase_opt || bug_opt) || i + 1 == argc) {
            std::printf("usage: solution [--phase N[,M...]] [--bug none|relaxed-seq-load|relaxed-seq-store|"
                        "no-full-check|no-fence]\n");
            return 2;
        }
        (phase_opt ? phases : bug) = argv[++i];
    }

#if defined(_WIN32)
    // The paced producers sleep between bursts. At the default 15.6 ms timer
    // resolution a 200 us sleep would last a whole tick and every burst would be
    // 15 ms of events; at 1 ms they wake about once a millisecond.
    ::timeBeginPeriod(1);
#endif
    set_power_throttling(false);

    const Context ctx = make_context();

    if (bug) {
        const int detected = run_bug(ctx, bug);
        std::printf("\n%s\n", detected ? "DETECTED" : "not detected");
        return detected;
    }

    std::printf("turnstile — a bounded MPMC work queue, and the slot that is reserved but not yet there\n");
    std::printf("%d producers, %d consumers, %zu slots, target %.1f M events/s, p99 enqueue budget %.0f ns\n"
                "capacity at the target rate: %.1f ms.  this machine: %d logical CPUs%s\n",
                kProducers, kConsumers, kCapacity, kTargetEventsPerSec / 1e6, kP99BudgetNs, kCapacityMs,
                logical_cpus(), ctx.hybrid ? ", hybrid" : "");

    struct Phase {
        int         n;
        const char* title;
        void (*fn)(const Context&);
    };
    const Phase all[] = {
        {0, "the instrument", phase_instrument},
        {1, "the brief's queue, broken", phase_brief},
        {2, "invariants", phase_invariants},
        {3, "does it meet the brief?", phase_throughput},
        {4, "full: backpressure", phase_backpressure},
        {5, "wakeups", phase_wakeup},
        {6, "preemption inside the window", phase_preemption},
        {7, "contention and fairness", phase_contention},
        {8, "shutdown", phase_shutdown},
        {9, "the reference outline's alternatives", phase_outline},
    };
    // A laptop under sustained load throttles, and this one runs a vendor power
    // profile that caps sustained power: across one full run the core clock fell by
    // about a third. So each phase starts after a pause to let the clock recover,
    // and reports what a pause instruction costs as it begins — a fixed cycle
    // count, so a direct reading of the clock. Arms compared within a phase run
    // interleaved, so drift during a phase is common to all of them; numbers from
    // different phases, or different runs, are not comparable at all.
    bool first = true;
    for (const Phase& p : all) {
        if (!selected(phases, p.n)) continue;
        if (!first) std::this_thread::sleep_for(std::chrono::seconds(5));
        first = false;
        std::printf("\n== %d. %s ==   [pause %.0f ns on CPU %d]\n", p.n, p.title, clock_indicator_ns(ctx),
                    ctx.fast_cpu);
        p.fn(ctx);
    }

#if defined(_WIN32)
    ::timeEndPeriod(1);
#endif

    const int failures = failure_count();
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
