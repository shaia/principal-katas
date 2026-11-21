# Kata 4 — a DAG executor, and the last predecessor that must have seen the others

## The brief

A DAG engine runs tasks whose inputs are the outputs of other tasks. Each task has a counter,
`pending_dependencies`, initialised to its number of predecessors. When a task finishes, it
decrements the counter of each of its successors, and the predecessor whose decrement takes a
counter to zero enqueues that successor to run. Successors read data produced by **all** of their
predecessors.

The engine decrements with `memory_order_relaxed`:

```cpp
void on_finished(Task& t) {
    for (Task* s : t.successors)
        if (s->pending_dependencies.fetch_sub(1, std::memory_order_relaxed) == 1)
            ready_queue.push(s);
}
```

Is relaxed ordering correct? Design the scheduler and its memory-ordering contract.

Underneath it is one question:

> **When the last predecessor finishes, what has it seen?**

The follow-up comes after step 10, and it is a different kind of question: going from 16 to 64 workers
raises throughput but worsens p99 from 4 ms to 18 ms while steal attempts explode.

## About the framing

The brief is quoted, not translated. It is stated in C++ because that is how it was asked, and its
first half is a question about the C++ memory model. Its second half is not: every language that runs
a graph of tasks on a pool of threads has to decide how a ready task is found, by whom, in what order,
and what happens to the rest of the graph when one task fails. The three tracks answer the first half
very differently, because the three languages give very different answers to "can you even write the
brief's bug?". They answer the second half with different machinery and the same arithmetic.

If C++ is not your language, read the brief as:

| the brief | Go | Python |
|---|---|---|
| `std::atomic<int>` with `fetch_sub(…, relaxed)` | `atomic.Int32` with `Add(-1)`, which has no weaker form | `self.pending -= 1` under a lock, or not |
| the predecessor's output, a plain field | a plain field | an attribute holding a reference |
| `ready_queue.push(s)` | a channel send, or `go run(s)` | `executor.submit(run, s)` |
| a worker thread | a goroutine, multiplexed onto `GOMAXPROCS` OS threads | a thread holding the interpreter lock, or a process |

The table is for orientation only. Each track covers what its language charges for these operations,
and which steps it makes disappear.

## How to use this kata

That brief is the whole question — as asked, a 20–25 minute one. The steps below take longer because
"is relaxed correct?" has a short answer and a long one, and the long one is the scheduler.

- **Answer a step before you open its hint.** Out loud, or on paper, in the number of sentences the
  *Done when* line asks for. A hint read first replaces the work it was meant to provoke.
- **Hints reframe, they never conclude.** None of them tells you what to build.
- **The answer sketch is at the bottom, folded.** It is the only place in this file where the shape
  is given away.
- **Do not skip step 0.** It is the shortest, and every later step is an elaboration of it.
- **Language tracks come after step 10.** They assume you have an answer to defend.

---

## Step 0 — Draw the edges

Take the smallest graph that has the problem: a source `S`, two predecessors `A` and `B`, and a join
`J` that reads what both wrote. `A` and `B` run on different workers; `J` runs on a third.

Draw every *happens-before* edge between `A`'s write of its output and `J`'s read of it, under the
brief's code. Then do the same for `B`, supposing `B` is the one whose decrement reaches zero.

**Be prepared to say:**

- which of the two writes is ordered before `J`'s read, and through which operations
- which is not, and why "`B` observed `A`'s decrement" is not the same as "`B` observed `A`'s writes"
- what the counter's atomicity *does* guarantee under `relaxed`, so that you are precise about what is
  wrong: exactly-once readiness, or visibility, or both
- what changes if `J` has exactly one predecessor

<details>
<summary>Hint</summary>

Atomicity and ordering are two different guarantees. An atomic read-modify-write on one counter is
never lost and is totally ordered with the others on that counter, whatever its memory order — so
exactly one predecessor sees the count reach zero. What `relaxed` withholds is any ordering between
that counter and *other* memory locations.

`B` wrote its output, decremented, and enqueued `J` on the same thread: program order covers its own
write, provided the queue does its job. Now look for the operation that would carry `A`'s write
across to `B`. A thread that observes a value written by a relaxed operation has synchronized with
nothing.

</details>

**Done when** you can point at the missing edge, name the two operations that would create it, and
say which of the brief's guarantees survive `relaxed`.

---

## Step 1 — Find it, or fail to

Suppose the brief's engine is in production on x86-64 servers and has run for a year without a
wrong answer.

**Be prepared to say:**

- what the hardware does to a `fetch_sub` on x86-64, whatever its memory order — and what it
  therefore does to the predecessor's earlier stores
- the one remaining way the bug can bite on x86: the code transformation that is permitted, whether
  or not a given compiler performs it
- what the same binary, rebuilt for AArch64, may do — at the predecessor and at the last predecessor
  separately — and why moving to Graviton or Apple silicon is how teams usually find this bug
- why "it is undefined behaviour" is a stronger statement than "it may read a stale value", and what
  the stronger statement licenses a compiler to do
- which instrument would find the bug on the x86 machine you have, and what it reasons about that a
  stress test does not

<details>
<summary>Hint</summary>

On x86-64 every locked read-modify-write is a full barrier, and stores are not reordered with other
stores. The relaxed decrement is the same instruction as the correct one. So on this hardware the
bug does not exist in the machine code. It exists only in the program, under the language's rules.

Weakly ordered hardware separates the two again. A predecessor's store to its output can become
visible after its decrement does, and the last predecessor's read of another's output can be
satisfied before its own decrement completes. Look at what each ordering compiles to on both
targets: the difference is a suffix on one instruction.

A tool that reasons about the C++ memory model, rather than the hardware's, sees the race on every
machine. Find out which constructs that tool does not model, before you trust its silence.

</details>

**Done when** you can say why a year of correct answers on x86 is no evidence, and name the tool
that would have caught the bug on day one.

---

## Step 2 — Write the contract

Fix the decrement. Then state, as rules a reviewer can check, what every party to a dependency must
do.

**Be prepared to say:**

- which decrements must *release*, which must *acquire*, and why the acquire is needed by exactly
  one of them
- how one acquire, by the last predecessor, can synchronize with *every* earlier predecessor's
  release — the C++ term for the chain of read-modify-writes that makes it so, and what C++20 changed
  about it
- three correct spellings: `acq_rel` on every decrement; `release` on every decrement plus an acquire
  fence in the last; `release` plus an acquire *load* of the counter in the last. What each costs, on
  which hardware, and which one a race detector cannot see
- the second hop: the last predecessor enqueues the successor, and a different worker runs it. What
  the ready queue must therefore guarantee, and why transitivity makes the two hops compose
- the dependency that needs no atomic operation at all

<details>
<summary>Hint</summary>

Every predecessor publishes: each wrote an output the successor will read. Only one predecessor
*consumes*: the one that goes on to run the successor or hand it on. That asymmetry is the whole
contract, and the cheapest correct spelling is the one that makes only the consumer pay for it.

Read-modify-writes extend release sequences. A release by `A`, followed in the counter's
modification order by any number of RMWs by anyone, is still observed by an acquire that reads any of
those later values.

Then the queue. "The ready queue correctly transfers publication" is an assumption in the brief's
outline, not a property of queues. A work-stealing deque of pointers is a publication channel, and
its textbook C11 orderings were written for a deque of integers.

</details>

**Done when** you can state the contract in three rules, give the cheapest spelling of each on
x86-64 and on AArch64, and say what the ready queue owes the successor.

---

## Step 3 — The last touch

The predecessor whose decrement reached zero now owns the successor. The others have resolved their
edge. Some of them are still running code.

**Be prepared to say:**

- what a predecessor may still touch after its *last* decrement, and why the answer is "nothing in
  the graph" unless that decrement made something ready
- the interleaving in which a predecessor reads its own successor list after its final decrement,
  and the graph has meanwhile finished and been reused for the next event
- how a graph instance is freed or reused, by whom, and how that thread knows nobody else still
  holds a pointer into it
- what immutable data lets the scheduler read without synchronization, and what must be per run
- what bounded memory means for an executor that accepts new graphs at an arrival rate it does not
  control

<details>
<summary>Hint</summary>

The dependency counter is a reference count with a twist: when it reaches zero the object is not
destroyed but *run*. Every rule about reference counts applies, including the one about touching the
object after you drop your reference.

The sink is the last node of the graph. Its completion is the moment the graph can be reused, so
everything any thread does to the graph must happen before the sink completes. Which threads can be
sure of that?

</details>

**Done when** you can state when a predecessor's last access to a graph must happen, and who may
reuse the graph.

---

## Step 4 — Design the scheduler

Now the rest of the brief: "design the scheduler".

**Be prepared to say:**

- one shared ready queue against one deque per worker: what each costs per task, and where the
  shared queue's contention comes from
- the owner's end and the thief's end of a deque, and why one is LIFO and the other FIFO — in terms of
  the cache and of how much work sits behind each task
- how new graphs enter the system when every worker's deque belongs to its worker
- how an idle worker finds work, and how a sleeping worker is woken without a lost wakeup, and without
  a wake on every push
- what a bounded deque does when it is full
- the one operation of a Chase–Lev deque that needs arbitration between the owner and a thief, and
  how both sides settle it

<details>
<summary>Hint</summary>

Work stealing makes the common case local: a worker pushes and pops its own deque with no
read-modify-write at all, and only an idle worker touches anyone else's. The price is paid when work
is scarce, by the thieves.

The owner takes the newest task because its inputs were just written by this core. A thief takes the
oldest because in a recursive or fan-out graph the oldest task usually has the most work behind it, so
one steal buys the most.

For sleeping, the shape is the store-buffering race again: the worker announces it is about to sleep
and re-checks; the pusher publishes and then checks for sleepers. And "wake someone on every push" is
the expensive version of correct.

</details>

**Done when** you can draw the path of one task from becoming ready to running on another core, and
name every atomic operation on that path.

---

## Step 5 — What a task costs

A task that does 100 ns of work in a scheduler that spends 200 ns on it is a scheduler benchmark.

**Be prepared to say:**

- every operation the scheduler performs per task — the decrements, the push, the pop or steal, the
  wake check — with a cost for each on your machine, measured
- inline continuation: the predecessor that makes a successor ready runs it directly. What it saves,
  and what it gives up
- task fusion: chains fused into one task, and fan-outs grouped. The grain at which overhead is
  negligible, and the grain at which there are fewer tasks than workers
- a join with 4,096 predecessors: what one counter costs when they all finish at once, and the
  alternative
- the edge that needs no read-modify-write, and how much of a typical graph is made of them

<details>
<summary>Hint</summary>

Measure a chain of empty tasks before anything else. Nothing in a chain can be balanced, so the time
per hop is the scheduler's alone. Then measure a fan-out at several grains and plot efficiency: useful
work over worker time.

Fusion and stealing trade against each other. Every task you fuse is one fewer the scheduler handles,
and one fewer a thief can take.

</details>

**Done when** you can give the scheduler's per-task cost in nanoseconds and the grain at which it
falls under 10 % of the work.

---

## Step 6 — The critical path

An event's latency is at least its critical path: the costliest chain from source to sink. A
scheduler that runs the wrong ready task first can stretch it.

**Be prepared to say:**

- how the scheduler knows which ready task is on the critical path — what it computes, when, and for
  which graphs that is impossible
- where priority acts in a work-stealing scheduler, which has no global ordering to act on
- what LIFO does to the critical path when a node makes both a chain step and a wide fan-out ready
- the starvation risk the brief's outline names, and where in your design it would show: which tasks
  wait, and behind what
- how much of the latency at 50 %, 80 % and 95 % load is the scheduler's choice, and how much is
  queueing that no ordering removes

<details>
<summary>Hint</summary>

The bottom level of a node — the costliest path from it to the sink — is computable once for a
static graph. Priority in a work-stealing scheduler is local: it decides which ready successor this
worker keeps and which it pushes, and what sits where thieves look.

Starvation needs a steady supply of higher-priority work. Ask whether your graphs supply it. Then
measure the maximum, not the p99, because starvation is a tail of the tail.

</details>

**Done when** you can say how much your priority rule changes p99 at each load, and whether anything
starved.

---

## Step 7 — Failure and cancellation

A task throws. Another graph's deadline expires. The process is shutting down.

**Be prepared to say:**

- what happens to the failed task's successors, and to tasks that do not depend on it at all
- why every edge must still be resolved exactly once, even for tasks that will never run — and what
  goes wrong if a failed task resolves its edges twice, or not at all
- poison against fail-fast: what each does to the latency of the failure being reported, and to the
  work done after the failure was known
- why cancellation latency is bounded below by the longest task body, and what that says about task
  granularity
- how the failure travels to the successor with the same visibility guarantee as a payload

<details>
<summary>Hint</summary>

A task that is skipped is still a task that finishes. It resolves its out-edges, marking its
successors as poisoned, and the graph reaches its sink exactly as before — which is how you know
nobody still holds a pointer into it. Abandoning the graph instead leaves no point at which it can be
reused.

Cooperative cancellation acts between tasks, never inside one.

</details>

**Done when** you can describe what a failure does to the graph, edge by edge, and give the
cancellation latency in terms of task size.

---

## Step 8 — Prove it, or fail to

**Be prepared to say:**

- the invariants a test can assert per graph: exactly-once execution, every input seen as written,
  exactly-once completion, the failure semantics — and how a node can check that its inputs were
  written, given that a stale read on x86 is never stale
- for each deliberate bug — relaxed decrement, release without acquire, acquire without release, a
  deque that publishes nothing, a failed task that resolves its edges twice — which instrument goes
  red: a stress test on x86-64, the same on AArch64, ThreadSanitizer, a model checker
- the *correct* spelling that one of those instruments reports as a race, and what you do about it
- how you know a clean run is not vacuous

<details>
<summary>Hint</summary>

Sort the bugs by what kind of fact they break. Counting bugs — a decrement too many — break
exactly-once, and any stress test on any machine catches them, usually as a hang. Ordering bugs break
visibility, and on x86 the hardware makes them unobservable. Separate the two before predicting
anything.

A race detector judges the program against the language's rules, so it catches ordering bugs on any
machine — but only for the constructs it models. Look up whether yours models standalone fences.

</details>

**Done when** you have the bug × instrument table, with predictions, and can say which correct
code your best instrument rejects.

---

## Step 9 — Measure it

**Be prepared to say:**

- how you measure event latency so that a stalled scheduler is charged for every event it delayed,
  not only the one it was running — open loop or closed loop, and from when
- how you measure capacity, separately
- what CPU time tells you that throughput does not, when workers are allowed to spin
- what this platform cannot show you: NUMA, a second socket, a core count above the follow-up's —
  and what you claim about each anyway

<details>
<summary>Hint</summary>

A closed loop, in which a new graph is submitted when one completes, measures capacity and hides
latency: when the scheduler stalls, so does the submission. Latency needs arrivals on a schedule, and a
clock that starts when each event was due.

</details>

**Done when** you can say how each number in your answer was produced, and which ones it would be
dishonest to quote.

---

## Step 10 — Argue against yourself

**Be prepared to say:**

- one global FIFO queue under a mutex, against this brief: what it costs per task at your grain, and
  which of steps 2–7 it answers by construction
- one thread per graph — no parallelism inside an event, all of it across events: when that beats
  everything above
- static partitioning: when the graph's shape is known and fixed, why schedule it at run time at all
- what would have to be true of your workload for work stealing to be the wrong call

<details>
<summary>Hint</summary>

Work stealing exists to balance load that is unpredictable and generated during execution. Parallel
work within one event buys latency; parallel work across events buys only throughput, and needs no
stealing if there are always more events than cores.

</details>

**Done when** you can name the workload for which you would ship the global queue, and mean it.

---

## The follow-up

Going from 16 to 64 workers raises throughput but worsens p99 from 4 ms to 18 ms, while steal
attempts explode. Explain stealing amplification, and redesign around task granularity, NUMA,
priority, and worker budgets.

**Be prepared to say:**

- why steal attempts grow faster than workers — count the attempts an idle worker makes per second,
  and the number of idle workers, as a function of the worker count
- what a failed steal costs the *victim*, not only the thief
- the three ways more thieves make p99 worse even as they raise throughput: on the victim's cache
  lines, on the CPUs (if workers exceed cores, or share them with anything), and on locality (a
  stolen task runs far from its inputs — across a socket, on the follow-up's hardware)
- a worker budget: how many workers may *search* at once, and how a searcher that finds work hands
  the role on — and which production runtime already works this way
- what NUMA changes: whom to steal from first, where a graph's memory was allocated, and whether a
  graph should ever cross sockets
- what you would measure first, on the 64-core machine, before changing anything

<details>
<summary>Hint</summary>

An idle thief is a loop. Its cost is not its own CPU; it is every cache line it pulls from a busy
victim, every time slice it takes from a worker with work, and every task it moves away from the
inputs that are still in the victim's cache. Double the workers when there is not twice the work, and
you double the thieves, and the thieves are what got more expensive.

Look at how the Go runtime decides whether an idle P may spin.

</details>

**Done when** you can explain the 4 ms → 18 ms in terms of where the extra time went, and name three
changes with the measurement that would confirm each.

---

## Rubric

This is the rubric the question was asked against.

- **Senior** — understands dependency counts. → steps 0, 3
- **Strong Senior** — identifies the missing publication of predecessor writes. → steps 0, 1, 2
- **Staff** — uses correct synchronization and work-stealing queues. → steps 2, 4, 8
- **Principal** — reasons about critical path, task size, NUMA, priority, and cancellation and failure
  propagation. → steps 5, 6, 7, 9, the follow-up

The asked rubric stops there. This kata adds:

- Strong candidates also say **when not to steal at all.** → step 10

Each level is a more complete answer to the central question. The Senior answer is that the last
predecessor knows it is last. The Strong Senior answer is that knowing you are last is not the same as
having seen the others. The Staff answer is the contract, and the second hop through the queue. The
Principal answer is everything the scheduler does once the contract holds, including the cost of
the threads that are looking for work.

---

## Language tracks

Do steps 0–10 first.

Answers, once you have yours: [cpp/solution.md](cpp/solution.md). The Go and Python answers are not
written yet. The answer outline that came with the question is in
[reference-outline.md](reference-outline.md), and
[cpp §11](cpp/solution.md#11-the-reference-outline-checked) checks it against measurements.

| Track | What it adds |
|---|---|
| [**cpp/question.md**](cpp/question.md) | The only track where the brief's bug can be written. Every ordering, what each compiles to, a Chase–Lev deque that has to publish rather than merely queue, the last-touch rule in raw memory, and a bug × tool table in which a correct spelling goes red. |
| [**go/question.md**](go/question.md) | The track where `relaxed` does not exist, and the scheduler is already written. Go's atomics are sequentially consistent, so the brief's bug cannot be typed. The runtime is a work-stealing scheduler with a `runnext` slot and a cap on spinning threads. The questions are what is left to design, and whether `go f()` per ready node is the answer. |
| [**python/question.md**](python/question.md) | The track where the counter, not the ordering, is the bug. `pending -= 1` is not atomic even with the GIL, so the failure is a successor that runs twice or never. Then the free-threaded build, and a mature DAG scheduler (Dask) whose ordering heuristic you can read. |

---

<details>
<summary><b>Answer sketch</b> — the shape of a strong answer. Read after you have committed at step 2.</summary>

**No, relaxed is not correct.** The relaxed RMW chain guarantees that exactly one predecessor sees the
count reach zero. It does not order any predecessor's *other* writes before anything. The last
predecessor's own output reaches the successor by program order and the queue. Every other
predecessor's output reaches it by nothing, and reading it is a data race: undefined behaviour, not
merely a stale read.

**The contract.**

1. Every decrement is a release: each predecessor publishes its own output.
2. The decrement that reaches zero also acquires. The decrements of one counter form a release
   sequence, so one acquire by the last predecessor synchronizes with every earlier release.
3. Handing the successor to another worker is a second publication, made by the ready queue's
   release/acquire. Happens-before is transitive.
4. After its last decrement a predecessor touches nothing in the graph, unless that decrement made a
   node ready.

`acq_rel` on every decrement is the simplest correct spelling. `release` everywhere, plus an acquire
fence or an acquire load of the counter in the last predecessor only, is the cheapest. On x86-64 all
three — and the brief's relaxed — compile to the same locked instruction, which is why nothing on x86
can see the bug except a race detector. ThreadSanitizer does not model standalone fences, so it reports
the fence spelling, which is correct, as a race. The acquire-load spelling is cheap and visible to it.
An edge into a node with one predecessor needs no RMW at all.

**The scheduler.** One bounded Chase–Lev deque per worker, LIFO for the owner and FIFO for thieves. The
slot is stored with release and loaded with acquire, because a deque of pointers is a publication
channel. A shared injection queue takes new graphs. The predecessor that makes successors ready runs
one of them inline, the one with the largest bottom level, and pushes the rest. Graph instances are
pooled and immutable shapes are shared. Idle workers search under a budget and the rest sleep on an
event count. Failure is a terminal state that still resolves every edge exactly once, propagating a
poison flag, optionally with a fail-fast flag.

**The follow-up.** Stealing amplification: every idle worker is a thief, so with more workers than
parallel work, steal attempts grow with the number of thieves times their rate, and each failed steal
touches a busy victim's cache lines. If workers outnumber cores, or share them, a spinning thief takes
CPU from a worker on the critical path. A worker budget fixes it: a cap on searchers, the rest asleep,
and a searcher that finds work waking one more. The Go runtime's rule is that spinning threads may not
exceed half the busy ones. Add coarser tasks so that there is less to steal, priority that keeps the
critical path on one core, and stealing that tries same-socket victims first.

</details>
