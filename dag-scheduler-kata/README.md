# dag-scheduler-kata — a DAG executor, and the last predecessor that must have seen the others

One systems-design question, asked three times in three languages — each from that language's own
cost model rather than translated from the others.

[**question.md**](question.md) — a DAG engine whose tasks decrement their successors' dependency
counters with `memory_order_relaxed`; the predecessor that takes a counter to zero enqueues the
successor, which reads what *all* its predecessors wrote. Is relaxed correct? Design the scheduler and
its memory-ordering contract. The clause the kata turns on is the last one: **knowing you are last is
not the same as having seen the others.** The relaxed RMW chain guarantees the first and says nothing
about the second. Then the follow-up, which is a different kind of question: going from 16 to 64
workers raises throughput and makes p99 worse while steal attempts explode.

The brief is based on a real case: a C++20 parallel execution engine for an endpoint security
product, which turns every event on a machine into a DAG of analysis tasks, with up to a million
tasks active, 64 worker threads, and a p99 budget of 5 ms an event. The follow-up's numbers came with
the question and are illustrative. [cpp §10](cpp/solution.md#whether-the-table-adds-up) checks them
before explaining them.

It is staged as a kata: eleven steps and the follow-up, each with probes, a hint, and a
self-assessable gate, and the answer sketch folded at the bottom. Three language tracks then ask what
the shared question cannot:
[cpp/question.md](cpp/question.md) · [go/question.md](go/question.md) ·
[python/question.md](python/question.md).

The kata stands alone. It shares the event-count wakeup with
[mpmc-work-queue-kata](../mpmc-work-queue-kata/), which derives it, and nothing else.

## Why three languages, when the code is C++

The brief's first half is about the C++ memory model, and that is exactly why it is worth asking in
the other two: in one of them the bug cannot be typed, and in the other it changes kind. The second
half, which is the scheduler, is the same arithmetic in every language. What differs is how much of it
the runtime has already decided for you.

| | what the language does to the question |
|---|---|
| **C++** | The only language where the brief's bug can be written, and on x86-64 the only one where it is invisible: `relaxed` and `acq_rel` compile to the same `lock dec`. The contract has three correct spellings with different costs on AArch64, and a race detector reports one of them as a race. You also manage the graph's lifetime yourself, and the work-stealing deque has to publish, not only queue. |
| **Go** | `relaxed` does not exist. `sync/atomic` is sequentially consistent, so the brief's bug cannot be typed, and the runtime already is a work-stealing scheduler, with a `runnext` slot that acts as inline continuation and a cap on spinning threads that is the follow-up's redesign. What is left is whether to write a scheduler at all. |
| **Python** | The ordering question disappears and the counter becomes the bug. `pending -= 1` is not atomic even under the GIL, so the failure is a successor that runs twice or never, not a stale read. Then the free-threaded build, and a real DAG scheduler's ordering heuristic to read. |

## The answer, so far

| | thesis |
|---|---|
| [**cpp/solution.md**](cpp/solution.md) | Relaxed is wrong, and on x86-64 nothing but a race detector can tell: every spelling of the decrement compiles to the same `lock dec`. The contract is release on every decrement plus one acquire by the last predecessor, spelled as an acquire *load* of the counter so that ThreadSanitizer can vouch for it, plus a work-stealing deque that publishes rather than merely queues, and a graph whose shared part is immutable so a predecessor may still read it after the run has ended. Then the scheduler: inline continuation (14 ns a hop against 76–93 ns through the deque), fusion to a few microseconds a task, rank priority that Brent's bound left almost nothing to win, and failure that still resolves every edge once. The follow-up's amplification reproduces (thieves made the owner's hop four to six times slower; 1.61 × 10⁸ steal attempts/s, 98 % failing). Its worse tail does not, on one socket: spikes to 43 ms on one day did not recur in 26 later runs. On a 64-core, two-socket server it did, in every run, and most of it was where the OS put 65 spinning threads: pinned, 64 workers kept p99 under half a millisecond at 76 % load. What survives pinning is the cost of waking sleepers, which oversubscription multiplies. Sleeping hurt the tail in every run, a small search budget works only after fusion, and under a budget much of the tail is the producer's, which pays to wake sleepers. Tail claims come from repeated runs, because one run can rank the arms either way. |
| go/solution.md | not written — [go/question.md](go/question.md) |
| python/solution.md | not written — [python/question.md](python/question.md) |

## Notable results

- **Two million cross-core joins per ordering, and the brief's bug never showed.** Phase 1 checked
  every input against the value it must have, under seven orderings, with 95 % of joins having
  predecessors on different workers: 0 bad inputs from relaxed, release-only and acquire-only alike.
  The binary has no bug in it to find, because x86-64 has one locked instruction for all of them.
  ([cpp §2](cpp/solution.md#2-relaxed-read-in-machine-code))
- **ThreadSanitizer reported every ordering bug, and one correct spelling.** The release-plus-acquire-
  fence idiom is reported as a race, because TSan does not model standalone fences, so the executor
  spells the acquire as a load of the counter instead. That costs the same and is visible to the tool.
  ([cpp §8](cpp/solution.md#the-bug--tool-table))
- **A clean stress run once meant nothing.** The first version of the stress had only 0.2 % of joins
  with predecessors on different workers, because inline continuation keeps a graph where it started.
  Counting cross-worker joins is what made the zeros above evidence of anything.
  ([cpp §8](cpp/solution.md#measurement-notes))
- **The deque round trip costs 62 ns more than calling the successor.** Inline continuation takes a
  chain hop from 76 ns to 14 ns. Fusing 1 µs tasks eight to one takes efficiency from 56 % to 94 %,
  and fusing 512 to one gives some of it back to imbalance.
  ([cpp §6](cpp/solution.md#6-what-a-task-costs))
- **Critical-path priority had 27 µs to win, and FIFO stealing had already won most of it.** By
  Brent's bound any work-conserving schedule finishes the fork graph between 400 and 553 µs; the
  median was 427 µs. A buried chain step is the oldest task in its deque, so the first thief takes it.
  ([cpp §7](cpp/solution.md#7-the-critical-path-and-how-much-priority-can-win))
- **Fail-fast reports a failure in 40 µs, poison in 284 µs**, and fail-fast's floor is the longest
  body still running, because cancellation acts between tasks.
  ([cpp §9](cpp/solution.md#9-failure-and-cancellation))
- **Sixteen idle thieves made their victim's hop four to six times slower** with nothing to steal,
  and 64 greedy workers spent 21.0 cores doing 11.9 cores of work.
  ([cpp §10](cpp/solution.md#10-the-follow-up-stealing-amplification))
- **A tail that looked like the follow-up's, and was the machine's.** On one day, 64 greedy workers
  at 20,000 events/s had p99 spikes to 43 ms in four of eight runs. In 26 runs since, alternating the
  binary from that day with the current one, none went above 3 ms. Tracing every slow event found
  descheduled task bodies behind only 6–17 % of them in the greedy arms, and under a search budget
  found more than half of the late ones queued late by the producer itself, which pays to wake
  sleepers.
  ([cpp §10](cpp/solution.md#under-load))
- **On the follow-up's machine, the worse tail was mostly the OS's.** On a 64-core, two-socket AWS
  `c6i.metal`, 64 workers with a search budget of four had p99 3.3–4.0 ms in every run at 13,000
  events/s, against 429 µs for 16. 64 greedy workers fell seconds behind at 76 % load while 3–20 CPUs
  sat idle. A greedy pool is 64 spinning workers and a spinning producer, and the OS doubled them up
  on some CPUs and left others empty. Pinned, the same arms had 475–588 µs and 429–497 µs, in all five
  alternated pairs. What survived pinning was the scheduler's: 128 budgeted workers stayed at 4–6 ms
  at 13,000 events/s either way.
  ([cpp §10](cpp/solution.md#on-64-cores-and-two-sockets))
- **A search budget of four without fusion was the worst arm in six of eight runs**, at p99 2.3–82 ms.
  With tasks fused 8 to 1, the same budget came within a bucket or two of the greedy tail at under
  two thirds of the CPU. Granularity first, budget second. The textbook budget, half the workers, had
  p99 above 2 ms in every run at 64 workers, because it sleeps and wakes constantly.
  ([cpp §10](cpp/solution.md#the-redesign))
- **One run was not enough to rank tails.** At 90 % load every critical-path arm had at least one run
  with p99 above 10 ms, and the "worst" arm changed between runs. Medians reproduced to the bucket.
  ([cpp §7](cpp/solution.md#7-the-critical-path-and-how-much-priority-can-win))

## Verify

```sh
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe"
cmake --build cpp/build
cpp/bin/solution.exe                       # exit code is the verdict; about five and a half minutes
cpp/bin/solution.exe --phase 2             # the invariants only
cpp/bin/solution.exe --bug relaxed-decrement   # one row of the bug x tool table
```

Run it **twice and read the second**, and do not compare numbers across runs: arms compared within a
phase are interleaved, and each phase prints a pause instruction's cost as it starts, as a reading of
the clock. The compiler is pinned because CMake's default here is the Clang bundled with Visual Studio,
which this MSVC STL rejects. `CMakeLists.txt` finds `llvm-rc` beside the pinned compiler, because CMake
4's Windows-Clang module asks for a resource compiler and does not look there itself.

The machine code behind §2's claim is not part of the build. Any clang can produce it for all three
targets:

```sh
clang --target=x86_64-linux-gnu  -O2 -S -o - cpp/probe/ordering_codegen.c
clang --target=aarch64-linux-gnu -O2 -S -o - cpp/probe/ordering_codegen.c
clang --target=aarch64-linux-gnu -O2 -S -o - -march=armv8.1-a cpp/probe/ordering_codegen.c
```

<details>
<summary>ThreadSanitizer and the bug matrix (needs WSL)</summary>

TSan is unavailable on `x86_64-pc-windows-msvc`. Under WSL with gcc, put this in a file and run it
with `wsl -- bash <file>`. Passing it inline makes `wsl`'s own shell expand `$b` first, to nothing,
and from Git Bash you also need `MSYS_NO_PATHCONV=1`, or the `/mnt/c/...` path gets rewritten.

```sh
cd /mnt/c/.../dag-scheduler-kata/cpp
cmake -S . -B build-tsan -G Ninja -DKEYSTONE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=g++
cmake --build build-tsan
./bin/solution_tsan --phase 2
for b in none relaxed-decrement release-only acquire-only release-fence relaxed-publish double-resolve; do
  ./bin/solution_tsan --bug $b; echo "$b: exit $?"
done
```

`--bug` exits 1 when the stress detected the bug, and TSan exits 66 when it reported a race. A hang
counts as a detection after 15 s, or after 300 s under TSan, where a run that is printing reports can
take minutes and still finish.

</details>

<details>
<summary>On a Linux server (the follow-up's 64 cores)</summary>

Phase 6 sizes itself to the machine: 16, 32, 64, C and 2C workers, where C is the CPU count, at 12
cores of work and at 0.76 C. Phase 0 measures the cross-node line transfer when there is a second
NUMA node. From the kata directory on an Ubuntu machine:

```sh
cpp/remote/run.sh --smt-off              # toolchain, machine record, two full runs, phase 6 repeated
cpp/remote/run.sh --smt-off --pin-ab 5   # phase 6 in five pairs: OS-placed and pinned, alternating
```

It writes `remote-out/` and bundles it as `remote-out.tgz`. `--smt-off` makes one logical CPU one
core; on a bare-metal `c6i.metal` that leaves 64 cores across two sockets. `--quick` builds and runs
phases 0 and 2 only, to trial the script. `KEYSTONE_STALL_TRACE=0` turns phase 6's slow-event
attribution off, and the script runs phase 6 twice that way to check the tracing changes nothing.
On Linux phase 6 also prints table d, every CPU's busy time around each run, and `KEYSTONE_PIN=1`
pins the workers: the submitter on CPU 0, worker *i* on CPU 1 + *i* mod (C − 1).

</details>

``` text
dag-scheduler-kata/
  question.md          the kata: 11 steps, the follow-up, the rubric, the folded answer sketch
  reference-outline.md the answer outline that came with the question, checked in cpp §11
  cpp/                 question.md solution.md  src/ bench/ probe/ CMakeLists.txt  -> bin/solution.exe
                       C1–C5   annotate the executor · read the machine code · the deque · lifetime by hand · the bug × tool table
  go/question.md       G1–G6   the bug you cannot type · go per node · the runtime's answer to the follow-up · errgroup · the runtime's instruments · argue against the runtime
  python/question.md   P1–P5   the counter is the bug · lose the GIL · read Dask · where parallelism is · the follow-up in Python
```

## Why keystone

A **keystone** is the last stone set in an arch, and the arch stands only once it is in. Each of its
properties maps onto part of the design:

| the keystone | the executor |
|---|---|
| set last, after every other stone of the arch | the successor runs only after its last predecessor |
| it carries the load of every stone below it, not only its neighbours | the last predecessor must have *seen* every other predecessor's writes, not only its own: the acquire |
| until it is set, the arch is a pile of stones in falsework | until the sink completes, the graph instance is held up by threads that may still touch it (§4) |
| setting it twice, or not at all, brings the arch down | a dependency resolved twice or never is a successor that runs early or hangs (§8) |

The second row is the brief. The relaxed decrement knows that it is the keystone, and carries only its
own load.
