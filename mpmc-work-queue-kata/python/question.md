# The Python track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

Python is where the central question nearly disappears, and this track is about finding where it
comes back. In one interpreter an event is a reference, and putting a reference into a container is a
single operation that no other thread can observe half-done — so "reserved" and "published" are the
same instant, by construction. The free-threaded build takes away the lock that made that true and
lets you find out which of your other beliefs were leaning on it. And across a process boundary the
two come apart again, by more than in any other track — on another thread, with no bound, and the
standard library documents it.

[Kata 1's Python track](../../mpsc-event-pipeline-kata/python/question.md) already covered where the
interpreter may switch threads, what an uncontended lock costs against a per-event budget, and why
timed waits are coarser than they claim. This one assumes all three.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step P1 — State the ceiling, then name what survives

Before designing anything, find out what this interpreter can do with 32 producer threads and 16
consumer threads.

**Be prepared to say:**

- `queue.Queue` `put` + `get` and `collections.deque` `append` + `popleft`, each measured end to end
  with 32 × 16 threads — on a standard build, and on a free-threaded build (3.13t or 3.14t)
- the throughput you can have, as a fraction of the brief's 5 M/s, and the p99 of one `put` against
  2 µs
- which of steps 0–10 survive the change of scale intact, which become meaningless, and which change
  their answer
- `multiprocessing`: what parallelism it buys, and what it charges per event to cross

<details>
<summary>Hint</summary>

Measure before designing; the number moves the whole track. The correctness questions do not depend
on scale — a lost wakeup at ten thousand events a second is the same bug as at five million. The cost
questions mostly do, and the one that changes most is the price of a lock, which you already know is
not "basically free" here.

So the design that survives is the one whose correctness argument is cheapest to keep true — not the
one that ported most faithfully.

</details>

**Done when** you have measured numbers for both builds and can say which steps of the main kata this
track still has to answer.

---

## Step P2 — Why reserving is publishing here

**Be prepared to say:**

- what `deque.append(event)` does inside the interpreter: when the event object was constructed, what
  is actually stored, and whether any thread can observe the slot reserved but not filled
- the same question on the free-threaded build: what makes `append` atomic there, and at what cost
- build the brief's ring by hand — a preallocated `list` of 65,536 slots and two integer indices — and
  find where it breaks. `self._tail += 1` is the first place to look; Kata 1's step P2 has the
  mechanism
- "`None` means empty", one slot per index: construct the main kata's step-3 lap wrap in Python, with
  real threads, and show the event it loses
- the per-slot sequence from step 3 needs a compare-and-swap. Find one in the standard library

<details>
<summary>Hint</summary>

The element is a reference. Constructing the object happened before `append` was called, in the
producer's own time, and storing the reference is one operation the interpreter does not split. That
is the C++ track's advice — construct before you reserve, and keep the window to one store — taken to
its limit, with a lock you did not write making that store atomic.

The hand-built ring fails because the operations you would need to separate reservation from
publication without a lock are not in the language: there is no atomic integer and no
compare-and-swap. So the per-slot design cannot be written in Python at all, and the honest
conclusion is a lock — which is the main kata's step 10, arrived at from the other side.

</details>

**Done when** you can say why `deque.append` has no reserve→publish window, demonstrate the hand-built
ring losing an event, and name the missing primitive.

---

## Step P3 — Read the monitor that ships

`queue.Queue` is the main kata's step-10 design — a mutex and condition variables around a container —
in the standard library. Read `Lib/queue.py` and check it against the brief.

**Be prepared to say:**

- its lock and its conditions: how many, what each one waits for, and why none of them can lose a
  wakeup
- what `put()` does to notify on every call, and what `Condition.notify` costs with nobody waiting
- `maxsize`: blocking, a timeout, and `put_nowait` raising `Full` — which of the brief's two full
  policies each implements, and how a rejected event is counted (it is not, unless you count it)
- on the free-threaded build, where 48 threads now contend, and whether a batch API would help —
  and whether `queue.Queue` has one
- `SimpleQueue`: what it gives up, what it gains, and which requirement of the brief it cannot meet

<details>
<summary>Hint</summary>

The monitor is correct for the reason the main kata's step 10 gave: the emptiness check, the sleep and
the notify all happen under one lock, so no interleaving can put a check on one side of a notify and
the sleep on the other.

Read `Condition.notify` to see what a notify with an empty waiter list touches. Then look at the cost
structure under free threading: every `put` and every `get` takes the same lock, so the 48-way
contention the C++ track spends three steps on is concentrated in one place — and batching is the only
lever left.

</details>

**Done when** you can say why `queue.Queue` cannot lose a wakeup, what `put` costs when nobody waits,
and which full policy you use and where its drops are counted.

---

## Step P4 — Take the lock away and re-check every claim

The free-threaded build removes the global interpreter lock. Re-run P2 and P3 against it.

**Be prepared to say:**

- what replaced the GIL for built-in containers, and which of P2's claims still hold
- the claims that do not: find one piece of code that is correct with the GIL and races without it,
  using only built-in types
- the scaling of `queue.Queue` from 1 to 48 threads on both builds, measured, and where each stops
- what your own code relied on the GIL for that you did not know about until now — and how you would
  have found out

<details>
<summary>Hint</summary>

The free-threaded build makes individual operations on built-in containers thread-safe with
per-object locks, so a single call stays atomic. What changes is *sequences* of calls. A
check-then-act across two calls was never atomic, but with the GIL and its switch interval the window
was narrow enough that a test rarely hit it. Without the GIL the window is as wide as the OS makes it.

That is the brief's `pop` — check, then take — in Python form, and it is worth writing and watching
fail.

</details>

**Done when** you have one GIL-safe, free-threaded-unsafe example running as a test, and a measured
scaling curve for both builds.

---

## Step P5 — Critique the shipped shutdown

Python 3.13 added `Queue.shutdown()`. It is a reference answer to the main kata's step 8, sitting in
the standard library; check it against the brief.

**Be prepared to say:**

- what `shutdown(immediate=False)` does to `put`, to `get`, and to threads blocked in each — and when
  `get` raises `ShutDown`
- what `shutdown(immediate=True)` does to events already accepted, and which clause of the brief that
  violates
- "accepted" for `queue.Queue`, defined at a line of `Lib/queue.py` — and a demonstration that
  close-versus-put cannot strand an event. Then say what made that easy
- what is still missing for the brief's graceful shutdown: draining with a deadline, counting what was
  refused, telling a producer that was refused because of shutdown apart from one refused because the
  queue was full

<details>
<summary>Hint</summary>

Read the implementation rather than the documentation; it is short. The close-versus-reserve race
from step 8 cannot arise, because the shutdown flag is checked and set under the same lock as the
put. The lock that collapsed "reserved" into "published" also collapses "closed" into "reserved".

That is a cross-track finding worth writing down: one lock answers steps 2, 5 and 8 at once, where
the C++ track needs three separate protocols. Then notice which mode of `shutdown` discards, and what
a supervisor that always calls it that way does to accepted events at every restart.

</details>

**Done when** you can say which shutdown mode meets the brief, at which line an event becomes
accepted, and what you would add.

---

## Step P6 — Cross a process boundary and watch them come apart

`multiprocessing.Queue` is the standard way to get consumers that run in parallel on a GIL build.
Read what its `put()` does.

**Be prepared to say:**

- what `put()` has done by the time it returns: where the object is, whether it has been pickled yet,
  and whether it is in the pipe
- the thread that finishes the job, and what the documentation says about a process that exits with
  items still buffered — and about `cancel_join_thread`
- put an object that cannot be pickled: what `put` returns, where the failure surfaces, what happened
  to the event — and whether the capacity it took was given back
- `empty()` called immediately after `put()` in the same process: what the documentation promises
- map it onto the main kata's step 0: which state is "reserved", which is "published", and how long
  the window between them can be. Then onto step 8: what "graceful shutdown must not lose accepted
  events" requires of a producer *process*
- what you would build to make "accepted" mean "delivered" — acknowledgements, `JoinableQueue`'s
  `task_done`/`join`, something else — and what each costs per event

<details>
<summary>Hint</summary>

`put()` appends to a buffer local to the producing process, and a feeder thread pickles the object
and writes it to the pipe later. So the return from `put()` is a reservation, the pipe write is the
publication, and they happen on different threads with no bound on the time between them.

Every Principal-level concern from steps 6 and 8 comes back at once. The window is unbounded. An
unpicklable object is a reservation that defaults *after* the producer was told it had been accepted.
And a process that exits with a non-empty buffer is a shutdown that loses accepted events, unless
something waits for the feeder.

The standard library documents all of it. The exercise is recognizing it as the same question the C++
track answers with a release store.

</details>

**Done when** you can name the reserve and publish points in `multiprocessing.Queue`, demonstrate the
unpicklable-object loss, and state your shutdown protocol for a producer process.

---

<details>
<summary><b>The shape of the Python answer</b> — read after you have your own.</summary>

The ceiling comes first and it is low: one interpreter does not approach 5 M/s, and the free-threaded
build buys parallelism at the price of making every lock a real one. What survives the change of scale
is the correctness argument, and in Python it is unusually cheap to keep true.

In one process, `queue.Queue` is the answer. It is the main kata's step-10 monitor, and it answers
the central question by making the distinction unobservable — the reference is built before the call
and stored under one lock. Since 3.13 it also has a shutdown whose default mode drains — and whose
other mode discards accepted events, which is the clause of the brief it can violate. The per-slot
design cannot be written here at all: the language has no compare-and-swap.

Across processes the question returns in its worst form. `multiprocessing.Queue.put()` returns before
the object is pickled or written, so "accepted" is a reservation held by a feeder thread. A pickling
failure surfaces on that thread after the caller was told the event was accepted, and an exiting
process can strand what it buffered. Making "accepted" mean "delivered" there takes an
acknowledgement protocol — which is the Principal answer to the brief, arrived at in the one track
where the queue itself was never the hard part.

</details>

Then read what the same questions look like in the other two:
[cpp/question.md](../cpp/question.md) · [go/question.md](../go/question.md).
