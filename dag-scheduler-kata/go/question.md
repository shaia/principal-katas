# The Go track

Extends [../question.md](../question.md). **Do steps 0–10 there first.**

The brief's bug cannot be written in Go. `sync/atomic` has no `relaxed`: the memory model makes every
atomic operation sequentially consistent, and says that an atomic operation which observes another's
effect is *synchronized after* it. So `pending.Add(-1)` already carries every predecessor's writes to
whoever sees the counter reach zero. And the scheduler the brief asks you to design is already in the
runtime: per-P run queues, stealing, a slot for the goroutine to run next, a global queue, and a cap
on spinning threads.

So this track starts by finding out which steps Go answers by construction, and then asks what is left
to design once you are not allowed to write your own scheduler, and whether you should write one
anyway.

---

## Step G1 — The bug you cannot type

**Be prepared to say:**

- what the Go memory model (2022 revision) says about `sync/atomic`, in one sentence, and which of
  steps 0–2 that answers
- what `atomic.Int32.Add` compiles to on amd64 and on arm64, and how that compares with the C++
  track's `acq_rel` and its `relaxed`
- what the brief's code looks like if the counter is a plain `int32` decremented under a
  `sync.Mutex`, and why that is also correct — the edge, by name
- the one way to write the brief's bug in Go anyway: a payload or a counter accessed without an
  atomic operation or a lock, and what `-race` does with it

<details>
<summary>Hint</summary>

Sequentially consistent atomics cost nothing extra for read-modify-writes on either architecture:
there is only one locked add on amd64, and on arm64 the compiler emits the acquire-release form. The
price of the stronger model is paid by atomic *loads and stores*, not by `Add`.

</details>

**Done when** you can say which half of the brief Go removes, and what it costs you.

---

## Step G2 — `go run(s)` per ready node

The most idiomatic executor is no executor at all: the predecessor that reaches zero writes
`go run(s)`.

**Be prepared to say:**

- what the runtime does with a goroutine created by a running goroutine: which queue it goes to, what
  `runnext` is, and how it compares with the C++ track's inline continuation
- what a goroutine costs to create and to schedule, measured, and the task grain below which that
  dominates
- what happens to the critical path when `go` is the only scheduling decision you can make, and
  whether `runnext` helps or hurts it
- a bounded worker pool fed by a channel, against goroutine-per-node: what each answers, and the
  memory bound each gives you

<details>
<summary>Hint</summary>

Read `runqput` and `runqget` in `runtime/proc.go`. A newly readied goroutine goes into the P's
`runnext` slot, displacing the previous one into the local run queue, and inherits the current time
slice. That is inline continuation with a different name, decided by recency rather than by rank.

</details>

**Done when** you can say whether you would write a scheduler in Go for this brief, with a number.

---

## Step G3 — The runtime's answer to the follow-up

**Be prepared to say:**

- the rule in `runtime/proc.go` that decides whether an idle P may start spinning, in terms of busy
  Ps, and what it is for
- how often a P checks the global run queue even when its local queue is not empty, and why that
  number is prime
- how much a stealing P takes from its victim, and why that is not one goroutine
- what `GOMAXPROCS` above the CPU count does, and what a container's CPU quota did to it before Go
  1.25
- the follow-up's 16 → 64 workers, where "workers" are Ps: what changes and what the runtime has
  already done about stealing amplification

<details>
<summary>Hint</summary>

The C++ track had to build a search budget by hand. Go's runtime limits spinning Ms to half the busy
Ps, wakes a new spinner only when the last one finds work, checks the global queue every 61
schedule ticks, and steals half a victim's queue. Compare each with the C++ design and its
measurements.

</details>

**Done when** you can map each part of the follow-up's redesign onto a line of the runtime, or say
which part the runtime does not do.

---

## Step G4 — Failure with `errgroup` and `context`

**Be prepared to say:**

- `errgroup.WithContext` as the failure policy: which of poison and fail-fast it implements, and
  where cancellation is checked
- why a goroutine blocked in a body cannot be cancelled, and what that says about task size, as in the
  main kata's step 7
- exactly-once edge resolution when a node returns an error: is it still needed, given the garbage
  collector?
- what the garbage collector makes unnecessary in step 3, and what it does not

<details>
<summary>Hint</summary>

The GC removes the use-after-reuse bug: no graph instance is reused while anything references it. It
does not remove the need for a completion that knows every node is done, unless you are happy to let
the stragglers run on after the graph has reported.

</details>

**Done when** you can describe your failure policy in terms of `context` and say what it does to a
running body.

---

## Step G5 — Measure it with the runtime's own instruments

**Be prepared to say:**

- the `runtime/metrics` that show scheduling latency and the number of runnable goroutines, and what
  `go tool trace` shows about steals and spinning
- what the race detector sees, and what it does not, in a design with no plain shared payloads
- the garbage collector's contribution to the p99, separated from the scheduler's

**Done when** you can attribute your p99 to the scheduler, the GC, and the work, with a number for
each.

---

## Step G6 — Argue against the runtime

**Be prepared to say:**

- what the runtime cannot know about your graph that rank priority needs, and whether a hand-written
  scheduler above goroutines can recover it
- when a fixed pool of `GOMAXPROCS` goroutines with your own deques beats goroutine-per-node, and
  when it only adds a second scheduler on top of the first

**Done when** you can name the workload for which you would write your own scheduler in Go.

---

The Go answer is not written yet.
