#pragma once
//
// The queue the brief proposes, verbatim apart from the includes it needs.
//
// It is here to be broken, by phase 1, on purpose. Every failure it has is a
// data race on a non-atomic T, which is undefined behaviour; with a trivially
// copyable 32-byte Event the compiled code does plain loads and stores, so what
// phase 1 observes is the hardware doing what the source said. That is a
// demonstration, not a licence — and the phase is compiled out of the
// ThreadSanitizer build, where the race is the only thing it would report.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace turnstile::brief {

using std::size_t;
using std::uint64_t;

template<typename T, size_t N>
class MPMCQueue {
    std::array<T, N> buffer_;

    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};

public:
    bool push(T value) {
        auto pos = tail_.fetch_add(1);
        buffer_[pos % N] = std::move(value);
        return true;
    }

    bool pop(T& result) {
        if (head_.load() == tail_.load())
            return false;

        auto pos = head_.fetch_add(1);
        result = std::move(buffer_[pos % N]);
        return true;
    }
};

}  // namespace turnstile::brief
