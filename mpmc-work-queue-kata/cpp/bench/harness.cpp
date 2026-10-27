#include "harness.hpp"

#include <cstdio>
#include <cstdlib>

namespace turnstile::bench {

namespace {
int g_failures = 0;
}  // namespace

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

int failure_count() { return g_failures; }

std::string fmt_ns(double ns) {
    char buf[32];
    if (ns < 1e3) std::snprintf(buf, sizeof buf, "%.0f ns", ns);
    else if (ns < 1e6) std::snprintf(buf, sizeof buf, ns < 1e4 ? "%.2f us" : ns < 1e5 ? "%.1f us" : "%.0f us", ns / 1e3);
    else if (ns < 1e9) std::snprintf(buf, sizeof buf, ns < 1e7 ? "%.2f ms" : ns < 1e8 ? "%.1f ms" : "%.0f ms", ns / 1e6);
    else std::snprintf(buf, sizeof buf, "%.2f s", ns / 1e9);
    return buf;
}

std::string fmt_rate(double per_sec) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f M/s", per_sec / 1e6);
    return buf;
}

Watchdog::Watchdog(std::string what, std::chrono::seconds limit, int exit_code)
    : thread_([this, what = std::move(what), limit, exit_code] {
          std::unique_lock lk(mu_);
          if (!cv_.wait_for(lk, limit, [this] { return done_; })) {
              std::printf("  FAIL: %s did not finish within %lld s — hung\n", what.c_str(),
                          static_cast<long long>(limit.count()));
              std::fflush(stdout);
              std::_Exit(exit_code);
          }
      }) {}

Watchdog::~Watchdog() {
    {
        std::lock_guard lk(mu_);
        done_ = true;
    }
    cv_.notify_one();
    thread_.join();
}

}  // namespace turnstile::bench
