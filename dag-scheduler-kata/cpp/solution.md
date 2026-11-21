# A DAG executor, and the last predecessor that must have seen the others

Answer to [../question.md](../question.md) and the [C++ track](question.md), with a working
implementation and benchmarks alongside it. The Go and Python tracks are not answered yet; their
questions are at [../go/question.md](../go/question.md) and
[../python/question.md](../python/question.md).

Every number below came from running [`bin/solution.exe`](bench/), whose exit code is the verdict,
on an Intel Core Ultra 9 285K (8 P-cores, 16 E-cores, 24 logical CPUs, one socket) under Windows 11
with clang 23.1.6. The ThreadSanitizer column came from the same sources built with gcc 15.2 under
WSL (Ubuntu 26.04). Unless a section says otherwise, figures are from one full run, the second of two
back to back. Terms of art (TSO, LSE, release sequence, Brent's bound, …) are expanded in the
[Glossary](#appendix-glossary).

---

## The problem

An endpoint security product turns each event into a DAG of analysis tasks. The requirements: up to
1 million active tasks, 64 worker threads, 0–8 dependencies per task, no task before all its
predecessors finish, predecessors' results visible to dependents, no global scheduler lock, minimal
scheduling overhead, and p99 event latency under 5 ms. A developer writes:

```cpp
struct Task {
    std::atomic<uint32_t> pending_dependencies;
    std::vector<Task*> successors;
    std::function<void()> execute;
};

void complete(Task& task, ReadyQueue& ready)
{
    for (Task* next : task.successors)
        if (next->pending_dependencies.fetch_sub(1, std::memory_order_relaxed) == 1)
            ready.push(next);
}
```

`ReadyQueue` is assumed correctly synchronized. The developer argues that the atomic counter
guarantees the dependent runs only after all predecessors complete, so the code is correct. Is it,
under the C++20 memory model? Explain dependency completion against memory visibility, and design an
architecture that minimizes scheduling overhead while preserving correctness. The follow-up: going
from 16 to 64 workers raises throughput but worsens p99 from 4 ms to 18 ms while steal attempts
explode. The full text is in [../reference-outline.md](../reference-outline.md).

### What the brief gives you, and what it withholds

| The brief states | What it forces |
|---|---|
| the last predecessor enqueues the successor | exactly one thread owns the successor, and the counter's atomicity alone guarantees that, under any ordering (§3) |
| successors read data from **all** predecessors | every predecessor must publish, and the last one must acquire. The relaxed decrement does neither (§2) |
| `memory_order_relaxed` | on x86-64 the same instruction as the correct version, so the bug is invisible to every test on that hardware (§2, §8) |
| "Assume `ReadyQueue` is a correctly synchronized concurrent queue" | a second publication hop, which the brief assumes, and which stops being an assumption once "design the architecture" replaces the queue with per-worker deques. A textbook work-stealing deque does not provide it (§5) |
| "the dependent task executes only after all predecessors complete" | two words to dispute. *After* is wall-clock, not happens-before (§2). *Complete* is wrong too: the decrement happens partway through `complete()`, while the predecessor is still walking its successor list, which is the lifetime race (§4) |
| `std::vector` and `std::function` per task | an allocation per task at a million tasks, and a counter on the same cache line as fields the running worker reads. The executor splits an immutable shape from per-run state and pads each counter to a line (§4) |
| — | **no statement about when a graph may be freed or reused**, though a predecessor can still be running after the graph it belongs to has finished (§4) |
| 1 M active tasks, 64 workers, p99 < 5 ms | numbers that cannot all describe *ready* tasks. A million ready 450 ns tasks take 1 M × 450 ns / 64 ≈ 7 ms to drain on 64 workers. By Little's law, a million tasks in flight at the follow-up's 1.4 M tasks/s spend about 0.7 s each. So "active" must mean mostly waiting, and the number a design needs, how many are ready at once, is the one the brief does not give |
| 16 → 64 workers, p99 4 → 18 ms | a machine with more cores than this one, and probably two sockets. §10 reproduces the amplification and not the 18 ms, and says why |

### How the answer is graded

- **Senior**: DAG scheduling, atomic dependency counters, and why only the last predecessor
  schedules a task. → §3, §4
- **Strong Senior**: relaxed operations do not establish the required happens-before. → §2, §3
- **Staff**: correct acquire/release synchronization, efficient ready queues, work stealing.
  → §3, §5, §8
- **Principal**: critical-path limits, task granularity, memory locality, load balancing,
  cancellation, and scheduling progress guarantees. → §6, §7, §9, §10
- And the line the kata adds: **when not to steal at all.** → §1

### Where each part is answered

| Question | Section |
|---|---|
| The design, and the alternatives | §1 |
| Is relaxed correct? What it compiles to, and what can see the bug | §2 |
| The contract, in three spellings, and what each costs | §3 |
| When a predecessor may last touch the graph; pooling | §4 |
| The deque, and why it has to publish | §5 |
| What a task costs; inline continuation; fusion; the wide join | §6 |
| The critical path, and how much priority can win | §7 |
| The bug × tool table, and what this platform cannot measure | §8 |
| Failure and cancellation | §9 |
| The follow-up: stealing amplification, and the redesign | §10 |
| The reference outline, checked | §11 |

## Layout

``` text
cpp/
  src/    platform (TSC clock, timed wait-on-address), graph (immutable shape, pooled instance,
          generators), deque (bounded Chase–Lev), eventcount (sleep/wake), scheduler (the executor)
  bench/  seven phases, one per translation unit, plus --bug  -> bin/solution.exe
  probe/  ordering_codegen.c: the decrement under each ordering, for three targets (§2)
```

```sh
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe"
cmake --build cpp/build
cpp/bin/solution.exe                        # exit code is the verdict; run twice, read the second
cpp/bin/solution.exe --bug relaxed-decrement  # one row of the bug x tool table (§8)
```

---

## 1. The decision

**No, relaxed is not correct, and the fix is one word. The rest of the answer is about everything the
fix assumes.**

The contract ([`scheduler.hpp`](src/scheduler.hpp), at the top):

1. A node's body writes its output with plain stores.
2. Every decrement of a successor's counter is a **release**: each predecessor publishes its own
   output.
3. The decrement that reaches zero also **acquires**. The decrements of one counter form a release
   sequence, so one acquire by the last predecessor synchronizes with every earlier release.
4. Handing the successor to another worker is a **second publication**, through the deque's release
   push and acquire steal. Happens-before is transitive, so the thief sees every predecessor's output.
5. After its last decrement a predecessor **touches nothing in the graph**, unless that decrement made
   a node ready and so pinned the graph alive.

The executor is [`keystone::Scheduler`](src/scheduler.hpp):

- **The decrement** is `fetch_sub(1, release)`. Only the predecessor that reaches zero follows it with
  `load(acquire)` of the same counter. An edge into a node with one predecessor gets no atomic at all.
- **One bounded Chase–Lev deque per worker** ([`deque.hpp`](src/deque.hpp)), with no standalone
  fences, and slots stored with release and loaded with acquire, because it carries pointers to tasks
  whose contents the thief must see. A full deque means the task runs inline.
- **Inline continuation.** The predecessor that makes successors ready runs one of them itself, the
  one with the largest rank (bottom level), and pushes the rest in ascending rank.
- **An immutable shape and pooled instances** ([`graph.hpp`](src/graph.hpp)). Edges, costs and ranks
  are shared by every run, and a predecessor reads them after its last decrement. Counters, flags and
  payloads are per run, reset only by the thread that completes the sink.
- **Idle workers search under a budget** and the rest sleep on an event count. A searcher that finds
  work and was the last one searching wakes one more.
- **Failure resolves every edge exactly once**, propagating a poison flag; the fail-fast policy also
  skips every node not yet started.

### Why not the alternatives

**One global queue under a mutex** answers steps 2 and 4 by construction. The lock's release/acquire
carries the payload, and no deque protocol is needed. Its cost is a contended lock per task. §6
measures the scheduler's per-task cost at 14–93 ns on one worker's own deque. A shared lock taken by
16 workers per task costs at least the 222–240 ns that phase 0 measures for a 16-way contended RMW. At
the brief's task sizes, which the follow-up implies are small, that is the whole budget. Ship it when
tasks are tens of microseconds or more.

**One thread per graph** runs each event serially on one worker, with all the parallelism across
events and none within them. It needs no publication at all, because program order covers every edge.
Latency is the whole work of an event: 2,448 µs for §7's graphs, against 427 µs with 16 workers. Ship
it when there are always more events in flight than cores, and latency per event does not matter.

**Static partitioning** assigns nodes to cores ahead of time. Ship it when the shape is fixed and the
costs are known. Neither is true of a general engine, and work stealing exists for the case where they
are not.

---

## 2. Relaxed, read in machine code

What the brief's decrement and the correct one compile to, from
[`probe/ordering_codegen.c`](probe/ordering_codegen.c) with clang 23.1.6 at `-O2`. A predecessor
writes its output and decrements:

| | x86-64 | AArch64 ARMv8.0 (LL/SC) | AArch64 + LSE |
|---|---|---|---|
| relaxed (the brief) | `mov` · `lock decl` | `str` · `ldxr`/`stxr` | `str` · `ldadd` |
| release | `mov` · `lock decl` | `str` · `ldxr`/`stlxr` | `str` · `ldaddl` |
| acq_rel | `mov` · `lock decl` | `str` · `ldaxr`/`stlxr` | `str` · `ldaddal` |

The last predecessor decrements and reads another predecessor's output:

| | x86-64 | AArch64 ARMv8.0 |
|---|---|---|
| relaxed (the brief) | `lock decl` · `mov` | `ldxr`/`stxr` · `ldr` |
| acq_rel | `lock decl` · `mov` | `ldaxr`/`stlxr` · `ldr` |
| release + acquire fence | `lock decl` · `mov` | `ldxr`/`stlxr` · `dmb ishld` · `ldr` |
| release + acquire load | `lock decl` · `mov` · `mov` | `ldxr`/`stlxr` · `ldar` · `ldr` |

**On x86-64 every spelling is the same instruction.** A locked RMW is a full barrier, and x86 has no
weaker one. So the brief's code is, on this hardware, the correct code. The compiler did not move the
payload store across the relaxed RMW. It did not even delete the first of two stores on either side
of it (`finish_relaxed_twice` in the probe), though under the C++ model that store is dead. But the program is undefined, and a compiler that learns to sink a
store past a relaxed RMW is allowed to.

**On AArch64 the ordering is a suffix.** Under `relaxed` the predecessor's `str` can become visible
after its `ldadd`, and the last predecessor's `ldr` can be satisfied before its own `ldxr` completes.
Both ends of the edge are missing, and either is enough. This is how teams usually find the bug: the
same code, moved to Graviton or Apple silicon.

**What saw it here.** Phase 1 runs the stress with every input checked against the value it must
have, under all seven orderings. The settings are chosen so that the joins actually cross cores:
every ready successor goes through a deque, idle workers never sleep, bodies cost 300 ns, and only
two instances are in flight. Without those settings an earlier version tested 0.2 % of joins across
workers, and its zeros meant nothing.

| ordering | joins | cross-worker | stolen | bad inputs | correct? |
|---|---:|---:|---:|---:|---|
| relaxed | 2,320,000 | 2,203,149 | 1,268,072 | **0** | no |
| release-only | 2,320,000 | 2,204,644 | 1,268,513 | **0** | no |
| acquire-only | 2,320,000 | 2,207,065 | 1,273,721 | **0** | no |
| acq_rel | 2,320,000 | 2,202,433 | 1,266,029 | 0 | yes |
| release+fence | 2,320,000 | 2,203,161 | 1,265,286 | 0 | yes |
| release+load | 2,320,000 | 2,203,245 | 1,266,313 | 0 | yes |
| seq_cst | 2,320,000 | 2,201,166 | 1,262,787 | 0 | yes |

Two million joins per ordering whose predecessors ran on different cores, and not one stale input
from the broken orderings. That is not weak evidence of correctness. It is no evidence at all, because the binary
contains no bug for the stress to find. ThreadSanitizer, which judges the program against the C++
model, reports every one of the three (§8).

**And what correctness costs.** The same graph with empty bodies, in the default configuration,
interleaved: 184 to 188 ns of worker time per task under every ordering, a spread that does not
follow correctness. Phase 0 measures the decrement alone: 5–6 ns uncontended and 222–240 ns with 16
threads on one counter, the same under every ordering. The only extra the correct contract adds on
x86 is the release+load spelling's acquire load, about 3 ns, paid once per node by its last
predecessor.

---

## 3. The contract, spelled

Three correct spellings, and what each makes whom pay:

| spelling | every predecessor | the last predecessor | x86-64 | AArch64 | ThreadSanitizer |
|---|---|---|---|---|---|
| `fetch_sub(acq_rel)` | release + acquire | — | `lock dec` | `ldaddal` | sees it |
| `fetch_sub(release)`, then `atomic_thread_fence(acquire)` | release | a `dmb ishld` | `lock dec` | `ldaddl`, + `dmb ishld` once | **reports it as a race** |
| `fetch_sub(release)`, then `load(acquire)` | release | an `ldar` | `lock dec`, + `mov` once | `ldaddl`, + `ldar` once | sees it |

The executor ships the third. **Why the acquire load works:** the load reads the value the last
predecessor's own RMW wrote. That RMW lies in the release sequence headed by every earlier
predecessor's release decrement, because C++20 release sequences are made of RMWs on the same
location, whoever performs them. So the acquire load synchronizes with all of them, exactly as an
acquire fence would, and it is an operation the sanitizer models. It is the shared_ptr idiom with the
fence replaced by something a tool can check.

**The poison flag rides on the decrement.** A predecessor that failed or was skipped stores
`poisoned = 1` (relaxed) before its release decrement, and the last predecessor reads it after its
acquire. The flag needs no ordering of its own, for the same reason the payload needs none. It fails
under the brief's ordering for the same reason the payload does.

**The edge that needs no atomic.** A successor with exactly one predecessor is ready the moment that
predecessor finishes, and program order already puts the predecessor's writes before anything it does
next. The executor skips the RMW for those edges (`skip_single_pred_rmw`). §6 measures what that is
worth: 17 ns per hop when the successor goes through the deque.

**A trap for whoever tests this.** `resolve_edge` spells every ordering as a constant in its own
branch. GCC's expander turns a memory-order argument that is not a compile-time constant into
`MEMMODEL_SEQ_CST` (`get_memmodel` in `gcc/builtins.cc`). A harness that passed the ordering as a
variable would compile the "relaxed" arm as seq_cst, on AArch64 too, and conclude that the bug costs
nothing and cannot be found.

---

## 4. The last touch

The dependency counter is a reference count that runs its object instead of freeing it. The rule
about touching an object after dropping your reference applies here too.

**The race.** Predecessor `P` decrements its successors in a loop. After the last decrement, if it
reads anything in the graph instance (its own node, the next successor's counter, a flag), it races
with whoever completes the sink. That thread may already have reset the instance for the next event.
Nothing on x86 would show it, because the reset writes the same kinds of values.

**The fix is a split, not a lock.** Everything a predecessor reads after its last decrement lives in
the [`Shape`](src/graph.hpp): successor lists (both orders), predecessor lists, costs, ranks. A shape
is immutable and outlives every run of it. Everything per run (counters, poison flags, payloads,
bookkeeping) lives in the `Instance`, and `execute` touches an instance only before its last
decrement, or through a node that decrement made ready, which keeps the instance alive until it runs.

**Who resets.** Only the thread that completes the sink holds the instance at that moment, because
every node is an ancestor of the sink. It resets with plain stores, and the injection queue's lock
publishes the reset to the next run's workers. Instances are pooled, so "bounded memory" is the pool
size. An event that arrives with the pool empty is rejected and counted, never allocated.

**Layout.** `NodeState` is padded to a cache line, so two nodes' counters never share one: 64 bytes a
node, 256 KiB for §6's 4,096-leaf graph. Packed, the counters of a join's siblings would false-share
under exactly the contention §6 measures for a wide join.

**What the invariants check.** For every instance, the completing thread asserts that every node was
resolved exactly once, that no body ran twice, that every input a body read had the value it must
have, and that the failure semantics held (§9). Phase 2 runs that across 64 configurations — 1, 4, 32
and 64 workers; push or inline; id or rank order; greedy or budgeted; poison or fail-fast; and a deque
of 8 entries in a third of them, so that the overflow path runs too. 192,000 instances, 66.7 million
bodies, no violation, no hang. Under ThreadSanitizer the same matrix runs clean: 9,600 instances,
229,162 steals, 0 reports.

---

## 5. The deque

[`WsDeque`](src/deque.hpp) is a bounded Chase–Lev deque. The owner pushes and pops at the bottom
(LIFO), with no RMW on the common path, and thieves CAS the top (FIFO). The last element, wanted by a
pop and a steal at once, is settled by the same CAS from both sides.

**It has to publish, and the textbook version does not.** The C11 Chase–Lev of Lê, Pop, Cohen and
Zappa Nardelli (PPoPP 2013) proves its orderings for a deque of `int`: a thief reads the slot
`relaxed`, which is enough to get an integer. A deque of *pointers to tasks* is a publication channel,
and the thief must see what the owner wrote into the task before pushing it. Here the slot is stored
with release and loaded with acquire, and the owner's every store to `bottom` is at least a release.
On x86 both are plain `mov`s. The `relaxed-publish` bug in §8 is exactly this omission, and
ThreadSanitizer reports it.

**No standalone fences.** The C11 version's `seq_cst` fences in `pop` and `steal` are replaced by
`seq_cst` operations on the same locations: the pop's store to `bottom`, which must not pass its load
of `top` (the store-buffering shape), and the thief's loads of `top` and `bottom`. On x86 the pop pays
one locked instruction either way (an `xchg` instead of an `mfence`), and the sanitizer can see it.

**Bounded.** A growable deque allocates on the busiest path and leaves a retired buffer that a thief
may still be reading, which needs a reclamation scheme. A bounded one needs a policy for full, and
this one runs the task inline. That turns the bound on the deque into a bound on stack depth, which
the deque's size bounds in turn. Phase 2's 8-entry deques sent 6.1 million tasks down that path with
every invariant holding.

**`top` and `bottom` on separate lines.** The owner writes `bottom` on every push and pop; thieves
CAS `top`. On one line, every push would invalidate every thief's copy of `top`. §10 measures what
thieves cost a victim even with them apart.

**Injection.** New graphs enter through a mutex-protected ring with an atomic count, so that an idle
worker checks it with one relaxed load. A busy worker also checks it every 61 tasks — the Go runtime's
number for its global run queue — so that new events do not wait for an idle worker to exist.

**Sleeping.** The budgeted policy's sleep is the mpmc kata's [event count](src/eventcount.hpp): a
waiter announces, re-checks every deque and the injection queue, and then blocks. A pusher publishes,
fences, and wakes one sleeper only if nobody is currently searching. The fence costs 8 ns; with
nobody asleep that is all a push pays.

---

## 6. What a task costs

**A chain of empty tasks** has nothing to balance, so the time per hop is the scheduler's alone
(phase 3a, 4 workers, one instance at a time):

| successor handed on by | RMW on 1-predecessor edges | per hop |
|---|---|---:|
| push to own deque, pop it back | yes | 93 ns |
| push to own deque, pop it back | no | 76 ns |
| inline continuation | yes | 15 ns |
| inline continuation | no | 14 ns |

The deque round trip costs 62 ns more than calling the successor. Most of it is the push's release
stores, the event count's fence, and the pop's `seq_cst` store and load. Inline continuation removes
all of it, and the cache stays warm too. The skipped RMW is worth 17 ns on a deque round trip and
1 ns when the hop is already a function call: the locked instruction overlaps with nothing in the
first case and with the next task's start in the second.

**Grain and fusion** (phase 3b): 4,096 leaves under one source, 16 workers, two instances in flight.
Worker time per leaf's worth of work, and in brackets the efficiency, useful work over worker time:

| leaf size | 1 leaf per task | 8 per task | 64 per task | 512 per task |
|---|---:|---:|---:|---:|
| 0 | 883 ns | 123 ns | 21 ns | 6 ns |
| 0.25 µs | 1.32 µs (19 %) | 326 ns (77 %) | 278 ns (90 %) | 340 ns (74 %) |
| 1 µs | 1.79 µs (56 %) | 1.06 µs (94 %) | 1.05 µs (95 %) | 1.12 µs (89 %) |
| 4 µs | 4.45 µs (90 %) | 4.20 µs (95 %) | 4.22 µs (95 %) | 4.22 µs (95 %) |
| 16 µs | 16.8 µs (95 %) | 16.9 µs (95 %) | 16.9 µs (95 %) | 16.8 µs (95 %) |

An unfused leaf costs the scheduler about 880 ns of worker time here, not the 76 ns of a chain hop,
because 4,096 leaves are pushed one at a time by the single node that spawns them, while 15 workers
wait to steal (see the wide join below). A 1 µs task in that shape runs at 56 % efficiency. Fused eight
to one, it reaches 94 %, close to the 95 % at which the closed loop's per-instance resets cap
everything. Fused 512 to one, the instance becomes 8 tasks for 16 workers and efficiency falls back to
89 % (74 % at 0.25 µs): fusion has traded the balancing freedom away. The rule the table gives: **fuse until a task
is a few microseconds, and stop while there are still several tasks per worker.**

**The wide join and the serial spawner** (phase 3c). 4,096 empty leaves into one join: 230 µs per
instance flat, 190 µs with the decrements spread over 64 partial joins. The flat join's 4,096
decrements of one counter are not what dominates, though. The spawner is: one node pushing 4,096
successors is a serial loop of about 50 ns per push. A recursive fan-out, in which each node spawns a
handful of children that spawn the rest, would spread the spawning the way the tree spreads the
joining. It is not built here.

---

## 7. The critical path, and how much priority can win

Phase 4 runs two graphs with the same work (2,448 µs) and the same critical path (400 µs), open loop
on 16 workers:

- **fork**: a chain of 8 × 50 µs beside a fan-out of 512 × 4 µs. One decision, at the source.
- **pipeline**: 8 × 50 µs steps, each making 64 × 4 µs leaves ready beside the next step, so there is
  a decision at every step. The builder numbers each next step before the current step's leaves, so
  id order meets the step first, and a push-all scheduler pushes it first and pops it last.

Four arms: successors resolved in id order or rank order, and the last ready one pushed or run inline.

| graph | load | id, push | id, inline | rank, push | rank, inline |
|---|---|---:|---:|---:|---:|
| fork | 50 % p50 / p99 | 427 / 516 µs | 427 / 533 µs | 427 / 551 µs | 427 / 551 µs |
| fork | 90 % p50 / p99 | 409 µs / 1.21 ms | 409 µs / 1.92 ms | 409 µs / 1.78 ms | 409 / 676 µs |
| pipeline | 50 % p50 / p99 | 480 / 640 µs | 480 / 640 µs | 462 / 640 µs | 462 / 640 µs |
| pipeline | 90 % p50 / p99 | 516 / 996 µs | 516 / 782 µs | 498 µs / 1.49 ms | 480 µs / 1.71 ms |

**Rank priority bought 4–7 % of the pipeline's median, and nothing reproducible in the tail.** In this
one run, rank plus inline has the best p99 at 90 % on the fork (676 µs) and the worst on the pipeline
(1.71 ms). An earlier version of the phase ran at 95 %, where rank plus inline was the worst fork arm
at 17.6 ms in one run and 7.68 ms, behind a different "worst", in the next. Every arm's measured
capacity is within 1 % of every other's, about 6,280 events/s against a nominal 6,536, so 95 % nominal
was 99 % of real capacity. The tail near saturation is queueing, not ordering.

**Why so little: Brent's bound, and the thief.** Any schedule that never idles a worker while work
is ready finishes one event between max(W/P, CP) and W/P + CP. For the fork on 16 workers that is
between 400 µs and 553 µs. The measured median, 427 µs, is already within 27 µs of the floor. That 27
µs is everything a priority rule can win before queueing. And the deque's FIFO steal end already acts
as a priority rule. When the id-order arm buries the next pipeline step under its leaves, the step is
the *oldest* task in that deque, so it is the first thing any idle thief takes. Explicit rank only
matters when no thief is idle and CP is comparable to W/P. On 4 workers (the third case in phase 4)
W/P is 612 µs, more than CP, so the event is bound by work, and all four arms gave the same p50 within
a bucket.

**The outline's own example says the same, once the bound is closed.** 10,000 tasks of 1 µs with a
2,000-task chain: W = 10 ms, CP = 2 ms, and on 64 workers W/P = 156 µs. The outline stops at the lower
bound, 2 ms. The upper bound is W/P + CP ≈ 2.16 ms, within 8 % of it, so no ordering can win much
there either. And W/CP = 5: the graph keeps about five workers busy, and the other 59 can only look
for work. Against the 5 ms target, each of the 2,000 hops on the chain may add (5 − 2) ms / 2,000 =
1.5 µs. §6's deque round trip fits in that many times over. A park and wake on the chain does not: at
phase 0's 10.6 µs median wake, 2,000 hops would cost about 21 ms. That is arithmetic, not a
measurement, but it puts the follow-up's 18 ms within reach of sleeping alone (§10).

**Starvation**, the outline's risk for critical-path priority, did not show either. Rank arms had no
systematically worse maximum. It would need a steady supply of high-rank work that keeps low-rank
tasks waiting, and with inline continuation the high-rank work is one chain per event, run by one
worker.

**One thing that does go wrong with rank plus inline**, and is worth knowing even though the
measurement cannot separate it from noise: the worker that runs the 400 µs chain inline leaves the
fan-out it displaced in its own deque, where only a thief can reach it. When nobody is idle, that
work waits for the chain. Running the critical path yourself is right; doing it while holding the
event's other work where nobody else looks is the cost.

---

## 8. Verification, and what this platform cannot measure

### The bug × tool table

Five deliberate bugs and one correct spelling, each run with `--bug <name>` under phase 1's stress:
six random DAGs, 40,000 instances each, about 14.3 million cross-worker joins and 8.7 million steals
per row on x86. x86 stress: the Windows binary. ThreadSanitizer: the same sources under gcc 15.2 in
WSL, with instance counts divided by 20, about 730,000 cross-worker joins per row.

| row | kind | x86 stress | ThreadSanitizer | what that means |
|---|---|---|---|---|
| none (control) | — | clean | 0 reports | neither instrument cries wolf on the correct protocol |
| relaxed decrement (the brief) | ordering | not detected | **13 reports** | TSO and the compiler hide it; the C++ model does not |
| release-only (last does not acquire) | ordering | not detected | **13 reports** | half the contract is no contract |
| acquire-only (nobody releases) | ordering | not detected | **13 reports** | the other half |
| deque push publishes nothing | ordering | not detected | **20 reports** | the second hop, which the brief assumes |
| failed node resolves its edges twice | counting | **hangs** (15 s watchdog) | **hangs** (300 s), 0 reports | a counting bug is not a race: only the hang shows it |
| release + acquire fence — *correct* | — | clean | **13 reports** | the sanitizer does not model fences |

**Sort the bugs before predicting.** Counting bugs break exactly-once, so they show on any machine,
usually as a hang. If the extra decrement is the one that reaches zero, its result is thrown away and
the successor never runs, and the next decrement wraps the counter to 2³² − 1. If it is not, the
successor becomes ready one predecessor early, and the graph can complete while a node is still
running. Ordering bugs break visibility, so on x86 they are unobservable, and under a tool that judges
the program against the C++ model they are reported every time.

**What the reports name** is the whole contract at once. In every ordering row, they pair a body's
read of a predecessor's `out` with that predecessor's write. They also pair the completing thread's
read of which worker ran each node with the write, and the completing thread's *reset* of the
instance (its generation, flags, payloads and expected values) with the previous run's accesses. The
missing acquire at a join is also a missing acquire at the sink, so §4's reuse of the instance is a
race too. The deque row adds the thief's reads of the task it took.

**The correct row that goes red.** gcc warns at every instantiation that `atomic_thread_fence` is not
supported with `-fsanitize=thread` (the TSan build silences that, with a comment saying why). So the
shared_ptr idiom, correct and as cheap as it gets on AArch64, is reported as a race. That is why the
executor ships the release+load spelling (§3): it costs the same, and the tool can vouch for it. The
event count still has its fences (§5), but they order only atomics, so what the sanitizer cannot see
there is a lost wakeup, never a race. Phase 2 runs with a 30-second park backstop and a watchdog, so a
lost wakeup would be a hang.

### What cannot be measured here

- **AArch64.** No machine to run it on. The three ordering bugs and the deque bug are the rows an
  AArch64 stress test would be expected to catch some of the time. On x86 only TSan sees them.
- **A model checker.** None installed. GenMC or CDSChecker would explore a four-node diamond
  exhaustively, under the C11 model, in seconds.
- **NUMA.** One socket. Every claim about remote steals and first-touch placement in §10 is reasoning,
  and phase 0 says so as it starts.
- **The follow-up's machine.** 64 workers here are 64 threads on 24 CPUs, not 64 cores.

### Measurement notes

- Every phase prints a pause instruction's cost as it starts. It is a fixed cycle count, so it reads
  the clock: 8 ns on a P-core when the clock is up. Arms within a phase are interleaved, and the
  numbers quoted are medians of 3 or 5.
- Latency runs from when an event was **due**, not when the submitter got to it, and the submitter
  spins for the last 2.5 ms rather than sleep through it. A sleep returns up to a timer tick late,
  and that lateness would be charged to the scheduler.
- `cores` in §10 is process CPU time per second of wall time, minus the submitter thread's own.
- The first version of the stress tested publication on 0.2 % of joins, because inline continuation
  and a full pipeline of instances keep a graph on the worker that started it. The cross-worker join
  count exists so that a clean stress has to show what it covered.

### Known gaps

- No recursive fan-out: one node spawning 4,096 successors pushes them serially (§6).
- No steal-half: a thief takes one task. The Go runtime takes half a victim's queue.
- No NUMA-aware victim order, and no hybrid-aware one, though E-cores are the closest thing this
  machine has to a remote node: a line moves from a P-core to another P-core in 36 ns and to an E-core
  in 77 ns.
- The budgeted policy's "last searcher wakes one more" chain ramps sleepers up one wake at a time, each
  10.6 µs at the median and 93.3 µs at p99 before the sleeper runs (phase 0). §10 shows that cost in
  the tail.
- `Stats::rescued` counts backstop timeouts that found work with nobody searching, a symptom of a lost
  wakeup. Nothing prints it. Phase 2 makes the same check stronger with a 30-second backstop and a
  watchdog.

---

## 9. Failure and cancellation

A failed node is a node that finishes. It resolves every out-edge exactly once, storing the poison
flag before each release decrement, so its successors are skipped, resolve their own edges, and the
graph reaches its sink exactly as it would have. That walk is how the completing thread knows nobody
still holds a pointer into the instance (§4). Abandoning the graph instead leaves no moment at which
it can be reused.

Two policies, at 60 % load with one event in five failing at a random interior node (phase 5):

| policy | failure → completion, p50 | p99 | bodies run after the failure was known |
|---|---:|---:|---:|
| poison: only descendants are skipped | 284 µs | 391 µs | 251 per failed event |
| fail-fast: every node not yet started is skipped | 40.0 µs | 57.8 µs | 0 |

Under poison a failed event still runs everything that does not depend on the failure, typically the
chain and most of the leaves, and reports when they finish. Under fail-fast it reports about 40 µs
later. That is the **longest body still running when the flag went up**, here a 50 µs chain step,
because cancellation is checked between tasks, never inside one. Cancellation latency is bounded below
by task size, which is one more reason for the grain §6 recommends. Which policy is right is a product
question: poison keeps the partial results, and fail-fast frees the workers.

This is the outline's extension, "separate task completion accounting from successful result
publication", made concrete. The counter accounts for completion and is decremented by every
predecessor whatever happened to it. The poison flag, stored before the same release decrement, says
whether the inputs are usable. A node here ends in one of three states: ran, failed, or skipped. A
finer split is worth having when the caller must report *why* a node did not run: *skipped* because a
predecessor did not succeed, against *canceled* because the run was canceled before the node was
dispatched. It costs nothing extra, because the state is written before the release decrement like
the flag. Neither split lets a canceled body be interrupted. A body that is already running finishes,
and may succeed after the cancel.

Every failed instance in phase 2 and phase 5 was checked against its expected run set. Under poison,
a body ran exactly when nothing upstream had failed. Under fail-fast, it never ran when something
upstream had.

---

## 10. The follow-up: stealing amplification

> Going from 16 to 64 workers raises throughput but worsens p99 from 4 ms to 18 ms while steal
> attempts explode.

### Whether the table adds up

The session gives the follow-up as a table, and the table is hypothetical. Before explaining it, it is
worth checking, using nothing but its own numbers:

| | 16 workers | 64 workers |
|---|---:|---:|
| cores busy (utilization × workers) | 14.1 | 26.9 |
| cores of task work (tasks/s × 450 ns) | 0.38 | 0.63 |
| task work as a share of busy CPU | 2.7 % | 2.3 % |
| steal attempts per worker per second | 12.5 K | 141 K |

Three things follow. First, task bodies are under 3 % of the CPU in *both* columns, so the
scheduler, or something the table does not name, was 97 % of the cost before any thief was added.
Second, steal attempts rise 45× for 4× the workers, while utilization *falls* from 88 % to 42 %: the
extra thieves spend most of their time asleep, not spinning. That points at the park and wake cost
§7 priced, rather than at spinning thieves. Third, p50 improves while p99 gets worse. That is a split
population: events that stay on one worker got faster, and events whose critical path crosses a wake
or a remote steal got much slower.

This machine runs a different regime. Below, 64 greedy workers spent 20.7 cores on 11.9 cores of
work, 57 % useful against the table's 2–3 %. What reproduces here is the amplification and the cost
of sleeping. The table's ratios do not.

### What a thief costs the work it steals from

Phase 6a: a chain of empty tasks, each successor pushed and popped back by its owner unless a thief
takes it first. Only one task exists at a time, so every other worker is a thief.

| workers | greedy: per hop | stolen | tries per hop | budgeted: per hop | stolen | tries per hop |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 32 ns | 0 % | 0 | 37 ns | 0 % | 0 |
| 4 | 114 ns | 9.1 % | 2.8 | 111 ns | 4.4 % | 0.57 |
| 16 | 153 ns | 17.2 % | 23.4 | 112 ns | 7.4 % | 5.0 |
| 24 | 150 ns | 16.2 % | 31.5 | 113 ns | 8.5 % | 9.7 |
| 64 | 69 ns | 5.1 % | 20.5 | 149 ns | 11.1 % | 37.9 |

**That is stealing amplification, in one table.** With 16 greedy thieves the owner's hop is nearly
five times slower than alone, though no thief has anything to do. Every failed steal reads the
victim's `top` and `bottom`, and every successful one moves a task, and its inputs, to a cold core.
Tries per hop grow with the number of thieves, not with the amount of work. The "budgeted" column
allows half the workers to search, and at 64 workers that is 32 searchers. It is not much of a
budget, and its tries per hop are the highest in the table. At 64 workers on 24 CPUs the greedy
column gets *faster* again, because 40 of its thieves are off-CPU at any moment and the rest yield
after every sweep. That is a property of oversubscription, not a fix.

### Under load

Phase 6b: events of a 4 × 100 µs chain beside 256 leaves of 2 µs, 912 µs of work, open loop.

**At 13,000 events/s**, 11.9 cores of work, which 16 workers sustain:

| workers, policy | capacity/s | p50 | p99 | steal tries/s | success | cores |
|---|---:|---:|---:|---:|---:|---:|
| 16 greedy | 16,763 | 409 µs | 444 µs | 4.98 × 10⁷ | 5.66 % | 15.6 |
| 16 budgeted | 16,890 | 409 µs | 533 µs | 2.72 × 10⁷ | 10.27 % | 13.6 |
| 64 greedy | 23,066 | 409 µs | 498 µs | 1.58 × 10⁸ | 1.72 % | 20.7 |
| 64 budgeted (32 searchers) | 24,315 | 409 µs | 533 µs | 1.51 × 10⁸ | 1.87 % | 15.8 |
| 64 greedy, fused × 8 | 25,342 | 409 µs | 480 µs | 1.47 × 10⁸ | 0.27 % | 20.5 |
| 64, budget 4 | 23,620 | 427 µs | 569 µs | 1.66 × 10⁸ | 1.74 % | 18.6 |
| 64, budget 4, fused × 8 | 24,204 | 409 µs | 498 µs | 4.92 × 10⁷ | 0.77 % | 13.6 |
| 24, budget 4, fused × 8 | 24,519 | 409 µs | 462 µs | 1.61 × 10⁷ | 2.36 % | 12.9 |

**At 20,000 events/s**, 18.2 cores of work, which 16 workers cannot sustain:

| workers, policy | p50 | p99 | p99.9 | cores |
|---|---:|---:|---:|---:|
| 16 greedy or budgeted | 100–105 ms | 1.64 s | 1.64 s | 15.8 |
| 24 greedy | 409 µs | 889 µs | 1.78 ms | 20.9 |
| 24 budgeted | 409 µs | 1.85 ms | 5.12 ms | 20.5 |
| 64 greedy | 409 µs | 960 µs | 1.92 ms | 21.1 |
| 64 budgeted (32 searchers) | 409 µs | 2.56 ms | 14.2 ms | 20.9 |
| 64 greedy, fused × 8 | 409 µs | 604 µs | 1.07 ms | 20.7 |
| 64, budget 4 | 747 µs | **68.3 ms** | 81.9 ms | 20.9 |
| 64, budget 4, fused × 8 | 409 µs | 640 µs | 1.21 ms | 20.3 |
| 24, budget 4, fused × 8 | 409 µs | 516 µs | 676 µs | 19.2 |

What reproduced, and what did not:

- **Throughput rises with workers**, from 16,763 events/s at 16 to 23,000–25,000 at 24 and above, and
  stops at the CPU count. Past 24 workers on 24 CPUs there is nothing left to add.
- **Steal attempts explode.** 4.98 × 10⁷/s at 16 greedy workers, 1.58 × 10⁸/s at 64, with the
  success rate falling from 5.7 % to 1.7 %. Ninety-eight attempts in a hundred find nothing, and 64
  greedy workers spent 20.7 cores doing 11.9 cores of work.
- **The p99 did not get worse with greedy thieves.** At 13,000/s it went from 444 µs at 16 workers to
  498 µs at 64; at 20,000/s it was 889 µs at 24 and 960 µs at 64. On this machine a spinning thief
  yields after every sweep and Windows hands its CPU straight to a worker with work, so
  oversubscription costs CPU and not latency.
- **What did hurt the tail was sleeping.** A sleeper takes 10.6 µs at the median, 93.3 µs at p99, to
  run after a wake (phase 0). Tokio's rule, half the workers may search, parks constantly, and at
  20,000/s its p99 was 2.56 ms and its p99.9 14.2 ms, against greedy's 960 µs and 1.92 ms.

So the follow-up's 4 → 18 ms needs something this machine does not have. On a 64-core, two-socket
server, the 48 extra workers are real cores running real thieves. They do not yield to the workers.
They pull the workers' `top` and `bottom` lines across the interconnect, which is phase 6a's 5× per
hop at cross-socket transfer prices. And every successful steal moves a task away from its inputs:
half of those moves cross a socket, and the stolen task then reads its predecessors' payloads
remotely. That is the 18 ms: the same work done farther from its data, with the owners' fast paths
taxed by thieves. It is reasoning, and the last paragraph of this section says how to check it.

### The redesign

Phase 6b runs it one change at a time, and the order turns out to matter:

1. **Task granularity first.** Fusing leaves 8 to 1 alone, still greedy, gave the best greedy tail in
   the table (604 µs at 20,000/s against 960 µs) and cut the steal success rate to 0.27 %, because
   there were an eighth as many tasks to find. It did not save a single core.
2. **A small, fixed worker budget, second, and only second.** At most four searchers, the rest asleep,
   and a searcher that finds work wakes one more. *Without* fusion this was the worst arm measured:
   p99 68.3 ms at 20,000/s, with the median at 747 µs. Four searchers handing out 256 two-microsecond
   leaves per event, waking helpers one at a time at about 10 µs a wake, cannot keep up, and the
   backlog compounds. *With* fusion, the same budget matched greedy's tail (640 µs at 20,000/s, 498 µs
   at 13,000/s) at 13.6 cores instead of 20.7. A budget is only safe once tasks are coarse enough that
   a few searchers can distribute them.
3. **Workers equal to cores, not more.** The best arm in both tables is 24 workers, budget 4, fused:
   p99 462 µs at 13,000/s on 12.9 cores, and 516 µs at 20,000/s. On the 64-core machine, measure
   whether 64 workers beat 48 before keeping them; the cores left over are worth more to the process's
   I/O and to the OS.
4. **NUMA, by reasoning.** One deque set per socket. Steal locally first, remotely only after a failed
   local sweep and with a backoff, and then take a batch (steal-half) to amortize the transfer.
   Allocate each graph instance on the socket of the worker that injects it, which first touch does if
   the pool is per socket. Keep a graph on one socket unless it is wide enough to need two. None of this
   is measured here.
5. **Priority stays where §7 put it.** Run the critical successor inline, let the FIFO steal end do the
   rest, and keep rank as a tie-breaker. Priority cannot win back what a remote steal costs.

The Go runtime's version of item 2 is different from the one that failed here. Spinning threads may
not exceed half of the *busy* ones, rather than half of all, so the budget shrinks as idleness grows.

What to measure first on the 64-core machine, before changing anything: steal attempts and successes
per worker, split by local and remote victim; then the time a task waits between ready and running,
split by whether it was stolen; then CPU time spent in the idle loop. If the 18 ms is remote steals,
the second shows it. If it is thieves taxing owners, the first and third do.

---

## 11. The reference outline, checked

The question came with an outline, Session 33's sections 3–7
([../reference-outline.md](../reference-outline.md)). Claim by claim:

| the outline says | measured here |
|---|---|
| "the relaxed decrement by A does not establish a happens-before relationship between A's write and C's read", so C's read "can constitute a data race … even if the implementation happens to work on a particular x86-64 system" | Right, and stronger than "happens to work". On x86-64, relaxed and acq_rel compile to the same `lock dec`, so no test on that hardware can tell them apart: two million cross-core joins per ordering, 0 bad inputs. Only ThreadSanitizer reported it (§2, §8). |
| "The ready queue can publish B's preceding writes because B enqueues C. It does not automatically publish unrelated writes performed by A." | Right. That is the brief's bug in two sentences (§2). |
| `fetch_sub(acq_rel)`, because "the acquire/release relationships establish transitive synchronization" | Right as stated, and more than is needed. Through release sequences, release on every decrement plus one acquire by the last predecessor is enough, which is cheaper on AArch64 and the same on x86 (§2, §3). The executor spells the acquire as a load, so that TSan can vouch for it (§8). |
| invariants: the graph fully initialized before execution, each predecessor decrements exactly once, result storage stays alive, predecessors stop modifying what successors read | Right, and one short. "Result storage" covers the payloads. It does not cover the successor list the completing predecessor is still walking after its last decrement, which is the lifetime race (§4). "Exactly once" is the assumption a failure path breaks: resolving twice hangs (§8), so failure resolves every edge once (§9). |
| "assuming the queue provides the necessary release/acquire handoff"; the deque's protocol "must be proven separately" | Right, and the warning is earned. The textbook C11 Chase–Lev thief reads the slot `relaxed`, and the owner's pop stores `bottom` relaxed, so a thief can take a task without synchronizing with its push. The relaxed-publish row in §8 is that gap, and TSan reports it (§5). |
| "Correct memory ordering is part of the scheduler's API contract" | Agreed. The contract (§3) is stated so that task bodies read and write plain fields. |
| stealing costs: cross-core transfers, NUMA, contention among stealers, reclamation, fairness | Cross-core cost measured: idle thieves made the owner's hop nearly 5× slower (§10). Reclamation is avoided by a bounded deque that runs overflow inline (§5). NUMA cannot be measured on one socket. |
| T_P ≥ max(W/P, L); 10,000 × 1 µs with a 2,000-task chain cannot beat 2 ms | Right, and it stops at the half that matters less. The upper bound W/P + L ≈ 2.16 ms is within 8 % of it, and W/L = 5 workers' worth of parallelism (§7). On the fork graph, Brent's bound left 27 µs for priority to win. |
| small tasks cost atomics and queue traffic; at 100 ns a stage, "enqueueing and dequeuing … may cost more than the work itself" | Measured: a deque round trip is 76–93 ns against 14 ns inline, so a 100 ns stage pays about as much again to cross the queue. Fusing 1 µs tasks 8 to 1 takes efficiency from 56 % to 94 % (§6). |
| work-first execution: "fairness and stack-depth concerns" | Inline continuation is a loop, not a call, so it adds no depth. Only a push to a full deque runs its task recursively, so the deque's size bounds the depth (§5). |
| priority scheduling: "better latency for critical tasks", "starvation" | Neither showed here. FIFO stealing already wins most of it, and nothing starved (§7). |
| the follow-up table | Its own numbers do not add up: task bodies are under 3 % of busy CPU in both columns, and utilization falls while steals rise 45× (§10). The amplification reproduces here. The 18 ms does not, on one socket. |
| the extension: explicit terminal states, failure propagated through the graph, "task completion accounting" separate from "successful result publication" | That is §9's design: the counter accounts for completion, and a poison flag released with the same decrement says whether the result is usable. |

What the outline does not mention: that the brief's requirements cannot all describe ready tasks (the
problem table), the ThreadSanitizer false positive on the fence spelling (§8), the cost of *sleeping*
thieves, which here was worse than spinning ones (§10), and that on x86 nothing but a race detector
can tell the brief's code from the fix.

---

## Appendix: Glossary

**Release sequence** — in C++20, a release operation on an atomic and every read-modify-write on the
same atomic after it in modification order, by any thread. An acquire that reads any value in it
synchronizes with the release that heads it. That is why one acquire by the last predecessor
synchronizes with every earlier predecessor.

**Bottom level / rank** — the costliest path from a node to the sink, including the node. The largest
rank in a graph is its critical path.

**Brent's bound** — for a work-conserving schedule on P workers, the time T for a graph of total work
W and critical path CP satisfies max(W/P, CP) ≤ T ≤ W/P + CP.

**Inline continuation** — the predecessor that makes a successor ready runs it directly instead of
pushing it. A "work-first" policy in Cilk's terms.

**Chase–Lev deque** — a work-stealing deque in which the owner pushes and pops at one end without an
RMW, and thieves take from the other end with a CAS.

**Stealing amplification** — steal attempts that grow with the number of idle workers rather than
with the amount of stealable work, each of which taxes a busy victim's cache lines.

**TSO** — total store order, x86's memory model. Every locked RMW is a full barrier there, which is
why the brief's relaxed decrement is the correct instruction on x86.

**LSE / LL/SC** — ARMv8.1's single-instruction atomics (`ldadd` and its ordered forms), and the
load-linked/store-conditional loop (`ldxr`/`stxr`) older AArch64 uses instead.

**Coordinated omission** — measuring from when work started rather than from when it was due, which
hides exactly the stalls a latency requirement is about. Every latency here is measured from the
event's due time.
