# mpmc-work-queue-kata — a bounded MPMC work queue, and the slot that is reserved but not yet there

One systems-design question, asked three times in three languages — each from that language's own
cost model rather than translated from the others.

[**question.md**](question.md) — a security agent's work queue: 32 producer threads, 16 consumer
threads, 5 M events/sec, a fixed 65,536 slots, consumers that sleep when it is empty, producers that
block or reject when it is full, a shutdown that loses nothing accepted, and a p99 enqueue under 2 µs.
Plus a ten-line queue built from two atomic counters. Every bug in that code — the overwrite, the
consumer overtaking the producers, the read of a half-written slot — is a symptom of one thing: **it
cannot tell a slot that has been *reserved* from one that has been *published***, and with 32
producers finishing out of order, no pair of counters can. That is the clause the whole kata turns on.
The second half is the one it leads to: once the distinction is representable, every Principal-level
problem — preemption, lost wakeups, shutdown races, tail latency — is about how long a slot stays
reserved, and who is waiting while it does.

It is staged as a kata — eleven steps that build the argument in the order it has to be built, each
with probes, a hint, and a self-assessable gate, and the answer sketch folded at the bottom. Three
language tracks then ask what the shared question cannot:
[cpp/question.md](cpp/question.md) · [go/question.md](go/question.md) ·
[python/question.md](python/question.md).

## Why three languages, when the code is C++

The proposed queue is C++, but the central question is not: any bounded buffer with more than one
writer must answer it, and every language's standard library already has — once, in a form you can
read. What differs is *which states of a slot the language lets you see*, and watching the question
appear and disappear across the three is what shows which parts of the answer were about atomics and
which were about the problem.

So the brief is quoted rather than translated, and steps 0–10 are answerable in any of the three.
[The framing note](question.md#about-the-framing) says how to read the C++ if it is not your language,
and each track holds the specific form of the probes the shared steps could only ask generally.

| | what the language does to the question |
|---|---|
| **C++** | Shows you everything. A slot's storage and the object in it have separate lifetimes, so "constructed" and "published" are two facts you keep by hand — raw storage, a `noexcept` move, a sequence that certifies an object exists. Then every ordering is a choice, `fetch_add`'s progress guarantee depends on the instruction set, C++20's waiting primitives each lack something, and the one fence the protocol depends on is invisible to the thread sanitizer. |
| **Go** | Has already written the queue. A buffered channel is a bounded MPMC with blocking, rejection and close, and it answers the central question by hiding it inside one runtime lock — so the track starts by finding which steps that settles and whether 48-way contention on that lock meets the p99. A hand-written ring then loses its orderings and its lost wakeup to sequential consistency, and gains a runtime that preempts the producer for you. |
| **Python** | Makes the question vanish, then brings it back worse. In one interpreter the element is a reference built before the call, so reserving *is* publishing; there is no compare-and-swap with which to write the per-slot design even if you wanted to. The free-threaded build removes the lock that made that true. And `multiprocessing.Queue.put()` returns before the event is pickled or written — a documented, unbounded reserve→publish gap on another thread. |

## The two halves

Every track reaches the same two-part structure and disagrees about the second part.

**Make the distinction representable.** The proposed code uses one counter for two events: claiming a
position and announcing it. Separate them, and publication has to live somewhere a consumer can check
*per slot*, because 32 producers finish in an order that no single "published up to here" counter can
describe without making the fast ones wait for the slow. A lap-stamped sequence in each slot does it:
each side tests the slot rather than the other side's counter, a release/acquire pair runs in each
direction, and a slot that holds raw storage has a sequence that certifies an object is alive.

**Then bound how long it lasts.** A reservation is a debt the producer has to pay on every path, on
time. Preempted inside the window, it stops every consumer — the queue reports empty while holding up
to 65,535 events — and 65,536 slots at 5 M/s is 13 ms, about one scheduler timeslice, before the
producers see it as full. That makes the design *blocking* in the progress sense, with no lock in it
anywhere. The same debt reappears as a throwing move constructor, as a reservation made after
shutdown began, and as the producer's half of the wakeup handshake — four problems with one shape.

**And what Kata 1 does not carry over.** [Kata 1](../mpsc-event-pipeline-kata/) removed its shared
coordination point, because one consumer and per-producer ordering allowed it. Here sixteen consumers
must find work wherever it is, and the shared head is how they do it. The contended counter is not
overhead to design away; it is the load balancer — so the question becomes how few times each event
touches it, and whether a mutex, which makes the reserved state unobservable rather than
representable, already touches it few enough times.

## The answer, so far

| | thesis |
|---|---|
| [**cpp/solution.md**](cpp/solution.md) | Reservation lives in the counter, publication in the slot, as a lap-stamped sequence. Two entry points on one tail word: a CAS `try_push` that can refuse, and a ticket `push` that never retries and pays a tombstone if it lands after a close. The result is **blocking in the progress sense with no lock in it** — one descheduled producer stops every consumer, and on this machine that happens six times in eight million pushes. Against the brief: tickets meet the 2 µs p99 at 16 × 8 (1.90 µs) and miss it at 32 × 16 by half (2.96 µs), on 32 CPUs that the brief's 48 threads oversubscribe. |
| go/solution.md | not written — [go/question.md](go/question.md) |
| python/solution.md | not written — [python/question.md](python/question.md) |

## Notable results

- **The proposed queue's three bugs are one bug, and it loses a ninth of everything.** Four producers
  and four consumers on 1,024 slots: 11.4 % of events never delivered, 90,192 delivered twice, 1,030
  read torn or never written. ([cpp §2](cpp/solution.md#2-the-proposed-queue-broken))
- **A consumer stall makes the queue report full with 833 events in it.** A producer's hole stops
  every consumer immediately; a consumer's costs nothing for one lap and then refuses producers with
  the ring nearly empty — 62.9 ms after the stall began, against 65.5 ms by division.
  ([cpp §7](cpp/solution.md#7-preemption-blocking-with-no-lock-in-it))
- **A wake costs more than the budget.** Waking a sleeping consumer cost the waker 2.86 µs at the
  median. Allowing one wake in flight took the low-load push p99 from 57.6 µs to 2.43 µs — and made
  events wait longer for a consumer, so it is a trade, measured side by side, not a free win.
  ([cpp §6](cpp/solution.md#6-sleeping-and-the-wake-that-costs-more-than-the-budget))
- **The fence the wakeup depends on is invisible to ThreadSanitizer, and an x86 stress test finds
  it.** With the producer's fence removed, an engineered race lost 14 wakeups in 80,000; TSan, which
  does not model fences, reported nothing — while it found both relaxed-sequence bugs that x86
  hides. ([cpp §12](cpp/solution.md#the-bug--tool-table))
- **The reject policy is what misses the p99.** Look-then-claim is a CAS loop that retries 3.7 times
  per push at 32 producers; claim-then-look is a ticket that never retries. At 5 M/s the CAS's p99 is
  28.8 µs and the ticket's 2.96 µs. Backoff, the textbook fix, made the CAS worse.
  ([cpp §5](cpp/solution.md#5-full-two-policies-two-primitives),
  [§8](cpp/solution.md#8-cache-lines-contention-fairness))
- **A mutex with 64 events per acquisition beats both lock-free arms** — 16.7 M/s against 5.30 —
  and one event per acquisition reaches 0.66 M/s. It is the right answer whenever producers naturally
  have batches. ([cpp §11](cpp/solution.md#11-when-the-mutex-is-the-better-engineering-choice))
- **Windows throttled the benchmark to a quarter of its speed for not being in the foreground.**
  8 × 4 saturated: 2.16 M/s with EcoQoS on, 8.75 M/s off, in one run. Single-thread pinned numbers
  never moved, which is why it took a while to see.
  ([cpp §12](cpp/solution.md#four-measurement-problems-and-what-they-cost))

## Verify

```sh
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe"
cmake --build cpp/build
cpp/bin/solution.exe                   # exit code is the verdict; about five minutes
cpp/bin/solution.exe --phase 2,8       # the correctness phases only
cpp/bin/solution.exe --bug no-fence    # one row of the bug x tool table
```

Run it **twice and read the second**, and do not compare numbers across runs: on this laptop the clock
moves under sustained load, so each phase prints a pause instruction's cost as it starts, and arms
compared within a phase are interleaved. The compiler is pinned because CMake's default pick here is
the Clang bundled with Visual Studio, which this MSVC STL rejects.

<details>
<summary>ThreadSanitizer and the bug matrix (needs WSL)</summary>

TSan is unavailable on `x86_64-pc-windows-msvc`, as in
[mpsc-event-pipeline-kata](../mpsc-event-pipeline-kata/). Under WSL with gcc:

```sh
wsl -- bash -lc 'cd /mnt/c/.../mpmc-work-queue-kata/cpp &&
  cmake -S . -B build-tsan -DTURNSTILE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=g++ &&
  cmake --build build-tsan &&
  ./bin/solution_tsan --phase 2,8 &&
  for b in none relaxed-seq-load relaxed-seq-store no-full-check no-fence; do ./bin/solution_tsan --bug $b; done'
```

`--bug` exits 1 when the stress detected the bug; TSan exits 66 when it reported a race. Pass the
script to `wsl` as a file, or quote it as above — `wsl` runs its arguments through a shell first, and
an unquoted `$b` is expanded there, to nothing.

</details>

``` text
mpmc-work-queue-kata/
  question.md          the kata — 11 steps, the rubric, the folded answer sketch
  cpp/                 question.md solution.md  src/ bench/ CMakeLists.txt  -> bin/solution.exe
  go/question.md       G1–G6   the runtime already wrote it · the port without orderings · a sleep without a futex · runtime preemption · close panics · the tail
  python/question.md   P1–P6   the ceiling · reserving is publishing · read Lib/queue.py · lose the GIL · critique Queue.shutdown · multiprocessing's feeder
```

## Why turnstile

A **turnstile** admits people one at a time and counts every passage. The name earns its place
because each of its properties is one of the design's:

| the turnstile | the queue |
|---|---|
| one person through per turn, and the count goes up by one | one producer per position, and a sequence per slot that only ever grows |
| a person who stops halfway through blocks everyone behind | a producer descheduled between reserve and publish stops every consumer (§7) |
| a counter that says how many went through, not who is still in it | the shared tail says how many positions were reserved, not which are published — the brief's bug |
| at closing time the entrance locks and everyone inside is let out | a close refuses new reservations, and the drain waits for every position already taken (§9) |

The second row is why it is more than a picture. A turnstile is a lock-free-looking device — nothing
in it is locked — whose throughput is set by the slowest person currently in it.
