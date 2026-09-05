#include "harness.hpp"

#include <cstdio>

namespace switchyard::bench {

namespace {
int g_failures = 0;
}  // namespace

void check(bool cond, const std::string& what) {
    if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); ++g_failures; }
}

int failure_count() { return g_failures; }

}  // namespace switchyard::bench
