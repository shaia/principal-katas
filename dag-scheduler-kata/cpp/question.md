# The C++ track

Extends [../question.md](../question.md). **Do steps 0–10 there first.** This track asks about *your*
design, and every step below assumes you have one you can defend.

C++ is the only language in this kata in which the brief's bug can be written at all. It lets you pick
an ordering per operation, compiles a wrong choice without comment, and on x86-64 emits the same
instruction for the right choice and the wrong one. It also lets a task's storage outlive its use, so
"who may still touch this graph" is a question you answer by hand.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step C1 — Annotate the executor

Write the executor's atomic operations as code: the dependency decrement, the poison flag, the
fail-fast flag, the deque's `top`, `bottom` and slots, and the sleep protocol's counters. Annotate
every one with an ordering, and justify each individually.

**Be prepared to say:**

- the decrement, in the spelling you would ship, and the two alternatives from step 2 — with what each
  makes the *last* predecessor pay, and what it makes every other predecessor pay
- the poison flag: a predecessor sets it before its decrement and the last predecessor reads it after
  its own. What ordering the flag itself needs, given the decrement's
- what GCC's code generator does with `fetch_sub(1, order)` when `order` is a variable rather than a
  constant — find it in the source — and why that matters for a test harness that selects the
  ordering at run time
- the deque: which of its operations need `seq_cst`, and what reordering each one forbids; which
  need only release or acquire; and which would be correct with `relaxed` if the deque held integers,
  but not when it holds pointers to tasks
- a `static_assert` or a type that makes the wrong decrement hard to write

<details>
<summary>Hint</summary>

The flag rides on the decrement. A plain store before a release, read after an acquire that
synchronizes with it, is already ordered, so the flag can be as weak as atomicity allows. The same
argument fails for the brief's relaxed decrement, and for the same reason the payload does.

GCC's expander turns a memory-order argument that is not a compile-time constant into
`MEMMODEL_SEQ_CST` (`get_memmodel` in `gcc/builtins.cc`) rather than test it at run time. So a
harness that passes the ordering as a variable, to compare "relaxed" against "acq_rel" on AArch64,
is comparing seq_cst with seq_cst. It will report that the bug costs nothing and that nothing can
find it.

The textbook C11 Chase–Lev (Lê et al., PPoPP 2013) proves its orderings for a deque of `int`. The
thief reads the slot `relaxed`, which is enough to get the integer. It is not enough to see what the
owner wrote into the object a pointer points at.

</details>

**Done when** every atomic operation has an ordering with a one-sentence reason, and you can say which
of them a deque of integers would not need.

---

## Step C2 — Read the machine code

**Be prepared to say:**

- what the brief's `fetch_sub(1, relaxed)` and the correct `fetch_sub(1, acq_rel)` compile to on
  x86-64, on AArch64 ARMv8.0, and on AArch64 with the Large System Extensions — all six, read from your
  compiler's output, not recalled
- where the release+fence spelling puts its barrier on AArch64, and whether it runs on every decrement
  or only the last
- what the acquire load in the release+load spelling costs on x86-64
- whether, at `-O2`, your compiler moved or deleted a plain store across a relaxed RMW in your probe.
  What that tells you about the next compiler release
- what MSVC's standard library does with the memory-order argument on x86-64

<details>
<summary>Hint</summary>

On x86-64 a locked RMW is a full barrier. There is no instruction for "a weaker `lock xadd`", so all
orderings compile to the same `lock` prefix, and every spelling of the contract is free.

On AArch64 the ordering is a suffix on the instruction: `ldadd`, `ldaddl`, `ldadda`, `ldaddal`, or
`ldxr`/`ldaxr` with `stxr`/`stlxr` without LSE. A standalone acquire fence is `dmb ishld`, and an
acquire load is `ldar`.

A compiler that does not reorder today has not promised anything. The program is undefined, and an
optimizer that learns a new trick is allowed to use it.

</details>

**Done when** you have the six instruction sequences side by side and can say which differences a
test on your machine could ever observe.

---

## Step C3 — The deque, in C++20

**Be prepared to say:**

- bounded or growable: what growth costs (allocation on the busiest path, and a retired buffer a
  thief may still be reading — so a reclamation scheme) and what bounded costs (a policy for full)
- the overflow policy: run the task inline. What that does to stack depth, and what bounds it
- the last-element race between `pop` and `steal`, written out with both CASes
- the C11 version's standalone fences against `seq_cst` operations on the same locations: what each
  costs on x86-64, and which one a race detector can see
- `alignas` for `top` and `bottom`, and why they must not share a line
- `sizeof` your per-node state, whether two nodes share a line, and which threads write each field

<details>
<summary>Hint</summary>

The owner pushes to and pops from `bottom` on every task. Thieves CAS `top`. On one line, every push
invalidates every thief's copy of `top`, and every steal invalidates the owner's `bottom`.

The pop's store to `bottom` followed by a load of `top` is the store-buffering shape. On x86 that needs
a full barrier however you spell it, and a `seq_cst` store is an `xchg`, which is one.

</details>

**Done when** you can write `push`, `pop` and `steal` with orderings, and say what a full deque does.

---

## Step C4 — Lifetime by hand

**Be prepared to say:**

- the graph split into what is immutable and shared (edges, costs, ranks) and what is per run
  (counters, flags, payloads), and which of the two a predecessor reads after its last decrement
- `std::shared_ptr` per task as the lifetime mechanism: what it adds per edge, and what it fixes
- pooling instances: who resets an instance, with what kind of stores, and what publishes the reset to
  the next run's workers
- the stack frame of a task running inline: what it holds pointers to, and for how long
- per-node state padded to a cache line against packed: the false sharing you remove, and the
  footprint for a 4,096-node graph

<details>
<summary>Hint</summary>

A predecessor that iterates its successor list after its last decrement is safe only if that list is
not part of the run. Put edges in an immutable shape that outlives every run of it, and the loop over
successors never touches per-run memory except through the counters it is decrementing.

The thread that completes the sink is the only thread that holds the instance at that moment. That
is when it can be reset, with plain stores, and the injection queue's lock publishes the reset.

</details>

**Done when** you can name, for every field a task touches, whether it is shared or per run, and the
last instant any thread may touch it.

---

## Step C5 — The bug × tool table

ThreadSanitizer does not exist for `x86_64-pc-windows-msvc`. Getting it at all is the first job on
Windows.

Introduce five deliberate bugs, one at a time: the **relaxed decrement**, **release without acquire**,
**acquire without release**, a deque whose push **publishes nothing**, and a failed task that
**resolves its edges twice**. Add one row that is not a bug: the **release+fence** spelling. For each,
predict which of four instruments goes red — a stress test on x86-64, the same stress test on AArch64,
ThreadSanitizer, and a model checker. Then run what you have.

**Be prepared to say:**

- your twenty-four predictions, written down before anything runs
- how the stress test checks that an input was written, when a stale read on x86 never happens
- which bugs are counting bugs and which are ordering bugs, and which instruments can see each kind
- what ThreadSanitizer does with `std::atomic_thread_fence`, what your compiler says about it when you
  build with `-fsanitize=thread`, and what you ship given the answer
- what a green run under gcc on Linux tells you about the clang-on-Windows binary you ship, and what it
  does not

<details>
<summary>Hint</summary>

Counting bugs show up everywhere, usually as a hang, because a successor whose counter was
decremented once too often either runs early or never runs. Ordering bugs show up nowhere on x86 and
everywhere under a tool that judges the program against the C++ model.

ThreadSanitizer does not model standalone fences. A correct protocol built on one is reported as a
race. A protocol you want the tool to vouch for has to put its acquire on an atomic *operation*.

</details>

**Done when** you have the six-by-four table, filled in by running it where you can, with your
predictions recorded beside it, and every cell you could not run named as such.

---

<details>
<summary><b>The shape of the C++ answer</b> — read after you have your own.</summary>

The decrement is `fetch_sub(1, release)`, and the predecessor that reaches zero follows it with
`load(acquire)` of the same counter. That load reads the value its own RMW wrote, which lies in the
release sequence of every earlier predecessor's decrement. On x86-64 the load is a plain `mov`; on
AArch64 it is one `ldar`, executed only by the last predecessor; and ThreadSanitizer can see it,
which it cannot do for the `dmb ishld` of the fence spelling. `acq_rel` is just as correct and costs
nothing more on x86. A node with one predecessor gets no RMW at all.

On x86-64 the brief's relaxed decrement is the same `lock dec` as every correct spelling, and the
compiler did not move the payload store across it. A stress test there cannot fail. ThreadSanitizer,
under gcc in WSL, reports every ordering bug — and also the correct fence spelling.

The deque is a bounded Chase–Lev with no standalone fences. Its slots are stored with release and
loaded with acquire, because it carries pointers to tasks whose contents the thief must see. A full
deque means the task runs inline. Edges live in an immutable shape, and the per-run state is pooled.
Only the thread that completes the sink resets an instance, with plain stores, published by the
injection queue's lock.

</details>

[**solution.md**](solution.md) — the worked C++ answer, with every number produced by
`bin/solution.exe` or, for the ThreadSanitizer column, `bin/solution_tsan`.

| This step | Answered in |
|---|---|
| C1 | §3 the contract, spelled · §5 the deque |
| C2 | §2 relaxed, read in machine code |
| C3 | §5 the deque |
| C4 | §4 the last touch |
| C5 | §8 the bug × tool table |

Then read what the same questions look like in the other two:
[go/question.md](../go/question.md) · [python/question.md](../python/question.md).
