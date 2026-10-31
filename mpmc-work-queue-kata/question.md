# Kata 3 — a bounded MPMC work queue, and the slot that is reserved but not yet there

## The brief

You are designing a high-throughput C++20 work-dispatch system for a security agent.

The system has 32 producer threads generating events and 16 consumer threads processing them.

Requirements:

- 5 million events/sec sustained throughput.
- Fixed-capacity queue of 65,536 elements.
- No dynamic allocation on the hot path.
- Multiple producers and consumers.
- Producers must never overwrite unread events.
- Consumers should sleep when the queue is empty.
- Producers may block or reject requests when the queue is full.
- Graceful shutdown must not lose accepted events.
- p99 enqueue latency below 2 µs.

A developer proposes:

```cpp
template<typename T, size_t N>
class MPMCQueue {
    std::array<T, N> buffer_;

    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};

public:
    bool push(T value) {
        auto pos = tail_.fetch_add(1);
        buffer_[pos % N] = std::move(value);
        return true;
    }

    bool pop(T& result) {
        if (head_.load() == tail_.load())
            return false;

        auto pos = head_.fetch_add(1);
        result = std::move(buffer_[pos % N]);
        return true;
    }
};
```

Identify the correctness problems and redesign the queue. Explain why atomic indices alone are
insufficient, how to publish initialized elements safely, how to avoid lost wakeups, and what
happens when a producer is preempted after reserving a slot.

Underneath all four of those is one question:

> **How do you distinguish a reserved slot from a fully constructed, published element?**

## About the framing

The brief is quoted, not translated. It is stated in C++ because that is how it was asked, but the
proposed code is short enough to read in any language, and the central question is not about C++.
Any bounded buffer with more than one writer has to answer it — and every language's standard
library has answered it at least once already, in a form you can go and read. Finding that answer
and checking it against the brief is part of each track.

What steps 0–10 are about is **what state a slot is in, which threads can tell, and what happens
while the thread that is supposed to change it does not**. None of that depends on the language.
What the language changes is which of those states you can see at all.

If C++ is not your language, read the brief's code as:

| the brief | Go | Python |
|---|---|---|
| `std::atomic<uint64_t>` with `fetch_add` | `atomic.Uint64` with `Add` | no equivalent in the language — see the track |
| `std::array<T, N>` slot storage | a `[N]T` array held inline | a preallocated `list` of `N` references |
| a consumer that "sleeps" | a parked goroutine | a thread blocked on a lock or a condition |
| a producer "preempted" | descheduled by the runtime, or its thread by the OS | switched out at the interval, or — free-threaded — descheduled by the OS |

The table is orientation only. What each language actually charges for those operations, and which
of the brief's problems it removes, is the subject of its track.

## How to use this kata

That brief is the whole question — as asked, a 20–25 minute one. The eleven steps below take longer
because the bugs in that code are not all the same kind, and fixing them in the order you happen to
notice them produces a patched version of the same design rather than the one the requirements need.
Each step is one layer.

- **Answer a step before you open its hint.** Out loud, or on paper, in the number of sentences the
  *Done when* line asks for. A hint read first replaces the work it was meant to provoke.
- **Hints reframe, they never conclude.** None of them tells you what to build.
- **The answer sketch is at the bottom, folded.** It is the only place in this file where the shape
  is given away. Steps 0–10 do not name it.
- **Do not skip step 0.** It is the shortest step and the one the rest is built on.
- **Language tracks come after step 10.** They assume you have an answer to defend.

---

## Step 0 — Count the states a slot passes through

Follow one slot — index 7 — for one full lap of the ring: from the moment it could accept an event
to the moment it could accept the next one. Write down every state it is in, and which thread moves
it out of each.

Then look at what the proposed class can represent.

**Be prepared to say:**

- the states, in order, with the thread that performs each transition — there are more than two
- which of those states the two counters can tell apart for this one slot, and which they cannot
- why your answer would be different if there were exactly one producer
- the state a consumer must observe before it may read, stated as a fact about the *slot* rather
  than about the counters

<details>
<summary>Hint</summary>

Two counters split the index space into three ranges: below `head_`, between `head_` and `tail_`, and
at or above `tail_`. Map each slot state onto a range, and then ask whether one range can hold slots
in different states at the same moment.

With one producer it cannot: the producer finishes slots in the order it starts them, so every slot
between the counters is in the same state and a single index can describe the boundary. That is why
a ring with one producer needs only one published index. Now count the producers in this brief, and
ask in what order they finish.

</details>

**Done when** you can name, in one sentence, the state this design cannot observe — and the
transition into it that no thread can see.

---

## Step 1 — Break it, one interleaving at a time

Now find every way the proposed code fails. For each, write the schedule: which threads, which
lines, in which order, and what is wrong at the end.

**Be prepared to say:**

- the failure that needs one producer and no consumer at all
- the failure in `pop` that needs two consumers and a single element — and what happens to `head_`
  afterwards, and to the event the *next* producer writes. Follow it for a few more operations; it
  does not stay a single bad read
- the failure that needs one producer, one consumer, and a slot neither of them is confused about
  the index of
- which of these are *undefined behaviour* in C++ and which are merely wrong, and why that
  distinction limits what a passing test entitles you to conclude
- "overflow": there are two quantities in this code that can overflow. Compute when each one does at
  5 M events/sec — and the second figure if someone narrows the counters to 32 bits to fit more in a
  cache line. Then say whether reaching it would actually break anything
- what `std::array<T, N>` commits you to before a single event has arrived

<details>
<summary>Hint</summary>

Sort the failures into kinds before you fix any of them.

Some are about *capacity*: nothing stops a producer lapping the consumers. One is a *check-then-act*:
a consumer tests a condition that another consumer invalidates before the first one acts on it. And
one is about *time*, not indices: the producer announces the position before it fills the slot, so
the announcement and the contents are two events, and the consumer only waits for the first.

The fixes for the first two kinds are more conditions on the counters. Try writing the fix for the
third using only `head_` and `tail_` before you go on to step 2.

For the overflow probe: at 64 bits one of the two figures is tens of thousands of years, and you can
stop worrying about it once you have computed it. At 32 bits it arrives in under a quarter of an
hour, and whether it breaks anything depends on how the comparisons are written — find the form that survives
a wrap, and the property of `N` it relies on. The other overflow is the brief's own word for it:
"never overwrite unread events".

</details>

**Done when** every failure has a concrete interleaving, they are sorted by kind, and you can say
which kind no condition on `head_` and `tail_` can fix.

---

## Step 2 — Separate reservation from publication

The proposed `push` does two things with one operation: it claims a position, and it announces that
the position holds an event. Only one of those is true at the instant it happens.

Split them. Then make the second one carry the payload's visibility with it.

**Be prepared to say:**

- the happens-before edge from the producer's construction of the event to the consumer's first read
  of it: the two operations that form it, by name, on which thread, at which memory location
- why that edge cannot run through `tail_` — and why making every access `seq_cst`, which the
  proposed code already does by default, does not help
- what memory ordering the *reservation* needs, now that it no longer publishes anything
- a second, "published up to here" counter beside the reservation counter. Make it work with 32
  producers that finish out of order: say what a producer that finished early has to do, and what
  that costs it when the producer ahead of it has been descheduled
- why a single-producer ring needs only one index and this design cannot make do with one — in one
  clause

<details>
<summary>Hint</summary>

Release/acquire is a pairing on one memory location: a release store, and an acquire load that reads
the value it wrote. So "where does publication live?" is a question about which location the producer
stores to *after* constructing the event and the consumer loads *before* reading it. `tail_` is
stored before construction. No ordering annotation moves a store to the far side of code that
follows it in program order — ordering constrains what other threads may see, not what this thread
has done yet.

For the second counter: it can only advance past position *p* once every position below *p* is
complete. Producers that finish out of order therefore have a choice — wait for their predecessors,
or record their own completion somewhere that is not a single number. Price the waiting first. Then
take the other branch, which is step 3.

</details>

**Done when** you can draw the edge and name both ends, and say what a "published up to here" counter
costs a producer whose predecessor was descheduled.

---

## Step 3 — Give every slot its own clock

Record completion per slot. Design the value each slot carries, and the comparisons each side makes
against it.

**Be prepared to say:**

- what the per-slot value encodes — not just "full or empty", because step 0 had more than two
  states — and its initial value for slot *i*
- the producer's test for "this slot is free for me", the consumer's test for "this slot is
  published for me", and the two stores that move a slot between them, each written as a comparison
  or an assignment involving the position
- how a producer detects a full queue, and a consumer an empty one, *without reading the other side's
  counter* — and what that does for the cache line the other side's counter lives on
- why a per-slot "null means empty" pointer or boolean is not enough: construct the interleaving in
  which the ring wraps while one slot is still reserved, and say which two situations the flag cannot
  tell apart
- what lives in a slot when no event does. Say whether a slot always holds a live object, and what
  the per-slot value is now certifying about object lifetime
- what a consumer that finds the head slot *not yet published* can and cannot conclude about the rest
  of the queue

<details>
<summary>Hint</summary>

A boolean answers "is there something here?". The question a producer needs answered is "is this
slot free *for the lap I am on*?" — and a producer on this lap and a producer on the next lap can
both arrive at the same slot. The value that tells them apart is one that only ever grows.

Write the slot's value as a function of position and lap, and the four tests fall out as equalities.
Then compute the signed difference between the slot's value and the position each side holds, and
read off what each sign means. One of the signs means "full" on one side and "empty" on the other.

For the last probe: a consumer that sees "not published" at the head cannot distinguish two quite
different situations. Name both, name the one other location that would distinguish them, and keep
the answer — steps 5, 6 and 8 all need it.

</details>

**Done when** you can write the four state tests as one-line comparisons, say which counter each side
reads and which it never touches, and say what the per-slot value guarantees about the payload's
lifetime.

---

## Step 4 — Let the full policy choose the primitive

"Producers may block or reject requests when the queue is full." Those are two different APIs, and
they want different instructions at the moment of reservation.

**Be prepared to say:**

- whether a push that rejects on full can claim its position with an unconditional `fetch_add`, and
  what it has to do instead — and what that costs under 32-way contention on one counter, counted in
  retries
- what an unconditional claim buys a *blocking* push, and what it then forbids it from doing
- 65,536 slots at 5 M events/sec is how many milliseconds of traffic? What, therefore, the capacity
  can absorb and what it cannot — and which of the brief's requirements is really a statement about
  the *consumers* rather than about the queue
- for a security agent specifically: what a rejected event is, who gets told, and what an attacker
  who can generate cheap, benign events does with your reject policy
- what a blocked producer is actually blocking, upstream of itself — and where the events that would
  have arrived during the block go instead

<details>
<summary>Hint</summary>

A claim you cannot take back is a ticket. A blocking push can take a ticket and wait for its slot to
come round. A rejecting push has to look before it claims, and look-then-claim on a shared counter is
a compare-and-swap loop in which every producer that loses the race retries.

Compute the capacity in milliseconds before choosing anything. A queue that holds a few milliseconds
of traffic absorbs bursts, never deficits: if the consumers sustain less than 5 M/s, the queue is
full within that time under any design, and from then on the p99 enqueue latency is set by the
consumers and nothing else.

For the security probes: a dropped event is a detection gap, and an attacker gets to choose when to
cause one. And a producer that reads from a kernel buffer and then blocks does not stop the events
being generated — it moves the drop somewhere you may not be counting it. The Linux audit subsystem
exposes exactly this trade as configuration; look at what its backlog options are, and what each one
does to the process being audited.

</details>

**Done when** you can name the reservation primitive for each policy and what it costs, say how long
the queue lasts once consumers stop, and say where a dropped event is counted.

---

## Step 5 — Sleep without losing a wakeup

Consumers sleep when the queue is empty. With one consumer there is one sleeper to wake. This has
sixteen, and every one of 32 producers has to decide, on every push, whether to wake one of them.

**Be prepared to say:**

- the naive version — *check empty, then sleep* on one side, *push, then notify* on the other — and
  the four-operation interleaving that leaves an event in the queue and every consumer asleep
- the shape of that race — each side stores to one location and then loads the other's, the pattern
  called *store buffering* — and the ordering it needs, which acquire/release does not give; then
  where your step-3 protocol puts the producer's half — before or after the slot's publication store
  — and why the other position is wrong
- what a push costs when nobody is asleep, which at 5 M/s is nearly every push. It must not be a
  system call; say what it is instead
- what a push costs when somebody *is* asleep — the wake, in microseconds, against a 2 µs p99 — and
  how you stop a producer paying it on every push of a burst
- one consumer woken, or all sixteen? If one, who wakes the second when the burst is larger than one
  consumer can drain?
- a consumer that found the head slot reserved but unpublished (step 3): should it sleep, and if so,
  who wakes it?
- what a waiting primitive that compares a value before blocking does when the value changes and
  then changes back — and what your sleep variable must therefore be: a flag, or something else

<details>
<summary>Hint</summary>

The cure always has the same shape. The sleeper announces itself *before* its final check, and the
waker publishes *before* it checks for sleepers: each side stores to one location, then loads the
other's, and the ordering between that store and that load is the one acquire/release does not give
you.

What differs from a single consumer is who gets woken. With one, it is always that one; here a wake
is a choice of how many and which, and the producer's half still runs once per event — so it has to
be cheap when the answer is "nobody is asleep" (one load behind one fence), and the expensive case
has to be rare. A count of sleepers lets the producer skip the wake almost always. A woken consumer
that finds more work than it can handle can do the next wake itself, which caps the producer's cost
at one wake per idle-to-busy transition rather than one per event.

On the last probe: a primitive that sleeps while a value still equals what you last saw can miss a
change that is reverted before it looks. If your variable can return to a previous value, it can lose
a wakeup with no reordering involved at all.

</details>

**Done when** you can exhibit the lost wakeup in four operations, place the fence, and give the
producer's per-push cost in both cases — nobody asleep, and somebody asleep.

---

## Step 6 — Preempt the producer at the worst instruction

A producer reserves position *p* and is descheduled before it publishes. Its timeslice is over; it
will run again in a few milliseconds.

Walk through what everyone else does in the meantime.

**Be prepared to say:**

- what every consumer observes at the head, what the queue *reports*, and what it actually contains.
  How many published events can be stuck behind the hole?
- how long until the producers are affected too, using step 4's capacity in milliseconds — and how
  that compares with a scheduler timeslice on your kernel
- the progress guarantee your design provides, *by definition*: wait-free, lock-free,
  obstruction-free, or blocking. "It has no mutex" is not a definition. Apply the one that asks what
  a suspended thread can prevent everyone else from doing
- what happens if the producer never comes back: its move constructor threw, or the thread was
  cancelled. Say what you owe the sequence on every path out of `push`, the exceptional ones included
- the length of the window between reserve and publish, in instructions — and three things that can
  make it far longer than that count. One of them is the first time the slot's memory is ever touched
- the asymmetry: a consumer preempted between claiming and releasing a slot also leaves a hole. Say
  whom it blocks and when, and why in a nearly-empty queue one kind of hole matters immediately and
  the other almost never does
- what it would take for consumers to get *past* a hole — and what that takes away from the producer
  that left it
- whether anything in the brief requires consumers to take events in FIFO order at all

<details>
<summary>Hint</summary>

A per-slot sequence fixes a safety bug by turning it into a progress bug: the consumer no longer
reads a half-built event, it waits for one. That is the right trade, and it is not free. No lock was
involved, and one suspended thread still stops every consumer — check that against the definition of
lock-free rather than against the absence of `std::mutex`.

A reservation is a debt that the producer must pay on every path, on time. Preemption is paying late;
an exception is defaulting. You cannot prevent the first, so you bound the window. You can prevent
the second, so you make it impossible. Those are two different techniques.

Then put the asymmetry to work. In steady state the queue is nearly empty, so consumers are right
behind producers and a producer's hole is felt at once — while the slot a stalled consumer holds is
not needed again for a full lap.

Lock-free bounded queues exist, and the price is that a reservation stops being a promise: a consumer
must be able to take a slot away from a producer that was too slow, which means a producer's push can
fail *after* it reserved, and must retry.

The last probe is the one to spend time on. The hole stalls consumers *because they must take slots
in order*. With sixteen consumers, ask what that order is still visible to by the time the events
have been processed.

</details>

**Done when** you can name your progress guarantee by its definition, say how long one badly timed
preemption stalls consumers and then producers, and say what every exit path from `push` writes to
the slot.

---

## Step 7 — Price the cache lines

Five million events a second through 48 threads that share a few counters. Count what moves between
cores per event, and put a price on it.

**Be prepared to say:**

- every contended read-modify-write one event costs, end to end, and which cache line each one lands
  on
- 5 M/s as the time between consecutive operations on the reservation counter; the cost of moving one
  cache line between two cores on one socket, and across sockets — measured, not quoted — and the
  headroom that leaves
- the queueing on that line: 32 producers that arrive together each wait for the line in turn.
  Compute the last one's wait against the 2 µs p99
- false sharing between the two counters, and between *adjacent slots*: how many slots share a line
  at your slot size, which threads write each, and what padding every slot to a full line does to a
  65,536-slot ring's footprint against your L2 and L3
- fairness: under a CAS loop, can one producer lose indefinitely? Under tickets, what does strict
  turn order do when the thread whose turn it is has been descheduled?
- what batching a reservation buys — one claim for *k* slots — and what it does to the window from
  step 6

<details>
<summary>Hint</summary>

The event's bytes have to move from the producer's core to the consumer's; that one transfer is the
cost of doing the job. Everything else is coordination, and coordination on one shared line is
serialized by the hardware however the code is written. So the question is how many serialized line
transfers each event pays, and whether 5 M/s times that number fits in a second with room left for a
tail — not whether the code is "lock-free".

Fairness and tolerance of preemption pull against each other. A discipline that hands the next turn
to a *specific* thread is fair right up until that thread is not running, and then everybody waits
for it. That is step 6's observation arriving from the other direction.

A batched reservation divides the contended operations by *k* and multiplies the window by *k*.

</details>

**Done when** you can say how many contended read-modify-writes one event costs, what one costs on
your hardware, and whether 5 M/s fits with headroom for the p99 — as arithmetic, not as an adjective.

---

## Step 8 — Shut it down without losing what you accepted

The process is told to stop. Some producers are mid-push, some are blocked on a full queue, some
consumers are asleep, and the queue holds 40,000 events.

**Be prepared to say:**

- what "accepted" means, precisely — at which instruction in `push` an event becomes the queue's
  responsibility, and on which side of that line a reserved but unpublished slot falls
- how producers learn that the queue is closed, and the race between a producer checking that and a
  producer reserving. Say what happens to a slot reserved after the close took effect
- every sleeper you must wake — on both sides — and what each does once awake
- how a consumer knows it is *finished* rather than momentarily empty. "The queue is empty" is not
  it; step 6 says why
- what the drain waits for if a producer is preempted inside its window at shutdown, and how long you
  are willing to wait — the service manager has a deadline of its own
- what is destroyed when the queue is, and how you know which slots hold live objects

<details>
<summary>Hint</summary>

"Stop accepting, then drain" and "drain, then stop accepting" are not the same sequence, and only the
first terminates: a drain that runs while the queue still accepts can always find one more event.
The new problem is that "stop accepting" is not one instant when the acceptance check and the
reservation are two operations: a producer can pass the check, the queue can close, and the producer
can then reserve.

That slot is a debt like any other. Either the close and the reservation are a single atomic decision
on the same word, or every producer that reserves after the close must still pay its slot — with an
event, or with a marker saying there is none.

The consumers' exit condition follows from that. They are done when they have passed the last
position that was ever reserved and every slot up to it has been paid — which needs a number that is
*final*, and a check of the slots, not of emptiness.

</details>

**Done when** you can define "accepted" at an instruction, show that your close cannot strand a
reserved slot, and state the consumers' termination condition without using the word "empty".

---

## Step 9 — Prove it, or fail to

The failures in steps 1, 5 and 6 are rare, timing-dependent, and some are invisible on x86. A green
run under load proves very little. Design the verification before you write the queue.

**Be prepared to say:**

- the invariants a test can assert at the end of a run — exactly-once delivery, no read before
  publication, and at least one more — and how you check each without the checking itself
  synchronizing the very threads you are trying to race
- which of your failure modes only appear under a weaker memory model than the machine you test on,
  and what you do about them
- what a model checker explores that a stress test does not, and how small a configuration —
  producers, consumers, capacity — still exercises the lap wrap from step 3
- how you *force* step 6 rather than waiting for it: injecting a stall inside the window, and
  measuring what consumers and producers experience
- how you measure the 2 µs p99 when producers can block — coordinated omission: why timing only the
  pushes that ran reports a flattering tail, and what you time instead — and what a blocked push
  contributes to the distribution
- how you know a clean run is not vacuous: the deliberate bug you introduce, per tool, to watch it go
  red

<details>
<summary>Hint</summary>

Separate the failure modes by what can see them at all.

Exactly-once and no-read-before-publication are end-state properties that a test can check, if every
event carries its producer and a per-producer sequence number, and a checksum the consumer verifies.

A missing ordering is not visible to a test on a strongly ordered machine. It is visible to a tool
that reasons about the *language's* memory model rather than the hardware's — and some tools in that
class do not understand every construct you used. Find out which ones before trusting their silence.

A model checker is exhaustive over a tiny configuration, which is exactly what a lap wrap needs:
capacity two, two producers, two consumers, a few operations each.

And a stall you inject is a stall you can measure. Put a delay inside the window behind a build flag
and step 6 stops being an argument and becomes a number.

Time the push that was due, not only the push that ran. A producer blocked for 10 ms records one
10 ms sample and then nothing, though at its share of 5 M/s it owed some 1,560 pushes in that time —
so a timer around each call turns a stall into one outlier and leaves the p99 clean. Give each
producer a schedule and measure from when each push was due: how late the event became is the number
the brief's p99 is about.

</details>

**Done when** for each failure mode you named, you can say which test or tool catches it, which
deliberate bug proves the tool is watching, and which failure nothing catches.

---

## Step 10 — Argue against yourself

Every step above exists because reservation and publication became two events. Make the strongest
case for a design in which nobody can observe the difference.

**Be prepared to say:**

- a mutex-protected ring with two condition variables, against this brief: which of steps 2–8 it
  answers by construction, with a one-line reason for each
- what a preempted lock holder does to everyone, and whether that is better or worse than a preempted
  slot reserver — compare the blast radius, not the mechanism
- what batching does to the mutex's arithmetic: how many events per acquisition reach 5 M/s, and
  what a batch API means for a producer that only ever has one event in hand
- the sharded alternatives — one queue per producer, or one per consumer: what each fixes, and the
  requirement it breaks. Then say what the single shared head was actually providing that the shards
  do not, and what happens under each to every other source when one source floods
- how many protocols a maintainer must hold in their head for each design, and which of them a
  reviewer would notice being broken
- what would have to be true of your system for the per-slot design to be the wrong engineering call

<details>
<summary>Hint</summary>

Under a lock, reservation, construction and publication happen inside one critical section. The
state from step 0 that started all of this still exists — but no other thread can observe it. Most of
steps 2–8 turn into textbook: the monitor pattern does not lose wakeups, and close-then-drain under
the same lock cannot strand a slot.

What it costs is that every operation serializes on one line, and a preempted holder blocks everyone,
not just the consumer behind one slot. Price that against the lock's real hold time *with* a batch,
not against a microbenchmark without one.

For sharding: a pipeline with one consumer can dissolve its shared coordination point — one ring per
producer, scanned by that consumer. Here there are sixteen, and an idle consumer has to find work
wherever the work is. Name what does that in your design, and ask whether it is overhead or the
function the queue exists to perform.

</details>

**Done when** you can name the conditions under which you would ship the mutex, and mean it.

---

## Rubric

This is the rubric the question was asked against.

- **Senior** — identifies overflow, concurrent slot access, and consumers reading unpublished data.
  → steps 0, 1
- **Strong Senior** — separates reservation from publication and uses acquire/release
  synchronization. → step 2
- **Staff** — designs per-slot sequence numbers, bounded capacity handling, and safe
  blocking/wakeup. → steps 3, 4, 5
- **Principal** — analyzes progress guarantees, preemption, cache contention, fairness, shutdown
  races, and tail latency. → steps 6, 7, 8, 9

The asked rubric stops there. This kata adds the line the other two end on:

- Strong candidates also say **when a mutex-protected queue is still the better engineering call.**
  → step 10

Each level is a more complete answer to the central question. The Senior answer is that this code
cannot tell a reserved slot from a published one. The Strong Senior answer is a release store made
after construction. The Staff answer is where that store lives. The Principal answer is about
everything that happens while a slot stays reserved for longer than anyone planned.

These are not four different answers. They are four depths of one, and the depth is set at step 0.

---

## Language tracks

Steps 0–10 are answerable in any of the three, for the reason the framing note gives. What is not
portable is *which states you can see*. C++ shows you the slot's bytes and the object living in them
as two separate things; Go's runtime has already written this queue and hides the states inside it;
and in Python, reserving and publishing are a single operation — until a process boundary pulls them
apart again, further than in either of the others.

Do steps 0–10 first; each track assumes you have an answer to defend.

Answers, once you have yours: [cpp/solution.md](cpp/solution.md). The Go and Python answers are not
written yet. The answer outline that came with the question is in
[reference-outline.md](reference-outline.md), and
[cpp §13](cpp/solution.md#13-the-reference-outline-checked) checks it against measurements.

| Track | What it adds |
|---|---|
| [**cpp/question.md**](cpp/question.md) | The only track where a slot's storage and the object in it have separate lifetimes, so "constructed" and "published" are two facts you manage by hand. Then: an ordering for every operation in the sequenced queue, what `fetch_add` actually guarantees on each instruction set, C++20's waiting primitives and what each of them lacks, and a bug-by-tool table filled in by running it. |
| [**go/question.md**](go/question.md) | The track that starts with the answer already written. A buffered channel *is* a bounded MPMC queue with blocking, rejection and close — so find which steps it answers, and the one requirement it may not meet. Then: the slot protocol ported with its orderings gone, a sleep built without a futex, and a runtime that preempts the producer for you. |
| [**python/question.md**](python/question.md) | The track where the central question nearly disappears, and the work is finding where it comes back. In one interpreter a reference store makes reserving and publishing one act. The free-threaded build removes the lock that made that true. And `multiprocessing.Queue` reintroduces the gap — documented, unbounded, and on another thread. |

---

<details>
<summary><b>Answer sketch</b> — the shape of a strong answer. Read after you have committed at step 3.</summary>

**The diagnosis.** The two counters conflate two events. `tail_.fetch_add` *reserves* a position and,
because consumers compare against it, simultaneously *announces* it. With 32 producers finishing out
of order, no pair of counters can say which positions between `head_` and `tail_` hold finished
events. Everything else — the overwrite, the consumer overtaking, the read of a half-written slot — is
that conflation surfacing in a different interleaving.

**The answer to the central question: put them in different places.** Reservation lives in a shared
counter. Publication lives in the slot, as a per-slot sequence number stamped with the lap:

``` text
slot i, lap k, position p = k·N + i

  seq == p        free for the producer that reserves p
  seq == p, tail > p      reserved — being written       (visible only by combining both)
  seq == p + 1    published: the consumer that claims p may read
  seq == p + 1, head > p  claimed — being read
  seq == p + N    released: free for the producer of position p + N, on the next lap
```

```cpp
// try_push — the reservation is a CAS so that a full queue can be rejected before
// anything is claimed. The reservation publishes nothing, so it can be relaxed.
auto pos = tail_.load(std::memory_order_relaxed);
for (;;) {
    Slot& s  = slots_[pos & (N - 1)];
    auto seq = s.seq.load(std::memory_order_acquire);   // pairs with the consumer's release
    auto dif = static_cast<std::int64_t>(seq - pos);
    if (dif == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
    } else if (dif < 0) {
        return Push::full;                              // the slot still holds the previous lap
    } else {
        pos = tail_.load(std::memory_order_relaxed);    // another producer took it
    }
}
std::construct_at(slot_ptr(pos), std::move(v));          // nothrow: the reservation is a debt
slots_[pos & (N - 1)].seq.store(pos + 1, std::memory_order_release);   // publication
```

The consumer mirrors it: it may read when `seq == pos + 1`, and releases the slot with
`seq = pos + N` — a second release/acquire pair, so that the next lap's producer cannot write while
this lap's consumer is still reading. Neither side reads the other side's counter to detect full or
empty, and the slots hold raw storage, so the sequence also certifies that a live object exists. The
same idea is in shipped code: the Disruptor's multi-producer sequencer keeps a per-slot availability
array that records the lap.

**Wakeups.** An event count — a generation number and a count of sleepers. The consumer registers,
re-checks the head slot, and then waits for the generation to change. The producer publishes,
executes a full fence, and wakes only if the sleeper count is non-zero. Without that fence it is
step 5's store-buffering race, now on every push. The generation is a counter rather than a flag,
because a compare-and-block wait can miss a flag that flips back.

**Preemption, honestly stated.** This design is *blocking* in the progress sense. It has no lock,
yet a producer descheduled between reserve and publish stops every consumer at that slot: the queue
reports empty while holding up to *N − 1* events, and 65,536 slots at 5 M/s is 13 ms — about one
timeslice — before producers start to see it as full. You bound it rather than eliminate it: build the
event before reserving, make the window a `noexcept` move and one store, prefault the ring, and pay
every reservation on every path. Lock-free bounded rings exist (Nikolaev's SCQ is the usual reference),
and they work by letting a consumer revoke a slow producer's slot — so a reservation stops being a
promise. Whether that is worth it is a measurement, not a principle.

**Shutdown.** Make close and reserve one decision: a closed bit in the same word the reservation CAS
operates on, so that no reservation can happen after the close. Wake both sides. Consumers are done
when the head reaches the final tail and every slot below it has been paid — a condition on the
slots, never "empty".

**Contention.** At least two contended read-modify-writes per event — the tail CAS and the head CAS —
plus the slot's own line moving between producer and consumer. The line itself has headroom: one
contended `fetch_add` sustains far more than 5 M/s even at 32 threads. A CAS loop is what spends it,
multiplying the transfers by its retries, so the reject policy's reservation is the one that runs out
first — and a blocking push that takes a ticket instead, never retrying, is the one that keeps the
p99. The 2 µs is also at risk from 32-way arrival bursts on one line, and from consumers that cannot
keep up, which no queue design fixes. (This paragraph originally said 5 M/s "fits on one socket with
headroom"; the C++ answer measured the CAS design at about 5 M/s and no more, on one socket.)

**And the counter-argument that has to be made.** A mutex makes the distinction *unobservable*
rather than representable, and answers steps 2, 5 and 8 by construction. With batching it can reach
the throughput. What it cannot do is keep a preempted thread from blocking everyone. The shared head
that both designs keep is not overhead: with sixteen consumers, it is the load balancer. It is also a
shared fate. The C++ answer later ran the alternative the question's own outline proposes, one ring
per producer: it met the p99 the shared head missed, refused most of a source busier than one
consumer, and left a flood from one source unable to touch the other 31
([cpp §13](cpp/solution.md#13-the-reference-outline-checked)).

</details>
