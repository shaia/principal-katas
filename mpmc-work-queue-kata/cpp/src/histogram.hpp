#pragma once
//
// A fixed-size latency histogram: log-linear buckets, about 3 % wide, over the
// whole 64-bit range. One per thread, so recording is a plain increment with no
// sharing and no allocation, and they are merged after the run.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace turnstile {

class Histogram {
public:
    void record(std::uint64_t v) noexcept {
        ++counts_[static_cast<std::size_t>(index(v))];
        ++n_;
        max_ = std::max(max_, v);
    }

    void merge(const Histogram& o) noexcept {
        for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        n_ += o.n_;
        max_ = std::max(max_, o.max_);
    }

    std::uint64_t count() const noexcept { return n_; }
    std::uint64_t max() const noexcept { return max_; }

    // The upper edge of the bucket holding the q-quantile. Conservative: it can
    // over-report by one bucket width and never under-reports, which is the
    // right way round for a number compared against a budget.
    std::uint64_t quantile(double q) const noexcept {
        if (n_ == 0) return 0;
        const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(n_ - 1)) + 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min(upper(static_cast<int>(i)), max_);
        }
        return max_;
    }

    // Samples strictly above v, to bucket resolution.
    std::uint64_t count_above(std::uint64_t v) const noexcept {
        std::uint64_t above = 0;
        for (int i = index(v) + 1; i < kBuckets; ++i) above += counts_[static_cast<std::size_t>(i)];
        return above;
    }

private:
    // Values below 32 get a bucket each. Above that, each power of two is split
    // into 16: index = 16·e + (v >> e), where e makes (v >> e) land in [16, 32).
    static constexpr int kBuckets = 976;

    static int index(std::uint64_t v) noexcept {
        if (v < 32) return static_cast<int>(v);
        const int e = std::bit_width(v) - 5;
        return 16 * e + static_cast<int>(v >> e);
    }
    static std::uint64_t upper(int i) noexcept {
        if (i < 32) return static_cast<std::uint64_t>(i);
        const int e = i / 16 - 1;
        const auto m = static_cast<std::uint64_t>(i - 16 * e);
        return ((m + 1) << e) - 1;
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t                       n_   = 0;
    std::uint64_t                       max_ = 0;
};

}  // namespace turnstile
