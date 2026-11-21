# The Python track

Extends [../question.md](../question.md). **Do steps 0–10 there first.**

Python has no memory orderings to choose, so the brief's question appears to vanish. What does not
vanish is the counter. `self.pending -= 1` is a load, a subtract and a store, and the interpreter may
switch threads between them, with or without the GIL. The brief's failure mode in Python is not a
stale read. It is a successor that runs twice, or never.

---

## Step P1 — The counter is the bug

**Be prepared to say:**

- the bytecode for `node.pending -= 1` (use `dis`), and the instruction boundaries at which another
  thread can run in between
- the interleaving with two predecessors that leaves `pending` at 1 forever, and the one that has both
  predecessors see zero
- which of steps 0–2 the GIL answers — visibility — and which it does not: atomicity of a
  read-modify-write
- three fixes: a lock per node, one lock for the graph, and `itertools.count` or another operation
  that is atomic under the GIL by implementation rather than by guarantee. Price each

<details>
<summary>Hint</summary>

The GIL makes each bytecode instruction atomic with respect to the others, and makes every write
visible to the next thread that takes it. That is the visibility half of the brief, and it is not the
half that `-=` needs.

</details>

**Done when** you can exhibit the double run and the lost successor, and say which fix you ship.

---

## Step P2 — Lose the GIL

**Be prepared to say:**

- what the free-threaded build (3.13t and later) guarantees about a plain attribute read racing an
  attribute write, and where that is documented, if it is
- whether the per-node lock from P1 is now also the visibility guarantee, and through which
  operations
- what the free-threaded build does to the cost of reference counting a payload that many successors
  read

**Done when** you can say what changed for your executor between the two builds.

---

## Step P3 — Read a real DAG scheduler

Dask's single-machine scheduler executes a task graph on a thread pool, and decides which ready task
to run first.

**Be prepared to say:**

- how `dask.order` ranks tasks, and which of the main kata's step-6 goals it serves — the critical
  path, or memory
- how the scheduler finds ready tasks: a dependency count, or something else
- how it propagates a failure

**Done when** you can compare Dask's ordering with rank priority on the main kata's event graph.

---

## Step P4 — Where parallelism is

**Be prepared to say:**

- a thread pool running CPU-bound bodies under the GIL: the speed-up, measured
- a process pool: what crossing the boundary does to every payload, and the grain at which pickling
  dominates
- `asyncio` and a `TaskGroup` per graph: what it answers (failure, cancellation) and what it cannot
  (parallel bodies)

**Done when** you can give the grain below which no Python executor gains from more workers.

---

## Step P5 — The follow-up, in Python

**Be prepared to say:**

- what adding workers to a GIL-bound pool does to throughput and to p99, and why that is not stealing
  amplification but something that looks like it
- whether the follow-up's redesign has any Python form at all

**Done when** you can say what "more workers" means in each of the three execution models.

---

The Python answer is not written yet.
