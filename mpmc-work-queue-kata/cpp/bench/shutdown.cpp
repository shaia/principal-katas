// Phase 8 — shutdown without losing what was accepted.
//
// "Accepted" means push returned ok. Every check here is the same identity —
// the multiset of events consumed equals the multiset accepted — under three
// shutdowns that each try to break it differently, all under a watchdog, because
// the failure mode of a wrong shutdown is as often a hang as a loss.
//
//   1. Close under load: 32 producers that never stop on their own, consumers too
//      slow for them, so the queue is full and producers are parked when the close
//      arrives. Every producer must learn of it by refusal; every accepted event
//      must still come out.
//
//   2. Close while a producer is stalled inside its window. Its position was
//      reserved before the close, so it is owed: the drain has to wait for it,
//      however long that takes, and then deliver everything behind it.
//
//   3. Close-versus-reserve, hammered: a small ring, producers pushing as fast as
//      they can, and the close landing at a different moment in each of twenty
//      rounds — once with CAS reservations, which a close refuses outright, and
//      once with tickets and CAS mixed, where a ticket taken after the close must
//      be paid with a tombstone. A reservation that slipped through unpaid would
//      either be lost or leave the consumers waiting for it forever.
//
// The reference outline's alternatives take 1 and 3 as well: its epoch wake, and
// one SPSC ring per producer, whose close races each producer's publication.

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

using Stalled = Queue<Event, kCapacity, Options{}, StallHooks>;

template <typename Q>
void close_under_load(const Context& ctx, const char* label, std::unique_ptr<Q> q) {
    RunSpec s;
    s.events           = 0;  // push until refused
    s.consumer_work_ns = 4000;
    s.mode             = PushMode::block;
    s.block_timeout    = std::chrono::seconds(10);
    s.close_after      = std::chrono::milliseconds(300);
    Watchdog        wd(label, std::chrono::seconds(60 * kScale));
    const RunResult r = run(*q, s, ctx);
    check_run(r, label);
    check(r.refused_closed == static_cast<std::uint64_t>(s.producers),
          std::string(label) + ": every producer learned of the close by refusal");
    // The mutex arm keeps no positions, so it has no backlog to report.
    const std::string backlog = requires { q->reserved(); } ? std::to_string(r.backlog_at_close) : "n/a";
    std::printf("  %-22s accepted %8llu  consumed %8llu  backlog at close %6s  drained in %s  refusals %llu\n",
                label, static_cast<unsigned long long>(r.accepted), static_cast<unsigned long long>(r.consumed),
                backlog.c_str(), fmt_ns(ctx.clk.to_ns(r.t_end > r.t_close ? r.t_end - r.t_close : 0)).c_str(),
                static_cast<unsigned long long>(r.refused_closed));
}

template <typename Q>
void close_versus_reserve(const Context& ctx, const char* label, PushMode mode) {
    constexpr int kRounds  = 20;
    int           clean    = 0;
    std::uint64_t accepted = 0;
    for (int round = 0; round < kRounds; ++round) {
        auto    q = std::make_unique<Q>();
        RunSpec s;
        s.producers   = 8;
        s.consumers   = 8;
        s.events      = 0;
        s.mode        = mode;
        s.close_after = std::chrono::microseconds(1000 + 997 * round);
        Watchdog        wd("close-versus-reserve round", std::chrono::seconds(30 * kScale));
        const RunResult r = run(*q, s, ctx);
        check_run(r, std::string("close-versus-reserve, ") + label);
        clean += r.exactly_once && r.refused_closed == 8;
        accepted += r.accepted;
    }
    std::printf("    %-22s %d of %d rounds clean, %llu events accepted in all\n", label, clean, kRounds,
                static_cast<unsigned long long>(accepted));
    check(clean == kRounds, std::string(label) + ": no round lost an accepted event or stranded a reservation");
}

}  // namespace

void phase_shutdown(const Context& ctx) {
    // 1.
    std::printf("  close after 300 ms, %d producers pushing until refused, consumers at 4 us per event:\n",
                kProducers);
    close_under_load(ctx, "turnstile (CAS)", std::make_unique<Turnstile>());
    close_under_load(ctx, "turnstile (ticket)", std::make_unique<Ticket>());
    close_under_load(ctx, "mutex + 2 condvars", std::make_unique<Locked>());
    close_under_load(ctx, "outline (CAS + epoch)", std::make_unique<Outline>());
    close_under_load(ctx, "per-producer SPSC", std::make_unique<PerProducer>());

    // 2.
    {
        StallControl ctl;
        ctl.duration = std::chrono::milliseconds(300);
        ctl.producer_at.store(100'000);
        auto    q = std::make_unique<Stalled>(std::chrono::milliseconds(50), StallHooks{&ctl});
        RunSpec s;
        s.events        = 0;
        s.mode          = PushMode::block;
        s.block_timeout = std::chrono::seconds(10);
        s.close_after   = std::chrono::milliseconds(150);
        Watchdog        wd("close during a stall", std::chrono::seconds(60 * kScale));
        const RunResult r = run(*q, s, ctx);
        check_run(r, "close during a producer's stall");
        auto ms = [&](std::uint64_t t) { return ctx.clk.to_ns(t - r.t_go) / 1e6; };
        const std::uint64_t ended = ctl.ended.load();
        std::printf("\n  close while a producer is stalled inside its window (300 ms, from position 100,000):\n");
        std::printf("    stall %.0f -> %.0f ms, close at %.0f ms, last event consumed at %.0f ms\n",
                    ms(ctl.began.load()), ms(ended), ms(r.t_close), ms(r.t_end));
        std::printf("    accepted %llu, consumed %llu, backlog at close %llu\n",
                    static_cast<unsigned long long>(r.accepted), static_cast<unsigned long long>(r.consumed),
                    static_cast<unsigned long long>(r.backlog_at_close));
        check(ended != 0 && r.t_end >= ended, "the drain waited for the position reserved before the close");
    }

    // 3.
    std::printf("\n  close-versus-reserve: 20 rounds each, 8 x 8 on a 64-slot ring, close at 1 to 20 ms:\n");
    close_versus_reserve<Queue<Event, 64>>(ctx, "CAS, reject on full", PushMode::reject);
    close_versus_reserve<Queue<Event, 64, Options{.reserve = Reserve::ticket}>>(ctx, "ticket + CAS mixed",
                                                                               PushMode::mixed);
    close_versus_reserve<ShardedQueue<Event, 64, 8, 8>>(ctx, "per-producer, reject", PushMode::reject);
    close_versus_reserve<ShardedQueue<Event, 64, 8, 8>>(ctx, "per-producer, block", PushMode::block);
}

}  // namespace turnstile::bench
