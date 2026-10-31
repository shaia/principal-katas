// Phase 1 — the brief's queue, broken on purpose.
//
// Two demonstrations, in the order step 1 asks for them.
//
//   1. The failure that needs one producer and no consumer at all: nine pushes
//      into eight slots. Deterministic, so it is a fact rather than an
//      observation.
//
//   2. The rest, under real threads on a small ring: events lost, events
//      delivered twice, and slots read before — or while — they were written.
//      Each delivery is checked against an exact record of what was pushed.
//
// Everything in (2) is a data race on a non-atomic T, which is undefined
// behaviour. With a trivially copyable 32-byte Event the compiled code does
// plain loads and stores, so what this phase sees is the hardware doing what the
// source said; it is a demonstration, not a licence. The ThreadSanitizer build
// compiles it out, because there the race is the only thing it would report.

#include "brief_queue.hpp"
#include "config.hpp"
#include "event.hpp"
#include "harness.hpp"
#include "phases.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace turnstile::bench {

void phase_brief(const Context&) {
#if defined(TURNSTILE_TSAN)
    std::printf("  skipped under ThreadSanitizer: the brief's queue is a data race by construction\n");
#else
    // 1. No consumer at all.
    {
        auto q = std::make_unique<brief::MPMCQueue<Event, 8>>();
        for (std::uint32_t i = 0; i < 9; ++i) q->push(make_event(0, i, 0));
        std::printf("  9 pushes into 8 slots, no consumer; then 9 pops return seq:");
        std::vector<std::uint32_t> got;
        Event                      e{};
        for (int i = 0; i < 9 && q->pop(e); ++i) {
            got.push_back(e.seq);
            std::printf(" %u", e.seq);
        }
        std::printf("\n");
        const bool overwrote = !got.empty() && got.front() == 8;
        std::printf("  push never checked for room: event 0 was overwritten by event 8, which is then "
                    "delivered %s\n",
                    std::count(got.begin(), got.end(), 8u) == 2 ? "twice" : "again");
        check(overwrote, "the brief's queue overwrites an unread event with no consumer present");
    }

    // 2. Under threads. A 1024-slot ring so that producers lap it constantly.
    {
        constexpr int           kP = 4, kC = 4;
        constexpr std::uint32_t kEach = 200'000;
        auto                    q     = std::make_unique<brief::MPMCQueue<Event, 1024>>();
        std::atomic<bool>       done{false};
        std::atomic<int>        producing{kP};
        std::vector<std::vector<std::uint64_t>> seen(kC);
        std::vector<std::uint64_t>              torn(kC, 0), pops(kC, 0);
        std::vector<std::thread>                ts;
        for (int c = 0; c < kC; ++c) {
            seen[static_cast<std::size_t>(c)].reserve(kP * kEach);
            ts.emplace_back([&, c] {
                Event e{};
                auto& mine = seen[static_cast<std::size_t>(c)];
                while (!done.load(std::memory_order_acquire)) {
                    if (!q->pop(e)) continue;
                    ++pops[static_cast<std::size_t>(c)];
                    if (intact(e) && e.producer < kP && e.seq < kEach)
                        mine.push_back(std::uint64_t{e.producer} << 32 | e.seq);
                    else
                        ++torn[static_cast<std::size_t>(c)];
                    // A runaway head can pop forever; stop collecting past a bound.
                    if (mine.size() + torn[static_cast<std::size_t>(c)] > 4u * kP * kEach) break;
                }
            });
        }
        for (int p = 0; p < kP; ++p)
            ts.emplace_back([&, p] {
                for (std::uint32_t i = 0; i < kEach; ++i) q->push(make_event(static_cast<std::uint32_t>(p), i, i));
                producing.fetch_sub(1, std::memory_order_acq_rel);
            });
        while (producing.load(std::memory_order_acquire) > 0) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the consumers drain
        done.store(true, std::memory_order_release);
        for (auto& t : ts) t.join();

        std::vector<std::uint64_t> all;
        std::uint64_t              torn_total = 0, pop_total = 0;
        for (int c = 0; c < kC; ++c) {
            all.insert(all.end(), seen[static_cast<std::size_t>(c)].begin(), seen[static_cast<std::size_t>(c)].end());
            torn_total += torn[static_cast<std::size_t>(c)];
            pop_total += pops[static_cast<std::size_t>(c)];
        }
        std::sort(all.begin(), all.end());
        std::uint64_t dup = 0, distinct = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (i > 0 && all[i] == all[i - 1]) ++dup;
            else ++distinct;
        }
        const std::uint64_t pushed = std::uint64_t{kP} * kEach;
        const std::uint64_t lost   = pushed - distinct;
        std::printf("  %d producers x %u pushes, %d consumers, 1024 slots:\n", kP, kEach, kC);
        std::printf("    pushes %llu, pops that returned true %llu\n", static_cast<unsigned long long>(pushed),
                    static_cast<unsigned long long>(pop_total));
        std::printf("    lost (never delivered)               %llu  (%.1f %%)\n", static_cast<unsigned long long>(lost),
                    100.0 * static_cast<double>(lost) / static_cast<double>(pushed));
        std::printf("    delivered more than once             %llu\n", static_cast<unsigned long long>(dup));
        std::printf("    read torn or never written           %llu\n", static_cast<unsigned long long>(torn_total));
        check(lost + dup + torn_total > 0, "the brief's queue loses, duplicates or tears events under threads");
    }
#endif
}

}  // namespace turnstile::bench
