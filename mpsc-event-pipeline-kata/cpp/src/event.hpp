#pragma once
//
// The unit of transfer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace weir {

// Trivially copyable and small: it lives by value inside the ring, so a push is
// a memcpy into preallocated storage. No allocation, no indirection, no shared
// ownership on the hot path.
//
// For payloads too large to inline, the extension is a fixed inline buffer for
// the common case plus a spill path for the rare large one — not a pointer,
// which would make the consumer's release of the payload the producer's
// synchronization problem, a second and harder protocol layered on this one.
struct Event {
    std::uint32_t producer_id;
    std::uint32_t seq;          // per-producer, strictly increasing
    std::int64_t  stamp_ns;     // *intended* send time — see the load generator
    std::array<std::byte, 16> payload;
};

static_assert(std::is_trivially_copyable_v<Event>);
static_assert(sizeof(Event) == 32, "keep Event small: 2 per cache line");

}  // namespace weir
