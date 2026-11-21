# The Python track

Extends [the shared kata](../question.md). Do steps 0–10 first. Answer before opening the hint.

## Step P1 — Read the scheduler already present

Use graphlib.TopologicalSorter as a coordinator. What do get_ready and done guarantee, and what do they not synchronize?

<details>
<summary>Hint</summary>

Ordering metadata is not an execution engine.

</details>

**Done when** you can defend a design with one owner for sorter mutation.

## Step P2 — Pick the execution boundary

Compare CPU-bound pure Python, native calls releasing the GIL, free-threaded Python, and process workers.

<details>
<summary>Hint</summary>

The GIL status and the work type both matter.

</details>

**Done when** you can defend a version-specific choice of thread or process execution.

## Step P3 — Price the edge crossing

Account for pickling, IPC, result size, and coordinator traffic for 450 ns tasks. Where should tasks be fused?

<details>
<summary>Hint</summary>

A process boundary can cost more than the useful work.

</details>

**Done when** you can defend a batching experiment with equivalent outputs.

## Step P4 — Interpret future completion

Define what a completed Future means after an exception. When may its successor run, and how do skipped nodes become done in the sorter?

<details>
<summary>Hint</summary>

Terminal and successful are different predicates.

</details>

**Done when** you can defend a failure-propagation algorithm that cannot strand sorter activity.

## Step P5 — Cancel what can be canceled

Compare pending futures, running callbacks, shutdown, and broken worker pools. Who owns shared-memory outputs?

<details>
<summary>Hint</summary>

Future cancellation does not interrupt arbitrary running Python.

</details>

**Done when** you can defend a lifetime and drain contract for every outcome.

## Step P6 — Measure the right latency

Measure arrivals, admission, serialization, execution, and return transport separately. Why does sequential submit-and-wait hide overload?

<details>
<summary>Hint</summary>

The time an event should have arrived is part of the measurement.

</details>

**Done when** you can defend a reproducible experiment that records interpreter and GIL mode.

<details>
<summary>Answer direction — read last</summary>

Keep a single graph coordinator, move only appropriately coarse work to executors, and propagate terminal outcomes independently of successful values. See [graphlib](https://docs.python.org/3/library/graphlib.html).

</details>
