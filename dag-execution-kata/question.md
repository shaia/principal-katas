# Kata 4 — counted is not published

## The brief

You're designing a C++20 parallel execution engine for an endpoint security product.

Each incoming security event triggers a directed acyclic graph (DAG) of analysis tasks.

For example:

```text
                 Process Event
                      |
             +--------+--------+
             |                 |
        Inspect File      Inspect Network
             |                 |
             +--------+--------+
                      |
                Correlate Data
                      |
                 Apply Policy
                      |
                 Emit Result
```

**Requirements:**

- Up to 1 million active tasks.
- 64 worker threads.
- Tasks have between 0 and 8 dependencies.
- A task must not execute before all its predecessors finish.
- Results produced by predecessors must be visible to dependent tasks.
- No global scheduler lock.
- Minimize task-scheduling overhead.
- p99 event-processing latency below 5 ms.

A developer implements:

```cpp
struct Task {
    std::atomic<uint32_t> pending_dependencies;

    std::vector<Task*> successors;

    std::function<void()> execute;
};

void complete(Task& task, ReadyQueue& ready)
{
    for (Task* next : task.successors) {
        if (next->pending_dependencies.fetch_sub(
                1, std::memory_order_relaxed) == 1) {

            ready.push(next);
        }
    }
}
```

Assume `ReadyQueue` is a correctly synchronized concurrent queue.

The developer argues:

> The atomic counter guarantees that the dependent task executes only after all predecessors complete. Therefore, the implementation is correct.

**Your challenge:** Is the implementation correct under the C++20 memory model?

Explain the difference between dependency completion and memory visibility. Then design an execution architecture that minimizes scheduling overhead while preserving correctness.

## How to use this kata

The interview brief takes 20–25 minutes; the extended exercise takes longer. Answer each step before opening its hint. The shared steps concern graph execution; language-specific memory and runtime rules belong in the tracks. Read the folded sketch last.

## Step 0 — State the graph contract

What does one edge promise? Distinguish task body return, successful output, and terminal accounting. State graph mutation and lifetime rules.

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

Consider two runs of the same graph, and one task that throws.

</details>

**Done when** you can state which facts belong to the reusable graph and which belong to a run.

---

## Step 1 — Counted is not published

Draw A and B feeding C. Mark the counter modification order and every actual happens-before edge. Which result does the ready queue publish?

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

Only one predecessor calls push. Draw that thread in a different color.

</details>

**Done when** you can identify the unsynchronized conflicting accesses without relying on stale-cache language.

---

## Step 2 — Publish through fan-in

Specify the atomic decrement and prove that the final predecessor acquires every earlier result. Does replacing the queue with a mutex fix the original bug?

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

Separate synchronization on the counter from synchronization on the queue.

</details>

**Done when** your proof covers eight predecessors and arbitrary completion order.

---

## Step 3 — Hand work to another worker

Design local queues, external injection, and stealing. Identify every producer and consumer of each queue operation.

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

A single-owner deque cannot accept arbitrary pushes from submission threads.

</details>

**Done when** each operation has an owner and a publication rule.

---

## Step 4 — Find the lower bound

Compute total work and weighted critical path for 10,000 one-microsecond tasks, including a 2,000-task chain. What can 64 workers possibly improve?

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

A lower bound is not a scheduling prediction.

</details>

**Done when** you can explain max(W/P,L) and name the overhead it excludes.

---

## Step 5 — Price the task boundary

At 450 ns per task, count counters, queue operations, allocations, and indirect calls. Which work can remain on the current worker?

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

Removing a queue trip does not remove a dependency edge.

</details>

**Done when** you can propose one measurable optimization and its fairness cost.

---

## Step 6 — Make stealing bounded

Explain why 9 million steal attempts can coexist with low utilization. Define a search budget and an idle-to-sleep handshake.

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

A task can be published between the last empty check and sleeping.

</details>

**Done when** you can walk both sides of that race and explain eventual progress.

---

## Step 7 — Keep the critical event moving

Compare background throughput with foreground p99. Define priority, a service quota, and a continuation budget.

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

Pure depth-first locality can keep one event warm and another waiting.

</details>

**Done when** your policy gives ordinary work service even under continuous urgent arrivals.

---

## Step 8 — Place workers and data

Compare 16 and 64 workers on a 32-logical-CPU machine. Separate oversubscription, SMT, cache movement, and NUMA effects.

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

The number of threads is not the number of independent execution resources.

</details>

**Done when** you can name a controlled experiment for each proposed cause.

---

## Step 9 — Fail without abandoning the graph

Define terminal outcomes for exceptions, canceled work, and invalid inputs. Who notifies the children of a skipped node?

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

A dependency can be finished without having produced a usable value.

</details>

**Done when** every accepted node reaches exactly one terminal outcome and every edge is accounted once.

---

## Step 10 — Drain before reclaiming

Race submission with shutdown and cancellation with a final completion. Include speculative thieves and callbacks that retain references.

**Be prepared to say:** the invariant, a concrete interleaving or example, and an experiment that could disprove your performance hypothesis.

<details>
<summary>Hint</summary>

The last result is not automatically the last scheduler reference.

</details>

**Done when** you can state precisely when run storage, deque storage, and callback contexts may be destroyed.

---

## Follow-up: more workers, worse latency

 — More workers, worse latency

Your work-stealing scheduler produces these results:

| Metric | 16 workers | 64 workers |
|---|---:|---:|
| Throughput | 850K tasks/sec | 1.4M tasks/sec |
| CPU utilization | 88% | 42% |
| p50 event latency | 1.2 ms | 900 µs |
| p99 event latency | 4 ms | 18 ms |
| Steal attempts/sec | 200K | 9M |
| Average task duration | 450 ns | 450 ns |

The scheduler achieves higher aggregate throughput, yet p99 latency becomes substantially worse.

**Challenge:** Explain why increasing the worker count causes this behavior and redesign the scheduler to improve tail latency.

Your answer should address task granularity, stealing amplification, cache-line contention, critical-path scheduling, worker oversubscription, and NUMA-aware placement.

Consider whether tasks on an event's critical path should receive different scheduling treatment from independent background tasks.



These figures are supplied hypothetical observations. Do not claim they identify one unique cause.

## Evaluation rubric



| Level | Expected reasoning |
|---|---|
| **Senior** | Understands DAG scheduling, atomic dependency counters, and why only the last predecessor schedules a task. |
| **Strong Senior** | Identifies that relaxed atomic operations do not establish the required happens-before relationships. |
| **Staff** | Designs correct acquire/release synchronization, efficient ready queues, and work-stealing execution. |
| **Principal** | Analyzes critical-path limits, task granularity, memory locality, load balancing, cancellation, and scheduling progress guarantees. |

**Critical distinction:** An atomic dependency counter establishes a scheduling condition, but relaxed operations do not necessarily publish the data produced by predecessor tasks.

---



## Language tracks

[C++](cpp/question.md) · [Go](go/question.md) · [Python](python/question.md).

<details>
<summary>Answer sketch — commit to an answer first</summary>

The counter identifies the final predecessor but must also aggregate publication. Acquire-release decrements followed by a synchronized queue handoff give the required chain. Use immutable graphs and separate run state, local work queues with explicit injection, bounded continuations and stealing, and terminal accounting that propagates failure without executing invalid inputs. Measure against both work/span limits and event arrival times.

See the [preserved source](reference-outline.md) and [C++ worked answer](cpp/solution.md).

</details>
