# The Python track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

This is the track that tests whether you re-derive or translate. Python removes one premise of the
brief outright, and a design ported faithfully from the C++ answer will be built to solve a problem
that is not there while ignoring the one that is. One of the main kata's steps has to be answered
from scratch here rather than ported — same mechanism, different reason — and the only honest way to
do it is to commit to a prediction before you measure.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step P1 — Find the premise that is gone, then state the ceiling

Two things before you design anything.

First: one clause of the brief's *implied model* — not its requirements, its model — is false in
CPython. Find it, and say what the C++ design's central argument was buying that you no longer need
to buy.

Second: the brief asks for millions of events per second. Before choosing a structure, establish
whether you can have them.

**Be prepared to say:**

- which assumption the interpreter removes, and therefore what the entire architecture of step 2 of
  the main kata was defending against — that no longer exists here
- an experiment that *demonstrates* it rather than asserting it: what would you measure, and what
  result would confirm the premise is gone
- what the scarce resource is instead. One sentence, and every later decision should follow from it
- the interpreter's actual ceiling in events/s on your machine, measured end to end — producer *and*
  consumer, into the bytes a socket would take
- what you would tell someone whose requirement is genuinely above that ceiling. There is a correct
  engineering answer and it is not "optimize harder"

<details>
<summary>Hint</summary>

For the first part: write down the mechanism the C++ answer exists to eliminate, then ask whether
that mechanism can occur at all when there is one interpreter executing one bytecode at a time. Then
prove it — put N threads through a *single shared* queue, and the same N threads through N
*completely independent* containers, and compare ns/op. If the C++ argument applied here, the gap
would be large. Measure the gap before you design around it.

For the ceiling: benchmark the whole path, not the push. A push benchmark in Python measures the
half of the problem that is not your bottleneck.

And a ceiling is a real deliverable. A design document that says "here is the fastest thing Python
can do, here is your requirement, and here is where the line crosses" is a better answer than one
that quietly aims below the requirement and hopes nobody checks.

</details>

**Done when** you can state, in one sentence, what the only currency is here — and quote a measured
events/s ceiling next to the brief's requirement.

---

## Step P2 — What is atomic when the unit is a bytecode

There are no memory orderings to choose. That does not mean the concurrency questions went away; it
means they moved.

Take one operation in your design that more than one thread touches — a flag, a counter, a bitmap
word — and decide whether it is safe. Then decide whether you can *justify* the answer.

**Be prepared to say:**

- take `self._x |= bit`, which compiles to a load, a modify, and a store. Does it lose updates under
  8 threads? Answer before you test it, then test it
- whatever answer you got: **why**. The reason is a property of the interpreter's implementation, not
  of your reasoning, and you should be able to name the mechanism
- a refactor that any reviewer would approve — extracting a helper, say — that changes the answer
  while leaving the logic identical
- the rule you would give a team, given that the unsafe version passes its tests
- statement order: the annotations are gone, so has ordering stopped mattering? Say what a thread
  switch can do between two Python statements, and compare it to what a CPU can do between two
  instructions

<details>
<summary>Hint</summary>

The received wisdom about read-modify-write in Python is not quite right, in a direction that will
surprise you either way. Do the experiment: eight threads, tens of thousands of iterations each, and
pin `sys.setswitchinterval` as low as it goes so that switches are maximally likely.

Then ask *where* the interpreter is allowed to switch threads. It is not "between any two
bytecodes". There is a specific, short list of instructions at which the eval breaker is checked —
go and find it, and then look at which instructions your operation actually compiles to
(`dis.dis` is three lines).

Now write the version that inserts one of those instructions between the load and the store, without
changing what the code means. That is your refactor, and its result is the whole point of this step.

The conclusion should make you *more* careful than the C++ track, not less: safety that depends on
where CPython happens to place checkpoints is worse than being outright broken, because it is
invisible to review and to testing.

</details>

**Done when** you have both experiments running as tests, and a rule that does not depend on
remembering where the checkpoints are.

---

## Step P3 — Price the primitives, then pick the container

You know from step P1 what the only currency is. So decide by measurement, not by idiom.

Establish your per-event budget. Then price the things you were going to reach for.

**Be prepared to say:**

- what an **uncontended** `threading.Lock` acquire/release costs, measured — and what fraction of
  your per-event budget that single number consumes
- given that, what actually buys you the lock-free path here. It is not cache coherence, and naming
  the real reason is the point of the step
- at least four candidate structures, each measured **end to end** — a producer submitting one event
  *plus* the consumer draining it into wire bytes. Not a push microbenchmark
- what the idiomatic Python engineer would write instead, and how your design compares to it
  honestly, including the cases where theirs wins
- how you get an event into a fixed buffer without allocating a Python object per event

<details>
<summary>Hint</summary>

Measure the lock first, because that one number decides the shape of everything after it. If an
uncontended acquire/release is comparable to your entire per-event budget, then "use a lock, it is
uncontended, it is basically free" — true and good advice in C++ — is not available to you, and the
reason has nothing to do with contention.

For the container: the trap is benchmarking the *append*. Every idiomatic Python container is fast to
put things into and expensive to take *n* things out of, and the consumer is the one participant you
cannot shard. So the benchmark that decides the design must include the drain, in the form the sink
actually needs — bytes.

Which points at what you want from a buffer: a drain that is a single C-level operation over many
events, rather than a loop that runs interpreted bytecode per event.

</details>

**Done when** every structure you considered has a measured end-to-end ns/event next to it, and your
choice is the one the numbers picked.

---

## Step P4 — Wake the consumer, and distrust the timeout

The consumer must sleep when idle rather than burn the interpreter. Design the wakeup handshake, and
then check what your sleep primitive actually does.

**Be prepared to say:**

- the protocol, as an ordering of statements on both sides — producer publishes then signals,
  consumer clears then re-checks. Say what each ordering prevents
- the C++ track needs an explicit fence here (step C2). What replaces it in Python, and why the
  hazard is not gone even though the annotation is
- when flags get cleared, and why clearing after each drain is a performance bug — quantify it in
  flag writes across a run
- what `threading.Condition.wait(timeout)` actually sleeps for on your platform. Measure it at 200 µs,
  1 ms, and 5 ms before answering
- given that measurement, what your idle ladder looks like and what a missed wakeup actually costs

<details>
<summary>Hint</summary>

The handshake is the same shape as every other language's: each side publishes its own state *before*
reading the other's, so no interleaving can leave both parties believing the other will act. What
differs is that there is nothing to annotate — the statement order **is** the mechanism, and a
reviewer reordering two lines for readability breaks it with no atomic in sight to warn them.

On the timeout: do not trust the number you passed in. Time the call. `threading`'s timed waits do
not necessarily use the high-resolution timer that `time.sleep` gained in 3.11, and on Windows the
scheduler tick is ~15.6 ms. If your missed-wakeup backstop is 200 µs on paper and 15.5 ms in
practice, you have designed a defence that is off by two orders of magnitude — and it will only ever
show up in a tail you might blame on something else.

The fix is not a better condvar. It is a ladder with a rung sized to what the platform actually does.

</details>

**Done when** you can state your measured condvar floor, and your idle ladder has a rung that exists
*because* of it.

---

## Step P5 — The follow-up, again — and predict before you measure

Redo step 8 of the main kata here: 64 producers, 4–8 active, stop scanning the idle ones.

Before you run anything, **write down your prediction** — will narrowing the scan move p50, p99, and
p99.9 here, and by how much? Commit to it in writing. Then measure.

**Be prepared to say:**

- your prediction, and the reasoning behind it, recorded before the run
- what one probe of an idle producer actually costs in Python — enumerate the operations, and note
  that not one of them is a memory access
- the sweep you would run: fix the active producers and their rate, vary the *registered* count, and
  report all three percentiles plus probes-per-pass and flag-writes
- what the result was, whether your prediction survived it, and if not, exactly which step of your
  reasoning was wrong
- the C++ answer states the condition under which this optimization would pay. Look it up, and say
  whether that condition holds here — the mechanism ports even where the justification does not

<details>
<summary>Hint</summary>

The reasoning trap is specific and worth walking into deliberately: it is easy to estimate the
*effect size* in nanoseconds, compare it to the noise floor of your instrument, and conclude the
effect will vanish. That reasoning omits a term — the saving is not a fixed number of nanoseconds.
What does it scale with?

So work out the cost of one probe first, multiply by the number of idle producers you are skipping,
and only then compare it to the percentile you care about. Whether that product is large or
negligible is exactly what you are predicting.

Whatever comes out, report it. If your prediction was wrong, leave it in the writeup with the
reasoning error exposed — that is more instructive than a document that was right about everything.

And note that the *justification* does not port even if the mechanism does. The C++ argument is
about cache coherence and read-mostly shared lines. Ask whether "read-mostly" is even a category
Python offers, given what happens to an object's header when you merely read an attribute.

</details>

**Done when** you have a table with all three percentiles across registered counts, and a sentence
saying whether your prediction survived.

---

## Step P6 — And if the producers are not threads

Most things emitting telemetry at volume in Python are not threads.

Redo the design for coroutines on one event loop, and say how much of the last five steps survives.

**Be prepared to say:**

- which of steps P2, P4, and P5 stop applying entirely, and the one property of the event loop that
  makes all three disappear at once
- what backpressure looks like when you can `await` it — and why that is a genuinely better answer
  to the brief's "producers must not block indefinitely" than dropping is
- what you still have to get right: the parts of the brief that a single-threaded event loop does
  *not* solve for you
- when this is the wrong answer, and you are back to threads

<details>
<summary>Hint</summary>

Ask what preempts what. If the answer is "nothing, between await points", then every hazard in P2
(a switch landing mid-sequence), every hazard in P4 (a wakeup racing a park), and most of the
machinery in P5 evaporate — not because you solved them but because the conditions for them do not
arise.

What does *not* evaporate is everything from step 6 of the main kata: bounded memory, a terminating
overload policy, accounting that adds up, and a consumer whose per-event work sets the ceiling. Those
were never about concurrency.

The interesting upgrade is backpressure. Dropping is what you do when you cannot make the producer
wait. Here you can make it wait *without blocking anything* — which is a strictly better answer to
the requirement, and worth saying out loud since the brief was written for a language where it was
not available.

</details>

**Done when** you can say which hazards are gone by construction, and which parts of the brief you
still have to satisfy by hand.

---

<details>
<summary><b>Check your work</b></summary>

[**solution.md**](solution.md) — the Python answer.

| This step | Answered in |
|---|---|
| P1 | §1 What Python changes about the question · §10 When not to use any of this |
| P2 | §3 What "atomic" means here, and why the obvious answer is wrong |
| P3 | §2 Why a `bytearray` ring, decided by measurement · §4 against what a Python engineer would write |
| P4 | §5 Waking the consumer · §"The 15.5 ms condvar, and the rung that exists because of it" |
| P5 | §6 Narrowing the scan — and here the answer inverts. The writeup's own prediction was wrong and is left in |
| P6 | §11 The asyncio answer |

Also §7, the GIL switch interval — the replacement for the C++ cache-behaviour section, and a larger
effect than the one it replaces.

Then read what the same questions look like in the other two:
[cpp/question.md](../cpp/question.md) · [go/question.md](../go/question.md).

</details>
