# The Go track

Extends [the shared kata](../question.md). Do steps 0–10 first. Answer before opening the hint.

## Step G1 — Start with the runtime

Build the conceptual baseline from goroutines, channels, and a coordinator. What remains of the C++ relaxed-counter bug under Go atomics?

<details>
<summary>Hint</summary>

Read the synchronization guarantee, not only the atomicity guarantee.

</details>

**Done when** you can defend a Go-specific happens-before argument.

## Step G2 — Separate goroutines from processors

Compare worker goroutines, OS threads, GOMAXPROCS, SMT, and CPU quotas. When do 64 goroutines oversubscribe execution?

<details>
<summary>Hint</summary>

A parked goroutine need not occupy an OS thread.

</details>

**Done when** you can defend a worker-count experiment that records GOMAXPROCS.

## Step G3 — Price representation

Compare one goroutine per node with a bounded worker pool. Measure closures, allocations, stacks, and GC pressure at one million active nodes.

<details>
<summary>Hint</summary>

Ready tasks and resident goroutines are different budgets.

</details>

**Done when** you can defend an allocation and retained-memory accounting plan.

## Step G4 — Cancel and still drain

Specify context cancellation, panics at worker boundaries, channel ownership, and propagation to skipped children.

<details>
<summary>Hint</summary>

Cancellation does not close a channel for its sender.

</details>

**Done when** you can defend a no-send-on-closed-channel shutdown protocol.

## Step G5 — Avoid fighting work stealing

When does an application-level stealing pool help, and when does it duplicate the runtime scheduler? Compare CPU work and blocking I/O.

<details>
<summary>Hint</summary>

The runtime balances runnable goroutines, not DAG critical paths.

</details>

**Done when** you can defend an experiment that separates application queue delay from runtime scheduling.

## Step G6 — Defend the tail

Define event admission, open-loop pacing, and tracing of a critical chain. How do GC and CPU throttling enter p99?

<details>
<summary>Hint</summary>

A throughput average cannot tell you where one event waited.

</details>

**Done when** you can defend a trace-backed latency argument.

<details>
<summary>Answer direction — read last</summary>

Use the runtime first, then measure the coordination boundary. Go atomics are sequentially consistent; that does not remove lifetime, cancellation, or queueing problems. See [the Go memory model](https://go.dev/ref/mem).

</details>
