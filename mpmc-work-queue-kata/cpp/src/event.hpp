#pragma once
//
// The event that crosses the queue: 32 bytes, carrying enough to check every
// delivery the moment it arrives.

#include <cstdint>
#include <type_traits>

namespace turnstile {

struct Event {
    std::uint32_t producer;
    std::uint32_t seq;       // per producer: exactly-once, and per-producer order per consumer
    std::uint64_t stamp;     // TSC when the producer began the push: end-to-end latency
    std::uint64_t payload;
    std::uint64_t check;     // digest of the other four fields
};
static_assert(sizeof(Event) == 32);
static_assert(std::is_trivially_copyable_v<Event>);

// splitmix64's finalizer: cheap, and every input bit reaches every output bit.
constexpr std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

constexpr std::uint64_t digest(std::uint32_t producer, std::uint32_t seq, std::uint64_t stamp,
                               std::uint64_t payload) noexcept {
    return mix(mix(mix((std::uint64_t{producer} << 32 | seq) + 0x9e3779b97f4a7c15ull) ^ stamp) ^ payload);
}

// A slot read before it was written, or while it was being written, fails this,
// and so does a default-constructed Event — the digest of four zeros is not zero.
static_assert(digest(0, 0, 0, 0) != 0);

inline Event make_event(std::uint32_t producer, std::uint32_t seq, std::uint64_t stamp) noexcept {
    const std::uint64_t payload = mix(std::uint64_t{producer} << 32 | seq);
    return Event{producer, seq, stamp, payload, digest(producer, seq, stamp, payload)};
}

inline bool intact(const Event& e) noexcept {
    return e.payload == mix(std::uint64_t{e.producer} << 32 | e.seq) &&
           e.check == digest(e.producer, e.seq, e.stamp, e.payload);
}

// Exactly-once without a per-event record: every accepted event adds two
// independent hashes of its identity to the producer's sums, every consumed one
// adds them to the consumer's. Equal counts and equal sums mean the same
// multiset of events, except with probability around 2^-128 — and a lost event
// plus a duplicated one, which leave the count unchanged, still move both sums.
struct Tally {
    std::uint64_t n = 0, a = 0, b = 0;

    void add(std::uint32_t producer, std::uint32_t seq) noexcept {
        const std::uint64_t id = std::uint64_t{producer} << 32 | seq;
        ++n;
        a += mix(id ^ 0x2545f4914f6cdd1dull);
        b += mix(id * 0x9e3779b97f4a7c15ull + 1);
    }
    void merge(const Tally& o) noexcept { n += o.n, a += o.a, b += o.b; }
    bool operator==(const Tally&) const = default;
};

}  // namespace turnstile
