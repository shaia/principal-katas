#include "config.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <thread>

int main() {
    using namespace switchyard;
    using namespace switchyard::bench;

    std::printf("switchyard — dispatch selection and the plugin ABI boundary\n");
    std::printf("%d handlers, %zu packets/stream, %d reps, budget %.1f ns/packet"
                " at %.1f M packets/s\n\n",
                kHandlers, kPacketPool, kReps, kBudgetNs, kTargetPacketsPerSec / 1e6);

    std::printf("== 0. the instrument ==\n");
    phase_instrument();

    std::printf("\n== 1. invariants ==\n");
    phase_invariants();

    std::printf("\n== 2. the scan, costed ==\n");
    phase_scan();

    std::printf("\n== 3. traffic distribution ==\n");
    phase_distribution();

    std::printf("\n== 4. the four mechanisms ==\n");
    phase_mechanism();

    std::printf("\n== 5. code size ==\n");
    phase_codesize();

    std::printf("\n== 6. the plugin boundary ==\n");
    phase_boundary();

    const int failures = failure_count();
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
