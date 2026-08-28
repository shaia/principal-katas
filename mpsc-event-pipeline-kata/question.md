# Kata 1 — high-throughput in-process event pipeline

## The brief

You are designing a high-throughput in-process event pipeline in C++20. Multiple producer threads
generate small events and push them to a single consumer thread, which batches and writes them to a
socket.

Requirements: very low latency, millions of events per second, minimal allocation, and bounded
memory usage. Producers must not block indefinitely. **Event order only needs to be preserved per
producer, not globally.**

Design the queueing mechanism. Explain whether you would choose a mutex-protected queue, lock-free
MPSC queue, per-producer SPSC queues, or another design. Discuss memory ordering, cache behavior,
backpressure, ownership, and shutdown semantics.

## How to use this kata

That brief is the whole question, and you could answer it in one sitting. The eleven steps below
exist because the answer is not reached by knowing more primitives — it is reached by reading the
specification in a particular order, and each step is one turn of that reading.

- **Answer a step before you open its hint.** Out loud, or on paper, in the number of sentences the
  *Done when* line asks for. A hint read first replaces the work it was meant to provoke.
- **Hints reframe, they never conclude.** None of them tells you what to build.
- **The answer sketch is at the bottom, folded.** It is the only place in this file where the shape
  is given away. Steps 0–10 do not name it.
- **Do not skip step 0.** It is the shortest step and the one the rest is built on.
- **Language tracks come after step 10.** They assume you have an answer to defend.

---

## Step 0 — Read the specification for what it does not ask for

Six requirements are listed above. Five of them ask for something. One asks for *less* than you
might have assumed.

Find it, and write down what it permits that would otherwise be forbidden.

**Be prepared to say:**

- which clause is the giveaway, quoted exactly
- what the design would have to contain if that clause were absent
- why a requirement that *relaxes* something is doing more work here than the five that constrain

<details>
<summary>Hint</summary>

Take the five demanding clauses seriously for a moment and notice that they are close to
contradictory: unbounded speed, bounded memory, no blocking, no allocation. If a specification
looks over-constrained, the resolution is usually not cleverness — it is a permission you have not
spent yet.

Ask of each clause: *is this asking me to provide something, or telling me I need not?*

</details>

**Done when** you can state, in one sentence, the guarantee you are being told you do *not* have to
provide.

---

## Step 1 — Turn each requirement into an exclusion

Requirements read as a list of goals are inert — everything sounds achievable. Read as a list of
*exclusions*, they are a filter, and the filter is narrow.

Take the six clauses one at a time and write, for each, the implementation technique it removes
from the table. Be specific: name the technique, not a quality.

**Be prepared to say:**

- what "millions of events per second" forbids on the push path, in terms of *writes*, not speed
- what "very low latency" forbids about where a producer may end up
- what "minimal allocation" removes, including which standard containers and smart pointers it rules out
- why "bounded memory" and "producers must not block indefinitely" together forbid the most common
  overload policy in production code

<details>
<summary>Hint</summary>

Two of the six interact rather than acting alone. "Bounded memory" on its own is satisfied by a
fixed-size queue that blocks when full; "must not block indefinitely" on its own is satisfied by an
unbounded queue that always accepts. Each is a clean answer to the other's problem. Together they
close both doors, and something else has to give — decide now what, because step 6 will ask you to
name it.

</details>

**Done when** your table has six rows, every "what it rules out" cell names a concrete technique,
and one row's cell is empty or reads *nothing*.

---

## Step 2 — Name the coordination point

Every candidate design has a place where producers meet: an address that more than one producer
thread writes to on the ordinary path. Enumerate the candidates and, for each, point at that place.

- a mutex-protected `std::deque`
- a lock-free MPSC queue with a shared tail (`fetch_add` or CAS)
- a per-producer structure the consumer drains
- a sequencer / ticket dispenser

**Be prepared to say:**

- for each design, the exact object producers contend on — a mutex word, a tail index, a node pointer
- what happens in hardware when 32 cores write to one 64-byte line in a loop, in nanoseconds, not adjectives
- why the instruction count of a push tells you almost nothing about which design is faster
- what the coordination point *buys* you in each case — every one of them is purchasing something

<details>
<summary>Hint</summary>

Do not evaluate these on how much code they need or how many atomic operations they issue. Evaluate
each one by answering a single question: **on every push, which cache lines does a producer write,
and who else writes them?**

Then compare the cost of an L1 hit to the cost of a line that must be taken exclusive from another
core, and notice these numbers differ by roughly two orders of magnitude.

</details>

**Done when** you can name, for each of the four candidates, the one address that limits it — and
say what guarantee that address is paying for.

---

## Step 3 — Commit

Pick one. Write a single sentence naming the structure, who owns which index, and what the consumer
does.

You may pick any of the four from step 2 or something else, but you must commit before continuing —
steps 4 through 7 are questions *about your design*, and they are much less useful asked in the
abstract.

**Be prepared to say:**

- the sentence itself, with no hedging clause in it
- which coordination point you chose to eliminate, and which one you accepted
- what you gave up, stated as a concrete loss rather than "some complexity"
- what would have to be true about the workload for a different choice to win

<details>
<summary>Hint</summary>

Re-read your step 0 sentence and your step 2 table side by side. One of the candidates is
purchasing exactly the guarantee that step 0 told you nobody asked for.

The question offers you four options. Nothing obliges you to accept the framing of the list — the
strongest answers to questions of this shape usually decline one of the offered premises and say
precisely why.

</details>

**Done when** the sentence exists, it fits on one line, and it names an owner for every mutable
index in your design.

---

## Step 4 — Publish and observe

A producer has written an event into a slot. Now it must make that slot visible to the consumer.

Write the two lines the producer executes, and the one line the consumer executes to observe them.
Annotate every atomic operation with its memory ordering, and defend each choice.

**Be prepared to say:**

- what the release/acquire pair guarantees, phrased in terms of what the *consumer sees of the
  payload* — not "it's safe", not "it's ordered"
- why `seq_cst` is not required here, and what it would cost on x86 and on AArch64 (they differ)
- what breaks, concretely, if the store is `relaxed` — describe the observed corruption
- whether your design contains any place at all where `seq_cst` or an explicit fence *is* required

<details>
<summary>Hint</summary>

The pair is not making anything atomic and it is not making anything fast. It is establishing a
*happens-before* edge: it says that everything the producer wrote before the release store is
visible to a consumer that reads the flag with acquire and sees the new value.

Write the failure as a trace of two threads. Which line does the consumer execute, having seen
which value, reading which bytes? If you cannot write the corrupted read as a concrete interleaving,
you do not yet know what the ordering buys.

The last probe is the interesting one and step 8 will come back to it. Publishing is a
producer→consumer edge. Is every synchronization in your design that shape?

</details>

**Done when** you can state the guarantee the acquire/release pair buys in one sentence, without
using the words *faster* or *safer*.

---

## Step 5 — Lay it out in memory

Take your design and write out the struct. Every field, in order, with its size, and a mark
wherever a 64-byte boundary falls.

Now annotate each field with **who writes it, and how often**.

**Be prepared to say:**

- which fields the producer writes on every push, and which the consumer writes on every drain
- what false sharing is, stated as a hardware consequence rather than a definition
- what padding you need and where — and why padding the *obvious* field is not sufficient
- how you would detect this problem in code that already works and passes its tests

<details>
<summary>Hint</summary>

The hot pair is easy to find and easy to pad: the producer's write index and the consumer's read
index. Almost everyone gets that one.

The bug that survives review is elsewhere. Look for a field that is *not* obviously hot — a flag
the consumer touches on every idle pass, a mutex, a counter, a bool read on every push — and check
which line it landed on. A structure can be perfectly padded at the two indices and still have the
consumer's idle loop stealing the producer's line sixty thousand times a second.

Nothing about this shows up in a test. What tells you is either a layout assertion you wrote
deliberately, or a counter.

</details>

**Done when** you can point at a specific 64-byte line in your struct and say which two threads
write it and how often — and, if the answer is "two, both often", you have found the bug.

---

## Step 6 — Decide what happens when it is full

Your buffer has a capacity. Sooner or later a producer arrives at a full one. Step 1 already
established that "wait until there is space" is not available to you.

State the policy. Then say who finds out.

**Be prepared to say:**

- your policy, named: drop the newest, drop the oldest, return failure to the caller, spin briefly
  and then fail, something else
- what the caller is expected to do with a failure — and whether your API makes ignoring it easy
- how `accepted + dropped == offered` is maintained exactly, under concurrency, with no lost counts
- how an operator learns that events are being dropped, before a user does

<details>
<summary>Hint</summary>

There are only two ways a bounded system can respond to sustained overload: it can refuse work, or
it can defer the problem and fail later and worse. "Grow the queue" is the second one wearing a
disguise — it converts a visible, countable, bounded loss into an OOM kill at an unpredictable time.

So the policy is not really the hard part; the hard part is the sentence after it. A design whose
failure mode is silent data loss must not be silent. Decide what you count, where the number is
read, and what a reasonable alert on it looks like.

</details>

**Done when** you can say what a producer does on a full buffer, what the operator sees, and what
the total accounting identity is — in three sentences.

---

## Step 7 — Ownership, churn, and shutdown

Steady state is the easy part. This step is the transitions.

A producer thread starts, pushes for a while, and exits. Another starts later. Meanwhile the
consumer is draining. Then the whole process shuts down.

**Be prepared to say:**

- who allocates a producer's buffer, who owns it, and who is allowed to free it
- how a departing producer knows the consumer is not mid-read of its buffer — noting that it cannot
  determine this about itself
- your shutdown sequence, in order, and what each step is protecting against
- whether shutdown loses events, and if not, what makes the drain provably complete
- what happens to a producer that pushes *during* shutdown

<details>
<summary>Hint</summary>

Two failure shapes live here and they pull in opposite directions.

Free too early and the consumer reads freed memory — the classic one, and the one everybody guards
against. Free too late, or never, and a process that churns producer threads leaks a buffer per
thread, which is the same bug on a longer fuse.

The resolution is that the *departing* thread is the one participant who cannot answer the question
"is anyone still looking at me?" Someone else has to answer it. Decide who, and decide when they get
the chance to.

For the drain: "stop accepting, then drain" and "drain, then stop accepting" are not the same
sequence and only one of them terminates.

</details>

**Done when** you can walk the full lifecycle — register, push, exit, reclaim, shutdown — naming
the owner of every buffer at every moment, and state whether a shutdown can lose an event that was
accepted.

---

## Step 8 — Follow-up: 64 producers, 4–8 active

Assume there are 64 producers, but only 4–8 are active most of the time. Your consumer currently
scans all 64 queues on every iteration.

Design a mechanism that avoids continuously polling empty queues without reintroducing a heavily
contended global synchronization point.

**Be prepared to say:**

- how an active-producer bitmap works, and why 64 producers is a suspiciously convenient number
- the sleeping/wakeup race, written as an interleaving: consumer decides to sleep, producer pushes,
  and the event sits there until the next producer arrives
- notification coalescing — how a producer avoids signalling on every push, and what the cost is of
  the check that decides
- when bits get *cleared*, and why the obvious answer is a performance bug rather than a correctness one
- what this mechanism costs on the push path, in nanoseconds against your push budget

<details>
<summary>Hint</summary>

The requirement reads as self-contradictory: know which of 64 producers are active, without a
contended global synchronization point. It resolves because coherence is asymmetric — a line that
64 cores *read* sits in all 64 caches simultaneously and costs each an L1 hit, while one write to
it invalidates all 64 copies. "Avoid shared state" would have blocked you here. The rule you want
is narrower.

Two traps, both of which look like obvious optimizations and neither of which fails a test:

*The fence.* Skipping the expensive barrier when the cheap check says it is unnecessary
reintroduces precisely the race the barrier existed to close — because the cheap check is itself
one of the two loads the barrier was ordering. Note that this is the one place in the design where
the synchronization is not producer→consumer, and go back to step 4's last probe.

*The clearing.* Clearing a producer's bit the moment its ring drains empty sounds tidy. Count the
writes to the shared line under a producer that pushes at a steady rate into a consumer that keeps
up: the mechanism whose entire purpose is to avoid touching a shared line is now touching it
proportionally to throughput.

</details>

**Done when** you can describe the mechanism, exhibit the wakeup race as a concrete interleaving,
and say what your clearing policy costs in writes per second to the shared line.

---

## Step 9 — Prove it, or fail to

You have just designed an optimization. Now decide whether it works, before you have any data —
design the experiment.

The claim under test: narrowing the scan improves **tail latency**. Not throughput, not mean. Tail.

**Be prepared to say:**

- what exactly you are A/B-ing, and what stays fixed between the arms — including thread counts,
  affinity, sink behaviour, and the compiler seeing the same code
- coordinated omission: why measuring only the events you managed to submit reports a flattering
  number, and what you measure instead
- what your clock actually resolves to on your platform, and how you established that rather than
  assuming it
- what result would make you *abandon* the optimization, stated before you run it
- how you would distinguish a real p99.9 improvement from run-to-run noise

<details>
<summary>Hint</summary>

Two disciplines, both easy to skip.

First, the instrument comes before the result. A timer whose resolution is coarser than the effect
you are measuring will report a beautiful, stable, entirely fictional number. Measure the clock
first — call it in a tight loop for a few milliseconds and count *distinct values*.

Second, decide the falsifying result in advance. The optimization costs something on the push path;
you know roughly what, from step 8. If the tail does not move, the honest report is that on this
hardware it does not move — and that is a finding, not a failure. The follow-up asks whether the
optimization actually improves tail latency. *Whether* is doing real work in that sentence.

</details>

**Done when** you have written down the experiment, the confounders you are holding fixed, and the
number that would make you delete the code — all before running anything.

---

## Step 10 — Argue against yourself

Make the strongest possible case for the design you rejected in step 3. Specifically: build the
case for a mutex and a bounded queue.

Not as a straw man to knock down. As the choice you would actually ship, under conditions you name.

**Be prepared to say:**

- the contention regime where a mutex-protected bounded queue performs comparably — with a reason,
  not a hunch
- what it is much easier to do with it: prove correct, review, debug, extend, hand to a new team member
- how many lines of code and how many subtle protocols each design carries, and who maintains them
- what would have to be true about *your* system for the sophisticated answer to be the wrong
  engineering call

<details>
<summary>Hint</summary>

An uncontended mutex is a CAS. The interesting question is what happens as contention rises, and
the answer depends on a ratio — time holding the lock versus time between acquisitions — that you
can estimate for a push of a 32-byte event.

Then price the other column honestly. Everything in steps 4 through 8 is a protocol that a future
maintainer must hold in their head correctly, in a domain where, as step 8 showed, two separate
obvious optimizations are silent bugs that pass every test.

"Lock-free" is not the goal. It is one means to the goal, and the goal was stated in step 2.

</details>

**Done when** you can name the specific conditions under which you would ship the mutex, and mean it.

---

## Rubric

- **Senior** — compares mutex against lock-free approaches, recognizes allocation and contention
  costs, and proposes bounded queues with sensible backpressure. → steps 2, 3, 6
- **Staff** — discusses cache-line contention, false sharing, producer-local state, batching,
  ownership/lifetime, and why a single shared atomic tail can become a scalability bottleneck.
  → steps 2, 5, 7
- **Principal** — challenges the premise of a single shared MPSC structure and considers one bounded
  SPSC ring per producer, with the consumer polling/draining all rings. Explicitly reasons about
  memory ordering, fairness, overload behavior, NUMA/cache locality, observability, failure modes,
  and operational simplicity. → steps 4, 7, 8, 9
- Strong candidates should also explain **when the theoretically less sophisticated mutex solution
  could still be the better engineering choice.** → step 10

These are not four different answers. They are four depths of one, and the depth is set at step 0.

---

## Language tracks

Steps 0–10 are language-agnostic on purpose — they are about which lines get written by whom, and
that argument is the same everywhere. What is *not* the same is what your language lets you choose,
what it takes away, and what it charges you for. Each track below extends this kata with the steps
its language alone can ask. Do steps 0–10 first; each track assumes you have an answer to defend.

| Track | What it adds |
|---|---|
| [**cpp/question.md**](cpp/question.md) | The language that gives you every choice and no guard rails — pick an ordering per operation, lay out the struct yourself, own the lifetime by hand, and verify it on a platform where the tool you want does not exist. |
| [**go/question.md**](go/question.md) | Write the idiomatic answer first and find out exactly where it stops. One of the steps above has no choices left in Go; a GC arrives that must be kept away from your buffers; and your clock is not what you think it is. |
| [**python/question.md**](python/question.md) | The track that tests whether you re-derive or translate. One premise of the brief is removed outright, the ceiling is low enough that you have to state it before designing anything, and one of the steps above has to be answered from scratch rather than ported. |

Answers, once you have yours: [cpp/solution.md](cpp/solution.md) · [go/solution.md](go/solution.md)
· [python/solution.md](python/solution.md)

---

<details>
<summary><b>Answer sketch</b> — the shape of a strong answer. Read after you have committed at step 3.</summary>

A strong default architecture is:

```
Producer 1 ──> bounded SPSC ring ──┐
Producer 2 ──> bounded SPSC ring ──┤
Producer 3 ──> bounded SPSC ring ──┼──> consumer ──> batch ──> socket
Producer N ──> bounded SPSC ring ──┘
```

Each producer owns exactly one ring and only writes its producer index. The consumer owns the read
index.

For the handoff, publishing an element typically looks conceptually like:

```c
buffer[write_index] = event;
published.store(next_index, std::memory_order_release);
```

The consumer observes publication with:

```c
auto available = published.load(std::memory_order_acquire);
```

The release/acquire pair ensures the consumer sees the event contents before treating the slot as
available. You generally do not need `memory_order_seq_cst`.

Preallocate the rings so the hot path performs no heap allocation. Keep producer and consumer
counters on separate cache lines to reduce false sharing.

When a ring is full, define an explicit overload policy: drop, return failure, overwrite only if
semantically acceptable, or briefly spin before failing. "Wait forever" violates the requirement.

For shutdown, stop accepting new events, signal producers, drain all published elements, then
terminate the consumer. Be careful not to destroy queue storage while producers can still access it.

### Key tradeoffs

Per-producer SPSC rings avoid producer-producer contention and make the memory model relatively
simple, but the consumer must manage many queues and fairness can become an issue.

One MPSC queue simplifies consumer logic but often introduces a shared cache line or atomic
coordination point among producers.

Mutex + bounded queue may perform surprisingly well at moderate contention, is much easier to prove
correct, and should not be dismissed without measurement.

The architectural principle is more important than "lock-free": minimize shared writable state on
the hot path.

</details>
