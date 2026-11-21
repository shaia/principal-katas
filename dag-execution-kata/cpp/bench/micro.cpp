#include "deque.hpp"
#include <benchmark/benchmark.h>
template<bool SC> void local(benchmark::State& state) {
    dag::Deque<int,4096,SC> queue; int value=42;
    for(auto _ : state) {queue.push(&value);benchmark::DoNotOptimize(queue.pop());}
}
BENCHMARK_TEMPLATE(local,false);
BENCHMARK_TEMPLATE(local,true);
