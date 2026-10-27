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

## Status

Scaffold. The question and its three tracks are written; the worked answers and the measurement code
are not.

``` text
mpmc-work-queue-kata/
  question.md          the kata — 11 steps, the rubric, the folded answer sketch
  cpp/question.md      C1–C5   orderings, two pairs · the object in the slot · what the instruction guarantees · waiting in C++20 · bug × tool
  go/question.md       G1–G6   the runtime already wrote it · the port without orderings · a sleep without a futex · runtime preemption · close panics · the tail
  python/question.md   P1–P6   the ceiling · reserving is publishing · read Lib/queue.py · lose the GIL · critique Queue.shutdown · multiprocessing's feeder
```

Each track's `solution.md` and implementation are the next piece of work, following
[mpsc-event-pipeline-kata](../mpsc-event-pipeline-kata/) and [plugin-dispatch-kata](../plugin-dispatch-kata/):
a benchmark binary that asserts the invariants under shared labels and exits non-zero if one breaks,
with every number quoted in a solution coming from running it.
