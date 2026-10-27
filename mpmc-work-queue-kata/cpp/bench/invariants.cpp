// Phase 2 — invariants, for every arm.
//
// Each run asserts, under the shared labels in driver.hpp: every offered event
// is accounted for as accepted, rejected or refused; every accepted event is
// consumed exactly once; nothing is read torn or unpublished; and from each
// consumer's point of view each producer's events arrive in order.
//
// Then the same against a 4-slot ring with eight producers and eight consumers,
// which wraps every few operations. At 65,536 slots a lap takes thousands of
// operations and a bug in the lap encoding has to wait for its moment; at four,
// every slot is reused by a different lap roughly every microsecond, in reject
// mode and in block mode.

#include "arms.hpp"
#include "config.hpp"
#include "driver.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace turnstile::bench {
namespace {

template <typename Q, typename... Args>
void one(const Context& ctx, const char* label, const RunSpec& spec, Args&&... args) {
    auto            q  = std::make_unique<Q>(std::forward<Args>(args)...);
    const auto      t0 = Clock::now();
    const RunResult r  = run(*q, spec, ctx);
    const double    s  = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("  %-30s %3d x %-3d  accepted %9llu  rejected %8llu  consumed %9llu  %5.1f s  %s\n", label,
                spec.producers, spec.consumers, static_cast<unsigned long long>(r.accepted),
                static_cast<unsigned long long>(r.rejected), static_cast<unsigned long long>(r.consumed), s,
                r.exactly_once && r.disorder == 0 ? "ok" : "BROKEN");
    check_run(r, label);
}

}  // namespace

void phase_invariants(const Context& ctx) {
    RunSpec full;  // the brief's thread counts, saturated, blocking
    full.events = 200'000 / kScale;
    one<Turnstile>(ctx, "turnstile (CAS, padded)", full);
    one<Packed>(ctx, "turnstile (CAS, packed slots)", full);
    one<Ticket>(ctx, "ticket (fetch_add)", full);
    one<Locked>(ctx, "mutex + 2 condvars", full);

    RunSpec reject = full;
    reject.mode    = PushMode::reject;
    one<Turnstile>(ctx, "turnstile, reject on full", reject);
    RunSpec mixed = full;
    mixed.mode    = PushMode::mixed;
    one<Ticket>(ctx, "ticket + CAS on one word", mixed);

    // The lap wrap, as hard as it can be driven.
    RunSpec wrap;
    wrap.producers = 8;
    wrap.consumers = 8;
    wrap.events    = 250'000 / kScale;
    wrap.mode      = PushMode::reject;
    one<Queue<Event, 4>>(ctx, "4-slot ring, reject", wrap);
    wrap.mode = PushMode::block;
    one<Queue<Event, 4>>(ctx, "4-slot ring, block", wrap);
    one<Queue<Event, 4, Options{.reserve = Reserve::ticket}>>(ctx, "4-slot ring, ticket", wrap);
    wrap.mode = PushMode::mixed;
    one<Queue<Event, 4, Options{.reserve = Reserve::ticket}>>(ctx, "4-slot ring, ticket + CAS", wrap);
    wrap.mode = PushMode::block;
    one<Queue<Event, 4, Options{.padded = false}>>(ctx, "4-slot ring, packed", wrap);
}

}  // namespace turnstile::bench
