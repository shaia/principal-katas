#pragma once
//
// The output side. The consumer hands each batch to a Sink as one contiguous
// span; in production that Sink writes a socket.

#include "platform.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace weir {

template <typename S>
concept Sink = requires(S& s, std::span<const std::byte> b) {
    { s.write(b) } -> std::same_as<void>;
};

// ---------------------------------------------------------------------------
// The real one
// ---------------------------------------------------------------------------
//
// A socket sink has the same shape. A socket write is partial by nature, so the
// remainder has to be carried, and the fd must be non-blocking or the consumer
// stalls every ring behind one slow peer:
//
//   void write(std::span<const std::byte> b) {
//       pending_.insert(pending_.end(), b.begin(), b.end());
//       while (!pending_.empty()) {
//           auto n = ::send(fd_, pending_.data(), pending_.size(), MSG_NOSIGNAL);
//           if (n > 0) { pending_.erase(pending_.begin(), pending_.begin() + n); continue; }
//           if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // retry next pass
//           mark_failed(); return;
//       }
//   }
//
// When the peer stops draining, pending_ stops shrinking, the consumer falls
// behind, the rings fill, and the drop policy fires. That is the whole
// backpressure chain, and every link in it is bounded.

// ---------------------------------------------------------------------------
// Benchmark sinks
// ---------------------------------------------------------------------------

// Measures the queue rather than the kernel: counts bytes so accounting is
// verifiable, and touches the batch so the compiler cannot elide producing it.
//
// The sampling stride matters. A per-byte hash over a 64 KiB batch is a serial
// multiply chain tens of microseconds long — it would make the *sink* the
// bottleneck and every measurement would be reporting the checksum instead of
// the queue. Sampling keeps this O(1)-ish per batch.
class CountingSink {
public:
    void write(std::span<const std::byte> bytes) noexcept {
        std::uint64_t h = checksum_ ^ bytes.size();
        const std::size_t words = bytes.size() / sizeof(std::uint64_t);
        const std::size_t stride = std::max<std::size_t>(1, words / 32);
        for (std::size_t i = 0; i < words; i += stride) {
            std::uint64_t w;
            std::memcpy(&w, bytes.data() + i * sizeof(std::uint64_t), sizeof(w));
            h = (h ^ w) * 0x100000001b3ULL;
        }
        checksum_ = h;
        bytes_ += bytes.size();
        ++batches_;
    }
    std::uint64_t bytes() const noexcept { return bytes_; }
    std::uint64_t batches() const noexcept { return batches_; }
    std::uint64_t checksum() const noexcept { return checksum_; }

private:
    std::uint64_t checksum_{0xcbf29ce484222325ULL};
    std::uint64_t bytes_{0};
    std::uint64_t batches_{0};
};

// A deliberately slow sink, to drive the rings into overflow and exercise the
// drop policy. Stands in for a socket whose peer has stopped reading.
class SlowSink {
public:
    explicit SlowSink(Nanos delay_per_batch) : delay_(delay_per_batch) {}
    void write(std::span<const std::byte> bytes) noexcept {
        bytes_ += bytes.size();
        ++batches_;
        const auto until = Clock::now() + delay_;
        while (Clock::now() < until) WEIR_PAUSE();
    }
    std::uint64_t bytes() const noexcept { return bytes_; }
    std::uint64_t batches() const noexcept { return batches_; }

private:
    Nanos delay_;
    std::uint64_t bytes_{0};
    std::uint64_t batches_{0};
};

static_assert(Sink<CountingSink>);
static_assert(Sink<SlowSink>);

}  // namespace weir
