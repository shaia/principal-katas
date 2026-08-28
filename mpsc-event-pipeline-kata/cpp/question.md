# The C++ track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

C++ is the language in this kata that gives you every choice and no guard rails. It lets you pick an
ordering per operation, lay out the struct by hand, own the lifetime yourself, and it will compile
all of that whether or not it is correct. Four of the five steps below are questions the other two
tracks cannot ask, because their languages already answered them for you — and the fifth is about
what to do when the tool that would have caught your mistake does not exist on your target.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step C1 — Give every operation its ordering

Write out the ring. Not prose — the actual operations, in order, for both sides:

- the producer loading its own write index, writing the slot, publishing
- the producer checking there is room
- the consumer observing, reading the slots, releasing the space

Now annotate every atomic access with `relaxed`, `acquire`, `release`, or `seq_cst`, and justify each
one individually. "It works" is not a justification; neither is "release/acquire is the safe default".

**Be prepared to say:**

- why the producer's load *of its own index* can be `relaxed` — the argument is one clause long and
  turns on a property of the design, not of atomics
- there are **two** release/acquire pairs here, not one. Name both, and say what the second one
  protects that the first does not
- what a plain `MOV` versus `LDAR`/`STLR` means for the cost of your choice on x86 versus AArch64
- why `volatile` appears nowhere in your answer

<details>
<summary>Hint</summary>

The first pairing is the one everybody writes: the producer's release publishes the slot contents,
the consumer's acquire sees them. If you stopped there, you have made the *consumer's read of a
written slot* safe.

Ask the mirror question. The producer decides whether there is room by reading how far the consumer
has got. What ordering does *that* read need — and what is the consumer's corresponding store? Then
consider what the CPU or the compiler is permitted to do with the consumer's slot reads relative to
its index update, and what the producer does the instant it sees that update.

The failure is a write into a slot that is being read. It is silent, it is load-dependent, and on
x86 you will probably never see it.

</details>

**Done when** every atomic access in your listing has an ordering with a one-sentence reason, and you
can name the two pairs and what each protects.

---

## Step C2 — The one place acquire/release is not enough

Somewhere in your design — if you did step 8 of the main kata — there is a handshake that
acquire/release cannot express, and where x86's memory model gives you no help.

Find it. Say what ordering it needs, why, and what it costs.

**Be prepared to say:**

- which of the four store/load orderings TSO does *not* preserve, and why the wakeup handshake is
  exactly that shape
- why this one is affordable when `seq_cst` on the publish path would not be — the answer is about
  *how often it runs*, not how fast it is
- the optimization of checking the cheap condition first to skip the expensive fence: write out why
  it reintroduces the race, as an interleaving
- what a misplaced fence here looks like in production — how often, how visibly, and how you would
  ever attribute the symptom to this line

<details>
<summary>Hint</summary>

Every synchronization in step C1 was producer→consumer: one side writes, the other reads. That is
what release/acquire is *for*, and it is why the hot path needs nothing stronger.

The wakeup is not that shape. Both participants store and then load, and each needs to see the
other's store before trusting its own load. Two threads, each doing store-then-load of the other's
variable, both allowed to see the stale value — that is the classic Dekker shape, and it is the one
reordering x86 permits.

On cost: locate the operation on a timeline. Is it per event, or per transition into idle?

</details>

**Done when** you can name the ordering, exhibit the lost-wakeup as an interleaving, and say how many
times per second it executes at full rate versus at idle.

---

## Step C3 — Write the struct and count the lines

You did this abstractly at step 5 of the main kata. Now do it in C++, where you control the layout and
therefore own the bug.

Write the declaration. Add your padding. Then, for each 64-byte line, list the fields on it and who
writes them.

**Be prepared to say:**

- what you aligned, and why aligning the two indices is necessary but not sufficient
- `alignas(64)` versus `std::hardware_destructive_interference_size` — the second is the standard
  spelling, and there is a specific reason to prefer the first in a type's layout
- why adjacent producers' metadata must not share a line, given that each producer only ever touches
  its own
- how you would *assert* your layout rather than hope for it — a check that fails the build, not a
  benchmark that looks slower

<details>
<summary>Hint</summary>

Padding the indices is the well-known move and it is not where the surviving bug lives. Go and look
for the fields nobody thinks of as hot:

- a `bool` the producer reads on every push to decide whether it is still accepting
- a flag or counter the consumer touches on **every idle pass** — including a read-modify-write on a
  loop that runs when there is nothing to do
- a mutex used only for parking

If exactly one field in your struct carries `alignas` and the rest were declared after it, ask what
line the *rest* landed on. A consumer spinning on an empty pipeline can take the push-path line
exclusive hundreds of thousands of times a second, and every test still passes.

The reason to prefer `alignas(64)` over the standard constant is not aesthetic — think about what
baking a compile-time constant into a type's layout does to that type's ABI across translation units
built with different flags.

</details>

**Done when** you can name, for every line in the struct, the threads that write it — and you have
found at least one field that is on the wrong one.

---

## Step C4 — Lifetime, by hand

A producer thread exits. Its ring may be mid-drain. Another thread starts and wants a slot.

Design the protocol. Name the states, name who may perform each transition, and give the ordering on
the transition that hands a slot to a new owner.

**Be prepared to say:**

- why the departing producer cannot free its own ring — the argument is about what it is able to
  *know*, not about who allocated
- your state machine, with the specific participant allowed to publish the state that makes a slot
  reusable, and the precondition they must have observed first
- the ordering on the claim, and what it pairs with — a new owner must not see a single byte of the
  previous tenant's ring
- how RAII makes retirement unforgettable, and what a move-only handle buys you over an explicit
  `unregister()` call
- what happens to a slot's *counters* when it is recycled, and what accounting bug that quietly
  introduces

<details>
<summary>Hint</summary>

Ask what each participant can observe. A producer knows it is done. It does not, and cannot, know
whether the consumer is currently inside its ring — there is no state it can read that answers that
question without a protocol, and a protocol is what you are being asked for.

So the producer can only *announce*. Someone else must decide, and there is exactly one participant
that can — the one that would be doing the reading.

Two states are not enough to say both "the owner is gone" and "and nobody is reading it any more."

For the last probe: run the accounting identity from step 6 of the main kata across a slot that has
been recycled, and check whether the totals still add up.

</details>

**Done when** you can draw the state machine with an owner on every transition, and say which single
thread publishes the reusable state and what it must have observed first.

---

## Step C5 — Verify it on a platform where the tool does not exist

The failure modes in steps C1 and C2 are memory-ordering bugs: rare, load-dependent, silent, and
hardware-dependent — x86's TSO hides bugs that AArch64 exposes. A green test suite proves very
little.

ThreadSanitizer is the tool for this, and it is not available on `x86_64-pc-windows-msvc`.

Design your verification anyway.

**Be prepared to say:**

- what a test *can* establish here — name the invariants worth asserting, and note that "no crash
  under load" is not one of them
- what AddressSanitizer covers that TSan would not, and specifically which part of step C4 it
  exercises
- how you would get TSan coverage at all on this target, and what that costs you in fidelity
- how you would demonstrate that a clean sanitizer run is not **vacuous** — a passing tool that is
  not actually watching reports the same green as a correct program
- what you would do about AArch64, given that you cannot run on it

<details>
<summary>Hint</summary>

Split the failure modes by which tool can see them at all:

*Lifetime* bugs — use-after-free on a recycled ring, a leak per departed producer — are memory-safety
bugs, and there is a sanitizer for those that does run here. Make sure your test actually churns
producers rather than running a fixed set at full rate, or the interesting path never executes.

*Ordering* bugs are not memory-safety bugs and that sanitizer will not see them. For these you have
three options and should name all three: a different toolchain on the same machine, a different
platform, and re-reading the code with the model in hand. The one real race in a design like this is
usually found by the third.

And the vacuity check is a habit worth stating: deliberately break the contract, confirm the tool
goes red, then fix it. Otherwise "clean" means only that you ran something.

One trap that costs an afternoon: on Windows, ASan cannot be mixed with the debug CRT, and CMake's
single-config generators default to `Debug`.

</details>

**Done when** you can say, for each of the four failure modes in this track, which tool would catch
it, which would not, and what you do about the ones nothing catches.

---

<details>
<summary><b>Check your work</b></summary>

[**solution.md**](solution.md) — the C++ answer, with the implementation next to it.

| This step | Answered in |
|---|---|
| C1 | §2 Memory ordering — including the second pairing, which the writeup calls the one that gets forgotten |
| C2 | §2 (where `seq_cst` is genuinely required) and §7 (the sleeping/wakeup race) |
| C3 | §3 Cache behavior — and a real shipped false-sharing bug, found long after the tests were green |
| C4 | §4 Ownership — the three-state slot, and the counter bug recycling caused |
| C5 | §10 Verification — what the tests actually caught, and the gap that stayed open until WSL closed it |

Then read what the other two languages do to the same five questions:
[go/question.md](../go/question.md) · [python/question.md](../python/question.md).

</details>
