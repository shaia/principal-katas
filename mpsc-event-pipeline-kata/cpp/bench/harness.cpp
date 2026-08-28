#include "harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace weir::bench {
namespace {
int g_failures = 0;
}  // namespace

void check(bool cond, const std::string& what) {
    if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); ++g_failures; }
}

int failure_count() { return g_failures; }

std::int64_t now_ns() {
    return std::chrono::duration_cast<Nanos>(Clock::now().time_since_epoch()).count();
}

Event make_event(std::uint32_t id, std::uint32_t seq, std::int64_t stamp) {
    Event e{};
    e.producer_id = id;
    e.seq = seq;
    e.stamp_ns = stamp;
    std::memcpy(e.payload.data(), &seq, sizeof(seq));
    return e;
}

Percentiles percentiles(std::vector<std::int64_t>& v) {
    if (v.empty()) return {0, 0, 0, 0, 0};
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        const std::size_t i = std::min(v.size() - 1,
            static_cast<std::size_t>(q * static_cast<double>(v.size())));
        return static_cast<double>(v[i]) / 1000.0;   // us
    };
    return {at(0.50), at(0.99), at(0.999), at(0.9999),
            static_cast<double>(v.back()) / 1000.0};
}

}  // namespace weir::bench
