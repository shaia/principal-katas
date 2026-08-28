# The Go track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

Go is where this kata stops being a translation exercise. One of the main steps has no choices left
in it; one has an answer the language will not let you spell incorrectly; and two problems appear
that C++ does not have at all — a collector that must be kept away from your buffers, and a clock
that will lie to you in a flattering direction. The most valuable step is the first one, which asks
you to write the *ordinary* Go answer before you write a clever one.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step G1 — Write the boring answer first, then find its wall

Before you port anything: write the design a competent Go engineer would produce in an afternoon,
using only the standard library and no `unsafe`. It should satisfy every clause of the brief.

Then attack it. Say precisely where it stops, and what measurement would tell you that it had.

**Be prepared to say:**

- your ordinary-Go design in one sentence, and which clause of the brief you were most worried about
- what a buffered channel actually *is* in the runtime — name the struct and the field that decides
  its behaviour under contention
- the sharding move, and why it works completely on the side it addresses
- the side it does **not** address, and the specific missing primitive that causes it. This is the
  finding of the whole track and it is not about contention
- the measurable condition — not a vibe — under which you would escalate to a hand-rolled ring

<details>
<summary>Hint</summary>

Go's answer to a contended resource is usually not to hand-roll a lock-free structure. It is to
arrange for nothing to be contended, and the standard library then gets you the rest of the way. Run
that instinct against step 2 of the main kata and you will find the ordinary design almost writes
itself.

Now measure the two halves separately, because they do not behave the same way:

1. **ns per send**, at 1, 8, and 32 producers. Sharded, this should stay roughly flat.
2. **Sustained events/s through the sink** — what the consumer actually got out the other end.

If the first is flat and the second plateaus, contention is not your problem any more and no
rearrangement of the same container will help. Go and read what the consumer must execute to take
*n* items out of a channel. Compare it to what the consumer must execute to take *n* items out of a
ring. That difference — not any property of the send path — is the whole argument for escalating.

</details>

**Done when** you can state the escalation condition as something you could put in a monitoring
alert, and name the primitive whose absence forces it.

---

## Step G2 — The step with no choices in it

Step 4 of the main kata asked you to annotate every atomic operation with its memory ordering. Do
that again in Go.

It will not take long. The interesting question is what that costs you, and what it hands back.

**Be prepared to say:**

- what Go's memory model says about `sync/atomic` operations, quoted — and therefore how many
  orderings you get to pick from
- what your publish store compiles to on amd64 versus what the C++ release store compiles to. Check
  by disassembling; do not assume
- where in the pipeline that cost actually lands: which operation runs once per *event*, and which
  once per *batch*
- step C2 of the [C++ track](../cpp/question.md) is about the one handshake acquire/release cannot
  express, and the tempting optimization that breaks it. What happens to that bug here — and note
  that "it is harder to write" and "it is not expressible" are different claims
- what the same trade does to your portability story

<details>
<summary>Hint</summary>

Two directions, and you should follow both.

*The cost.* The hottest operation in the design happens once per event, and in Go it is a
lock-prefixed instruction where C++ emits a plain move. Build with `-gcflags=-S` and look at the
publish. Then find the operation on the consumer side that pays the same price, and divide by the
batch size before you decide it matters.

*The gift.* Take the wakeup race from step 8 of the main kata and try to construct the bad
interleaving under a memory model where every atomic operation appears in one sequentially
consistent total order. Write the four operations — the producer's publish and its subsequent check,
the consumer's clear and its subsequent emptiness test — assume the bad outcome, and follow the
ordering constraints. Then ask what you would have to write to reintroduce the bug, and what the
race detector says about it.

</details>

**Done when** you can price the publish in nanoseconds against your budget, and either exhibit the
wakeup race or prove it cannot occur — in four operations.

---

## Step G3 — Keep the collector out of your buffers

A problem with no C++ counterpart. You have preallocated *N* × capacity events and they live for the
lifetime of the process. Decide what the garbage collector is allowed to do with them.

**Be prepared to say:**

- what property of your `Event` type determines whether the collector walks your ring storage at all,
  and what that property is called
- the cost of getting it wrong, expressed in the metric this whole design exists to protect — not in
  mark time
- the specific field an obvious translation of the brief adds to `Event` that silently forfeits it,
  and the several ordinary Go types that do the same
- why the ring holds its storage inline rather than as a slice
- how you assert zero allocation on the push path, and the two ordinary constructs in a park loop or
  a stats call that would break it

<details>
<summary>Hint</summary>

Ask what the collector has to do with an array of 4096 elements: does it have to look inside each
one, or can it skip the whole allocation? What, exactly, decides that? The answer is a single yes/no
property of the element type, and it holds only if it holds for *every* field, transitively.

Then re-read your `Event` definition with that question in mind. The brief calls for a timestamp.
Write down the type you reached for and look at its definition in the standard library — count its
fields, and notice that one of them is not a number.

Measure it rather than reasoning about it: force a collection repeatedly at a fixed heap size with a
pointer-free element type and again with one pointer per event, and compare the stop-the-world
pause. Put the result next to your p99.9 target.

Because this property can be destroyed by a future one-line edit from someone who never read your
comment, it wants a test rather than a comment.

</details>

**Done when** you can name the property, name the field that would break it, and state the pause cost
in microseconds against your latency target.

---

## Step G4 — Shutdown without destructors

Step C4 of the C++ track leans on a move-only handle whose destructor makes retirement impossible to
forget. Go has neither destructors nor scope-bound cleanup.

Port the lifetime protocol anyway, and be honest about what weakens.

**Be prepared to say:**

- whether the three-state slot protocol is still necessary here, and — precisely — which of the two
  things it was protecting against in C++ the runtime now handles for you
- what it is *still* protecting, given that answer. The protocol survives; its justification changes
- what forgetting your cleanup call costs, concretely, against a fixed slot budget
- why `runtime.SetFinalizer` / `runtime.AddCleanup` is not the substitute for RAII. Give more than
  one reason, and make one of them about *when* rather than *whether*
- the `go vet` check that catches a whole class of mistake here, and whether `go test` runs it

<details>
<summary>Hint</summary>

Separate the two failure modes the C++ protocol addressed. One was a memory-safety failure: freeing
storage another thread is reading. Ask whether that failure is even *available* to you in Go, given
that the consumer holds a reference. If it is not, say so plainly rather than porting the protocol
with its original justification attached — the stakes changed even though the code did not.

The other failure mode has nothing to do with memory and everything to do with the accounting
identity from step 6 of the main kata. That one ports unchanged.

On finalizers: a destructor is a guarantee about a *scope*; a cleanup is a hint about
*reachability*. Before treating them as interchangeable, work out when a cleanup runs, on which
goroutine, whether it is guaranteed to run before the process exits — and what happens to a cleanup
whose closure captures the very object it is meant to clean up.

</details>

**Done when** you can say which half of the C++ justification survives the port, and what a forgotten
cleanup costs after a thousand producer generations.

---

## Step G5 — Establish the instrument before the result

Step 9 of the main kata asked you to design the A/B before running it. This step is the part that a
port gets wrong silently, in the direction that flatters you.

Before measuring anything: measure your clock.

**Be prepared to say:**

- what `time.Now()`'s monotonic reading is actually derived from on your platform — read the runtime
  source, do not assume it is the obvious counter
- how to measure a clock's *smallest observable tick* in about five lines: what you call, how many
  times, and what you count
- what a p50 latency report looks like when the effect you are measuring is smaller than one tick of
  the instrument, and why the resulting number is worse than a wrong one
- what you would use instead, what has to be true of the hardware for it to be sound, and how you
  would probe for that
- why an open-loop pacer at these rates must spin rather than sleep, and what `runtime.Gosched()`
  would do to your tail

<details>
<summary>Hint</summary>

Do not benchmark the clock's *read cost* — that is the easy number and it is not the one that hurts.
Benchmark how often the value **changes**: call it in a tight loop for a few milliseconds, collect
the results, and count distinct values. A clock can be cheap to read and still advance only about a
thousand times a second.

Now imagine that on a design whose p50 is measured in microseconds. The report is not noisy; it is
clean, stable, and mostly zeros — and it will make your port look *faster* than the C++ answer it was
translated from. A flattering number with no bug attached to it is the worst possible measurement
outcome, because nothing prompts you to look.

The remedy is a phase that prints the instrument before any result depends on it. If you cannot
state your timer's resolution, you do not have a p99.9.

</details>

**Done when** you have printed your clock's smallest observable tick, next to the latency you intend
to report, and the ratio is not embarrassing.

---

## Step G6 — Make "clean" mean something

Go ships a race detector, which is the tool step C5 of the C++ track had to work around. Use it —
and then establish that it is actually watching.

**Be prepared to say:**

- what the race detector can and cannot see: which of your failure modes it detects, and which class
  of bug it is blind to by construction
- what it costs to run, and what has to be installed for it to run on your platform at all
- how you demonstrate the clean run is not **vacuous** — a green result from a tool that never
  observed the interesting path looks identical to a green result from correct code
- what your tests must *do* — not assert — for the lifetime protocol of step G4 to be exercised

<details>
<summary>Hint</summary>

A green race run over a workload that never churns producers, never fills a ring, and never parks
the consumer proves that three code paths you did not execute are fine.

So build the control deliberately: write a variant that violates the single-producer contract on
purpose, put it behind a build tag or an environment variable, and confirm the detector goes red.
Then the clean run on the real code means the detector was on, was watching, and found nothing.

</details>

**Done when** you can show a red run on a deliberate violation and a clean run on the real thing,
from the same command.

---

<details>
<summary><b>Check your work</b></summary>

[**solution.md**](solution.md) — the Go answer.

| This step | Answered in |
|---|---|
| G1 | §1 why sharding and why the obvious Go answer is half right · §1a the escalation · §11 the four designs, measured |
| G2 | §2 Memory ordering: the collapse — the cost, and the bug that becomes unwritable |
| G3 | §3a Garbage collection — the section with no C++ counterpart |
| G4 | §4 Ownership · §"No RAII, and that is a real loss" · §6 Shutdown |
| G5 | §8a Measuring at all — the clock, and why the naive port reports a flattering lie |
| G6 | §10 Verification — the race detector closes the C++'s stated gap |

Then read what the same questions look like in the other two:
[cpp/question.md](../cpp/question.md) · [python/question.md](../python/question.md).

</details>
