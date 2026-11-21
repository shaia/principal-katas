# The CPP track

Extends [the shared kata](../question.md). Do steps 0–10 first. Answer before opening the hint.

## Step C1 — Prove the fan-in

Annotate each atomic operation in an eight-way fan-in. Compare acquire-release decrements with release decrements plus an acquire fence on the last arrival. Explain release sequences in C++20.

<details>
<summary>Hint</summary>

The final RMW reads the immediately preceding modification.

</details>

**Done when** you can defend a proof from predecessor writes to successor reads, including the queue handoff.

## Step C2 — Prove the deque separately

Identify the publication operation and the last-item arbitration in Chase–Lev. Why are reusable slots atomic even if only the owner writes them?

<details>
<summary>Hint</summary>

A thief can lose its reservation after reading a reused slot.

</details>

**Done when** you can defend tests for empty take, reuse, full capacity, and owner/thief last-item races.

## Step C3 — Own every borrow

Specify graph, run, callback context, output, queue-entry, and speculative-thief lifetimes. What happens when the caller drops its handle?

<details>
<summary>Hint</summary>

Graph immutability does not extend the lifetime of a void pointer.

</details>

**Done when** you can defend an ownership diagram and a reclamation argument.

## Step C4 — Separate failure from completion

Catch a callback exception and drain a diamond without reading an invalid result. Race cancellation with dispatch.

<details>
<summary>Hint</summary>

Cancellation is a request, not thread interruption.

</details>

**Done when** you can defend a terminal-state table with exactly one notification per edge.

## Step C5 — Sleep without global serialization

Write the empty-check, registration, recheck, and wakeup protocol. Identify which locks are control-path only.

<details>
<summary>Hint</summary>

A correctly synchronized queue does not by itself prevent missed wakes.

</details>

**Done when** you can defend a progress argument that states whether timed polling is a backstop.

## Step C6 — Measure the claim

Control optimization, task cost, warmup, load, and arrival schedule. Which checks need TSan, which need a memory-model proof, and which need different hardware?

<details>
<summary>Hint</summary>

Instrumentation can accidentally add the synchronization a broken test lacks.

</details>

**Done when** you can defend a reproducible comparison that distinguishes correctness from speed.

<details>
<summary>Answer direction — read last</summary>

Publication must compose through the counter and through the queue. Keep ownership, terminal accounting, and idle-worker signaling as separate proofs. See [the worked answer](solution.md).

</details>
