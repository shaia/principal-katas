# The C++ track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

C++ is the only language in this kata where a slot's *storage* and the *object* living in it are two
different things with two different lifetimes, so "constructed" and "published" are two facts you
manage by hand rather than one fact the runtime manages for you. It is also the language that lets
you choose, per operation, how much ordering to pay for — and that compiles a wrong choice without
comment.

[Kata 1's C++ track](../../mpsc-event-pipeline-kata/cpp/question.md) already covered the
single-writer orderings, the Dekker fence, cache-line layout, and getting a thread sanitizer at all on
Windows. This one starts where those stop: many writers, a slot reused across laps, an instruction
set that has opinions about your progress guarantee, and waiting primitives that the standard added
in C++20 and did not finish.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step C1 — Annotate the sequenced queue

Write `try_push` and `try_pop` for your step-3 design as code. Then annotate every atomic access with
`relaxed`, `acquire`, `release` or `seq_cst`, and justify each one individually.

**Be prepared to say:**

- the ordering on the reservation CAS, and why the answer is the weakest one available — the argument
  is one clause long, and it is about which location carries the data
- the slot sequence's load and store on each side: there are two release/acquire pairs, as in Kata 1.
  Name what each protects, and say which one is a *consumer of lap k* synchronizing with a *producer
  of lap k + 1*
- the signed difference between sequence and position: write it so that it is defined behaviour in
  C++20 for every pair of values, including after a 32-bit wrap — and say which spelling of it is UB
- `compare_exchange_weak` versus `strong` in this loop, and what the failure ordering argument is for
- the wakeup fence from step 5 now runs on every push. Say what it costs on x86-64 and on AArch64,
  whether you can fold it into an instruction you already execute — and whether doing so is correct
  under the C++ memory model, not just under x86's

<details>
<summary>Hint</summary>

After step 2, the reservation counter protects nothing but itself: no thread reads data *because* it
observed a particular value of that counter. That is the whole argument for its ordering. The slot
sequence is where the data is, so both handoffs go through it — the payload handoff, and the
slot-reuse handoff that runs the other way and gets forgotten, because what must finish first there
is the consumer's *read*, and what must wait is the next producer's *write*.

For the difference: unsigned subtraction is defined modulo 2ⁿ; converting an out-of-range unsigned
value to a signed type has been defined, as modular, since C++20; signed subtraction that overflows
is still undefined. Write the difference using only the defined operations, in that order.

For the fence: a `seq_cst` read-modify-write is a full barrier on x86, which is tempting to rely on.
The C++ model gives you what it gives you on every target. Check that the store-buffering outcome is
forbidden by the *language's* rules for the operations you chose, not merely by the instructions x86
happens to emit for them.

</details>

**Done when** every atomic access has an ordering with a one-sentence reason, both pairs are named,
and your difference is defined behaviour across a wrap.

---

## Step C2 — The object in the slot

The proposed code holds a `std::array<T, N>` and move-assigns into it. Steps 2 and 3 fix its races.
They do not fix what it means for a slot to "contain" an event.

**Be prepared to say:**

- why `std::array<T, N>` plus move-assignment is wrong for this design even with the races fixed —
  two reasons, one about what it requires of `T`, and one about what the sequence can then certify
- raw storage instead: the declaration, `std::construct_at`, `std::destroy_at`, and where
  `std::launder` is required and why
- `push(T)` by value, `push(T&&)`, and `emplace(Args&&...)`: what each one runs *inside* the
  reserve→publish window. `emplace` saves a move, and runs arbitrary user code — a constructor that
  allocates, takes a lock, or throws — while a slot is reserved. Decide
- the `noexcept` requirement: what you constrain `T` with, and what you do about a `T` whose move can
  throw — reject it at compile time, or pay the slot with a tombstone
- the consumer's side: move out, destroy, release. What runs inside the *consumer's* window, and whose
  lap it delays
- the destructor: which slots hold live objects when the queue is destroyed, how you find them, and
  what ordering you need to read their sequences safely

<details>
<summary>Hint</summary>

With move-assignment, every slot holds a live `T` from the queue's construction to its destruction,
so "published" no longer means "there is an object here" — only "the object here has a new value".
That costs a default constructor, 65,536 constructions at startup, a moved-from residue in every
consumed slot (which may still own resources), and an assignment operator that runs with whatever the
slot happened to hold.

Raw storage makes the per-slot sequence certify *lifetime*: a live `T` exists exactly in the
published-but-not-yet-released states. And then every constructor and destructor is code executing
inside a step-6 window. The design question is how much of your user's code you are willing to run
while a slot is reserved, and the conservative answer is "one `noexcept` move". If you allow more,
remember that the `catch` block still owes the slot.

</details>

**Done when** you can write the slot declaration, say what the sequence certifies about the object's
lifetime, and list what runs inside each side's window.

---

## Step C3 — What the instruction guarantees

Step 6 classified your algorithm's progress guarantee. The instruction set has an opinion too, and it
can quietly downgrade yours.

**Be prepared to say:**

- what `fetch_add` and `compare_exchange` compile to on x86-64, and on AArch64 with and without the
  ARMv8.1 Large System Extensions. Whether "`fetch_add` never retries" is true on each — and so
  whether a ticket-based push is wait-free at the instruction level anywhere
- what GCC and Clang emit for AArch64 Linux by default when you do not pass `-march`, and what the
  run-time selection costs per atomic operation
- CAS failure rate: how you measure retries per successful push at 1, 8 and 32 producers, and what
  the tail of that distribution looks like, not only its mean
- your spin loop: the pause instruction, its latency on the specific cores you run on — it changed by
  roughly an order of magnitude between Intel generations, and a hybrid part has two kinds of core —
  and therefore what "spin 100 iterations" means in nanoseconds
- `sizeof(Slot)` for a realistic event, slots per cache line, `alignas` on the slot versus on the
  array, the resulting footprint — and when the ring's pages are first touched, by which thread, on
  which NUMA node

<details>
<summary>Hint</summary>

An LL/SC loop is a CAS loop the hardware wrote for you: on a heavily contended line the
store-conditional can fail and the instruction sequence retries. Single-instruction atomics changed
that on AArch64 — but only in a binary that uses them, and a binary that must run on both kinds of
core decides at run time.

Measure CAS retries with a counter in a test build. It predicts your p99 better than mean throughput
does.

The pause instruction's latency is the unit of every spin-then-park ladder. If it grew tenfold on the
CPU you deploy to, your ladder is ten times longer than you designed — and on a hybrid part it may be
two different lengths depending on where the scheduler put the thread.

For first touch: a ring that has been allocated but never written has no physical pages. The first
lap faults on every one of them, *inside* producers' windows, and places each page on whichever node
the faulting thread happened to be running on.

</details>

**Done when** you can say whether your reservation instruction can retry on each target, give
measured CAS retries per push at 32 producers, and say when and where the ring's pages were first
touched.

---

## Step C4 — Waiting in C++20

Three jobs need a thread to sleep: a consumer on an empty queue, a producer blocking on a full one
with a deadline, and shutdown waking everyone. C++20 offers `std::atomic<T>::wait`/`notify_*`,
`std::counting_semaphore`, and `std::condition_variable_any` with `std::stop_token`; below them sit
the OS primitives — `futex` and `WaitOnAddress`.

Choose one per job, and find out what each choice lacks.

**Be prepared to say:**

- which of these has a timeout and which does not — and what your blocking push with a deadline does
  about the one that does not
- what `atomic::wait` does on a 64-bit atomic on a platform whose native wait is 32-bit, in *your*
  standard library — find it in the source — and what that means for unrelated atomics that share its
  bookkeeping
- the ABA note in the standard's specification of `atomic::wait`, and the variable step 5 told you to
  use because of it
- what `notify_one` costs with nobody waiting, in libstdc++, libc++ and MSVC's STL. Measure it; this
  number is multiplied by 5 million
- a `counting_semaphore` of items as the entire wakeup protocol: what it fixes by construction, and
  what it adds to every operation on both sides
- `std::stop_token` with `condition_variable_any::wait` for shutdown: what it gives you, and the lock
  it costs

<details>
<summary>Hint</summary>

The standard's waiting primitives are not interchangeable, and none of them is complete. One has no
timeout at all. One carries a count, so it cannot lose a wakeup, at the price of another contended
read-modify-write per operation. One is correct by construction because it needs a mutex — and puts
that mutex back on a path you removed it from.

Read your library's implementation of atomic wait before relying on it. The interesting part is what
it does when the atomic's size does not match what the OS can wait on, because the answer involves a
table that every atomic in the process shares.

Then measure the no-waiter notify, because it is the producer's per-push cost from step 5, and it is
an implementation detail rather than a guarantee.

</details>

**Done when** you have one primitive per job with a reason, a measured no-waiter notify cost for each
standard library you ship on, and a blocking push with a deadline that does not depend on a feature
C++20 lacks.

---

## Step C5 — The bug the hardware hides and the tool cannot see

Kata 1's C5 was about getting a thread sanitizer onto the target at all. This one is about what the
tools see once you have them.

Introduce four deliberate bugs, one at a time: the slot-sequence **load** made `relaxed`, the
slot-sequence **store** made `relaxed`, the wakeup **fence removed**, and the **full check removed**.
For each, predict which of four instruments goes red — a stress test on x86-64, the same stress test
on AArch64, ThreadSanitizer, and a model checker. Then run what you have.

**Be prepared to say:**

- your sixteen predictions, written down before anything runs
- what ThreadSanitizer does with a standalone `std::atomic_thread_fence` — build one with
  `-fsanitize=thread` and see what your compiler tells you — and therefore which of the four bugs it
  cannot judge, in either direction
- the model checker you would use — Relacy, CDSChecker, GenMC — what it requires of your code, and
  the configuration that reaches a lap wrap with a slot still reserved
- the preemption injection from step 9 as a build option: exactly where in the code the stall goes,
  and what number comes out
- what a green run on x86-64 entitles you to say about AArch64, in one sentence

<details>
<summary>Hint</summary>

ThreadSanitizer reasons about the C++ happens-before relation, not about the hardware. That is why it
can find a relaxed sequence on x86, where the hardware hides it: the payload access becomes a race in
the *model* even though TSO would have ordered it in practice. It does not model standalone fences,
so a protocol that depends on one lies outside what it can judge.

A model checker covers that gap, exhaustively, for small configurations. A stress test on x86 is the
weakest of the four instruments and should be named as such — with one exception worth predicting
carefully: store-to-load reordering is the one x86 *does* perform.

Fill in the table by running it, and keep your predictions next to the results. The cells where you
were wrong are the finding.

</details>

**Done when** you have the four-by-four table, filled in by running it where you can, with your
predictions recorded beside it and every cell you could not run named as such.

---

<details>
<summary><b>The shape of the C++ answer</b> — read after you have your own.</summary>

The reservation CAS is `relaxed` on both success and failure: it orders nothing, because nothing is
read on account of it. The slot sequence carries both handoffs — a producer's release store of
`pos + 1` pairs with the consumer's acquire load, and the consumer's release store of `pos + N` pairs
with the next lap's producer's acquire load. The difference is `static_cast<std::int64_t>(seq - pos)`
on unsigned operands: subtract first, convert second.

The slots are raw storage, `alignas(T) std::byte[sizeof(T)]` beside the sequence, with `T` required
to be nothrow-move-constructible. That way the sequence certifies that an object exists, the
destructor knows which slots to destroy, and nothing that can throw or block runs inside a window.
`emplace` is the tempting API and the wrong one here.

`fetch_add` is a single `LOCK XADD` on x86 and never retries; on AArch64 it is a single `LDADD` only
where LSE is available, and an LL/SC loop otherwise — so "the ticket design is wait-free" is a
statement about one instruction set. The CAS loop retries everywhere, and its retry tail is the
number that predicts p99.

`atomic::wait` cannot carry a deadline, so the timed blocking paths sit on the OS primitive directly,
behind a generation counter, with the no-sleeper fast path reduced to a fence and a load.

And the table's hard cell is the fence: ThreadSanitizer cannot see it, and an x86 stress test *can* —
because the reordering it forbids is the one x86 really does.

</details>

Then read what the same questions look like in the other two:
[go/question.md](../go/question.md) · [python/question.md](../python/question.md).
