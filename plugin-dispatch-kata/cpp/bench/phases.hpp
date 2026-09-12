#pragma once
//
// The seven phases, one per translation unit. Each prints its own results and
// registers failures via harness check().

#include "config.hpp"

namespace switchyard::bench {

// 0. The instrument: clock resolution, the empty-loop floor, and proof that the
//    100 handler bodies are actually distinct. Printed before any result that
//    depends on them.
void phase_instrument();

// 1. Invariants: every arm returns byte-identical results for every packet, so
//    a faster arm is faster at the same job. Also that the table preserves the
//    scan's first-match ordering, which is the one behaviour a table cannot
//    have for free.
void phase_invariants();

// 2. The scan, costed against handler count — the claim that the loop, not the
//    call, is what the profile was pointing at.
void phase_scan();

// 3. Traffic distribution: the number the brief withholds, and the one that
//    decides whether any of this was worth building.
void phase_distribution();

// 4. The brief's four options with selection held constant, each measured with
//    one resident handler and with 100 interleaved ones.
void phase_mechanism();

// 5. Code size per mechanism, read from what llvm-size measured at build time.
void phase_codesize();

// 6. The plugin boundary: crossing cost per packet against batched, version
//    negotiation, and an old plugin under a new host.
void phase_boundary();

}  // namespace switchyard::bench
