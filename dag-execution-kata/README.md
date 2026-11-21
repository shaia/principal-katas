# dag-execution-kata — counted is not published

[The shared question](question.md) stages Session 33 as eleven exercises. Start there, then choose the [C++](cpp/question.md), [Go](go/question.md), or [Python](python/question.md) track. Hints and answer sketches are folded. The [original reference](reference-outline.md) is preserved separately, including its hypothetical 16/64-worker comparison.

The [C++ worked answer](cpp/solution.md) includes runnable graph validation, failure propagation, cooperative cancellation, and draining, plus four scheduling arms. Go and Python are question-only tracks.

## Build and verify

From `dag-execution-kata/cpp` in a Visual Studio developer shell:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
./build/release/solution.exe --samples 10000 > results/smoke.csv
```

The Windows presets pin standalone Clang at `C:/Program Files/LLVM/bin/clang++.exe`. Override that cache variable for a different installation. On Linux use `linux-release`, or `tsan` (Clang and its sanitizer runtime must be installed):

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
./build/tsan/solution --bug relaxed-deps # deliberately racy; expect a TSan diagnostic
```

The default suite is correct code only. Tests have watchdog timeouts and return nonzero on failure; `million_nodes` is labeled `stress`. The bug mode is deliberately undefined C++ and must never be used for performance comparisons. A clean x86 execution does not make it valid.

## Measurements

```powershell
cmake --preset micro
cmake --build --preset micro
./build/micro/micro.exe --benchmark_min_time=0.2s --benchmark_min_warmup_time=1 --benchmark_repetitions=5 --benchmark_out=results/micro.json --benchmark_out_format=json
./build/release/solution.exe --full > results/full.csv
```

Only the optional `micro` preset fetches Google Benchmark v1.9.4. Standard builds use no third-party dependencies. The full matrix runs 1/4/16/32/64 workers, five graph shapes, two policies, 450 ns/1 us calibrated kernels, five repetitions, and at least 10,000 offered events per case. It takes substantially longer than the correctness suite. Use `--rate` to sweep offered event rate. Numeric arms are global=0, sharded=1, Chase–Lev=2; the sequentially consistent deque is a correctness/reference arm.

Results retain individual repetitions. Do not compare different hardware or background load as if it were a scheduler change. Performance budgets are reported, not asserted by CTest.
