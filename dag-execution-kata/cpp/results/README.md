# Recorded native measurements

Date: 2026-11-15. Windows, x86-64, 32 logical CPUs; standalone Clang 21.1.7, Ninja, CMake 4.2.1. Release uses CMake default `-O3 -DNDEBUG`, no explicit ISA extension or LTO. Affinity and power/frequency were uncontrolled; other verification processes overlapped part of the run. Results therefore characterize this run, not an isolated hardware speed limit.

Commands, from `dag-execution-kata/cpp`:

```powershell
cmake --preset release
cmake --build --preset release
./build/release/solution.exe --samples 10000 > results/smoke.csv
cmake --preset micro
cmake --build --preset micro
./build/micro/micro.exe --benchmark_min_time=0.2s --benchmark_min_warmup_time=1 --benchmark_repetitions=5 --benchmark_out=results/micro.json --benchmark_out_format=json
```

Smoke: three repetitions, alternating arm order, one-second warmup per case; seed 33; 128 nodes/event, irregular DAG; 4 workers; work-first and priority enabled; integer kernel calibrated toward 450 ns; 10,000 intended arrivals at 2,000 events/s per repetition. Setup and oracle construction are excluded; run construction is included. The CSV contains each repetition. Counters include warmup; latency samples do not.

| Arm | Median event p99 (us) | Repetition p99 range (us) | Rejected |
|---|---:|---:|---:|
| Global mutex | 16366.3 | 16363.8–16555.5 | 0 |
| Sharded mutex | 15573.2 | 15538.4–15659.2 | 0 |
| Chase–Lev | 15539.1 | 15525.9–15561.3 | 0 |

All three miss the 5 ms p99 objective in this offered-arrival experiment. Windows sleep pacing creates late arrivals; the harness deliberately retains that delay rather than shifting the clock to the eventual submission time. These data do not isolate the scheduler as the cause or establish that 64 workers would improve the tail. Throughput is roughly the offered 2,000 events/s, so adequate throughput is compatible with a missed latency target.

The deque microbenchmark performs one owner push/pop pair, without thieves or task execution. Five repetitions gave real-time medians of 11.2 ns for the portable deque and 26.9 ns for the all-SC reference; real-time CVs were 2.28% and 3.99%. CPU-time and raw repetitions are in `micro.json`. This measures one tiny operation pair, not event latency or end-to-end scheduler speed. Cases were grouped by arm rather than interleaved, so thermal/order drift is an additional limitation.

Debug, Release, and native ASan correctness suites passed, including the million-node/64-worker case. ASan initially failed before main with Windows status 0xc0000139 because PATH supplied an incompatible Visual Studio runtime; CTest now prepends the standalone compiler's resource-directory runtime. This fix changes test execution environment, not the scheduler.

Linux TSan, ARM, multi-node NUMA behavior, and the full worker/workload matrix have not been run. WSL enumeration was denied in the available environment. NUMA affinity falls back on unknown topology and does not cover processor groups beyond zero. The mixed workload now offers one foreground event per three background events and reports class-specific p99. That matrix has not been measured, so priority benefit remains unestablished. The raw smoke CSV predates the added class-specific columns; its irregular workload and measured behavior are unchanged. Source hashes describe the final implementation, rather than claiming the CSV came from that exact final patch.
