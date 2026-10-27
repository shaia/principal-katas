#pragma once
//
// The queues under test, and the hooks the preemption and wakeup phases use to
// put a thread to sleep exactly where the real queue never would.

#include "config.hpp"
#include "event.hpp"
#include "mpmc_queue.hpp"
#include "mutex_queue.hpp"
#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

namespace turnstile::bench {

using Turnstile = Queue<Event, kCapacity>;
using Ticket    = Queue<Event, kCapacity, Options{.reserve = Reserve::ticket}>;
using Packed    = Queue<Event, kCapacity, Options{.padded = false}>;
using Backoff   = Queue<Event, kCapacity, Options{.backoff = true}>;

// The first versions of the two wake economies, kept to be measured beside the fixes.
using WakeEveryPush    = Queue<Event, kCapacity, Options{.coalesce_wakes = false}>;
using WakeEveryRelease = Queue<Event, kCapacity, Options{.batch_full_wakes = false}>;

// The rate the capacity arithmetic is checked at. The brief's 5 M/s is beyond what
// this machine sustains with 48 threads once its clock has dropped, and a queue
// that is full for that reason says nothing about N / rate — which holds at any
// rate. At 1 M/s the 65,536 slots last 65.5 ms.
inline constexpr double kStudyRate = 1.0e6;
using Locked    = MutexQueue<Event, kCapacity>;

// A pause of about n loop iterations — a nanosecond or less each — that the
// optimizer cannot delete. Finer than a pause instruction, which is tens of ns.
inline void spin_iterations(std::uint64_t n) noexcept {
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < n; ++i) sink = i;
    (void)sink;
}

// One stall, at one position, on one side. Shared by pointer so the hooks stay
// trivially copyable and the control block can live in the phase.
struct StallControl {
    static constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t>     producer_at{kNever};
    std::atomic<std::uint64_t>     consumer_at{kNever};
    std::chrono::milliseconds      duration{30};
    std::atomic<std::uint64_t>     began{0}, ended{0};  // TSC
};

struct StallHooks {
    StallControl* ctl = nullptr;

    void reserved(std::uint64_t pos) noexcept {
        if (pos == ctl->producer_at.load(std::memory_order_relaxed)) stall();
    }
    void claimed(std::uint64_t pos) noexcept {
        if (pos == ctl->consumer_at.load(std::memory_order_relaxed)) stall();
    }
    void parking() noexcept {}

    void stall() noexcept {
        ctl->began.store(TscClock::now(), std::memory_order_relaxed);
        std::this_thread::sleep_for(ctl->duration);
        ctl->ended.store(TscClock::now(), std::memory_order_relaxed);
    }
};

// Announces that the consumer is about to register as a sleeper, then waits a
// random few nanoseconds — so that a producer reacting to the announcement lands
// at a different offset from the consumer's re-check on every iteration, and some
// iterations land inside the store-buffering window.
struct ParkControl {
    std::atomic<std::uint64_t> parking{0};
    std::uint64_t              rng    = 0x9e3779b97f4a7c15ull;
    std::uint64_t              spread = 64;  // the consumer's random pause, in loop iterations
};

struct ParkHooks {
    ParkControl* ctl = nullptr;

    void reserved(std::uint64_t) noexcept {}
    void claimed(std::uint64_t) noexcept {}
    void parking() noexcept {
        ctl->parking.fetch_add(1, std::memory_order_release);
        ctl->rng ^= ctl->rng << 13, ctl->rng ^= ctl->rng >> 7, ctl->rng ^= ctl->rng << 17;
        spin_iterations(ctl->rng % ctl->spread);
    }
};

}  // namespace turnstile::bench
