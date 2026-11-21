#include "harness.hpp"
#include "phases.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <timeapi.h>
#endif

namespace {

bool selected(const char* list, int phase) {
    if (!list) return true;
    const std::string s = std::string(",") + list + ",";
    return s.find("," + std::to_string(phase) + ",") != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace keystone;
    using namespace keystone::bench;

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char* phases = nullptr;
    const char* bug    = nullptr;
    for (int i = 1; i < argc; ++i) {
        const bool phase_opt = std::strcmp(argv[i], "--phase") == 0;
        const bool bug_opt   = std::strcmp(argv[i], "--bug") == 0;
        // An option without its value is an error, not a request for the default.
        if (!(phase_opt || bug_opt) || i + 1 == argc) {
            std::printf("usage: solution [--phase N[,M...]] [--bug none|relaxed-decrement|release-only|"
                        "acquire-only|release-fence|relaxed-publish|double-resolve]\n");
            return 2;
        }
        (phase_opt ? phases : bug) = argv[++i];
    }

#if defined(_WIN32)
    // The open-loop submitter sleeps while the next arrival is far off; at the
    // default 15.6 ms timer resolution it would oversleep by a tick.
    ::timeBeginPeriod(1);
#endif
    set_power_throttling(false);

    const Context ctx = make_context();

    if (bug) {
        const int detected = run_bug(ctx, bug);
        std::printf("\n%s\n", detected ? "DETECTED" : "not detected");
        return detected;
    }

    std::printf("keystone — a DAG executor, and the last predecessor that must have seen the others\n");
    std::printf("this machine: %d logical CPUs%s\n", logical_cpus(), ctx.hybrid ? ", hybrid" : "");

    struct Phase {
        int         n;
        const char* title;
        void (*fn)(const Context&);
    };
    const Phase all[] = {
        {0, "the instrument", phase_instrument},
        {1, "the brief's relaxed decrement", phase_brief},
        {2, "invariants", phase_invariants},
        {3, "task size, fusion, and the wide join", phase_granularity},
        {4, "the critical path", phase_critical_path},
        {5, "failure propagation", phase_failure},
        {6, "the follow-up: 8 to 64 workers", phase_scaling},
    };
    // The laptop throttles under sustained load, so each phase starts after a
    // pause to let the clock recover, and prints what a pause instruction costs
    // as it begins. Arms compared within a phase are interleaved; numbers from
    // different phases, or different runs, are not comparable.
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
