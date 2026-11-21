# Original Session 33 reference

Preserved source material. Read [the kata](question.md) before this answer. The source performance table is a hypothetical exercise, not a measurement from this repository.

---

# C++ Principal Practice — Session 33

**Topic:** Parallel DAG Execution, C++20 Memory Ordering, Task Scheduling, and Critical-Path Optimization

**Difficulty:** Senior → Principal  
**Duration:** 20–25 minutes  
**Focus:** Concurrency, release/acquire synchronization, work stealing, scheduling overhead, and systems design.

## 1. Challenging question

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

---

## 2. Evaluation rubric

| Level | Expected reasoning |
|---|---|
| **Senior** | Understands DAG scheduling, atomic dependency counters, and why only the last predecessor schedules a task. |
| **Strong Senior** | Identifies that relaxed atomic operations do not establish the required happens-before relationships. |
| **Staff** | Designs correct acquire/release synchronization, efficient ready queues, and work-stealing execution. |
| **Principal** | Analyzes critical-path limits, task granularity, memory locality, load balancing, cancellation, and scheduling progress guarantees. |

**Critical distinction:** An atomic dependency counter establishes a scheduling condition, but relaxed operations do not necessarily publish the data produced by predecessor tasks.

---

## 3. Concise answer outline

### A. The memory-ordering problem

Consider a task `C` depending on two tasks, `A` and `B`.

```cpp
int result_a;
int result_b;

void task_a()
{
    result_a = 42;
    complete(A, ready);
}

void task_b()
{
    result_b = 84;
    complete(B, ready);
}

void task_c()
{
    process(result_a, result_b);
}
```

Assume both results are initially valid storage, and only their respective predecessor writes them.

A possible execution:

```text
Thread 1                 Thread 2                 Thread 3

result_a = 42

pending.fetch_sub(1)
   relaxed
                         result_b = 84

                         pending.fetch_sub(1)
                            relaxed
                            returns 1

                         enqueue(C)
                                                  dequeue(C)

                                                  read result_a
                                                  read result_b
```

The atomic counter correctly reaches zero.

However, **the relaxed decrement by A does not establish a happens-before relationship between A's write and C's read**.

The ready queue can publish B's preceding writes because B enqueues C. It does not automatically publish unrelated writes performed by A.

Consequently, C's access to `result_a` can constitute a data race.

This is undefined behavior, even if the implementation happens to work on a particular x86-64 system.

### B. Correct synchronization

A straightforward correction is:

```cpp
void complete(Task& task, ReadyQueue& ready)
{
    for (Task* next : task.successors) {
        const auto previous =
            next->pending_dependencies.fetch_sub(
                1,
                std::memory_order_acq_rel);

        if (previous == 1) {
            ready.push(next);
        }
    }
}
```

Why does this work?

Each predecessor performs an acquire-release read-modify-write operation on the same dependency counter.

The operations form an atomic modification order.

Each decrement reads the value written by the preceding decrement, and the acquire/release relationships establish transitive synchronization between them.

The final predecessor therefore acquires the effects published by earlier predecessors.

Its subsequent queue publication makes those effects visible to the worker executing the dependent task, assuming the queue provides the necessary release/acquire handoff.

Conceptually:

```text
Task A writes result
        |
        v
   release decrement
        |
        v
   acquire/release chain
        |
        v
Last predecessor observes zero
        |
        v
   release enqueue
        |
        v
   acquire dequeue
        |
        v
Task C safely reads results
```

This reasoning depends on several invariants: the graph is fully initialized before execution, each predecessor decrements exactly once, result storage remains alive, and predecessor tasks do not continue modifying data that their successors read.

**Principal-level point:** Correct memory ordering is part of the scheduler's API contract, not an implementation detail that task authors should have to compensate for.

---

## 4. Principal-level architecture — Work stealing

A single global ready queue may become a scalability bottleneck.

Instead, use per-worker task deques.

```text
               DAG Scheduler
                     |
       +-------------+-------------+
       |             |             |
       v             v             v
    Worker 0      Worker 1      Worker 2
       |             |             |
   Local deque   Local deque   Local deque
       |             |             |
       +-------------+-------------+
                     |
              Work stealing
```

Each worker preferentially processes tasks from its local deque.

When it runs out of work, it attempts to steal tasks from another worker.

A common approach uses a Chase–Lev-style work-stealing deque, in which the owner and stealing threads have different access patterns.

However, the synchronization protocol must be proven separately from the dependency-counter protocol. A correct decrement does not make an incorrectly implemented deque safe.

### Why work stealing helps

Work stealing can improve:

- Load balancing across irregular DAGs.
- Cache locality for related tasks.
- Reduction of global synchronization.
- Utilization when task durations vary.

But it introduces additional concerns:

- Cross-core cache-line transfers.
- Task migration and NUMA locality.
- Contention among stealing workers.
- Memory reclamation if deque storage can grow.
- Scheduling fairness for latency-sensitive tasks.

The ideal implementation depends on the shape of the DAG, not merely the number of available cores.

---

## 5. The critical-path limitation

Suppose a DAG contains 10,000 tasks.

Each task takes approximately 1 µs.

With 64 workers, a naive throughput estimate is:

\[
T \approx \frac{10{,}000 \times 1\ \mu s}{64}
\]

Approximately 156 µs.

But this ignores dependencies.

Let:

- \(W\): Total work across all tasks.
- \(P\): Number of processors.
- \(L\): Length of the critical path.

Any execution has the lower bound:

\[
\boxed{T_P \geq \max\left(\frac{W}{P}, L\right)}
\]

If the critical path contains 2,000 sequential tasks, each requiring 1 µs:

\[
L = 2\text{ ms}
\]

No amount of additional parallelism can reduce execution time below 2 ms without changing the dependency structure or reducing task costs.

Actual execution time is greater because of scheduling, communication, and other overheads.

**A Principal engineer asks whether the scheduler is slow or the computation simply lacks sufficient parallelism.**

---

## 6. Key tradeoffs

| Decision | Benefit | Cost |
|---|---|---|
| Global task queue | Simple scheduling | Centralized contention |
| Per-worker deques | Locality and scalability | Stealing complexity |
| Small tasks | High scheduling flexibility | More atomic operations and queue traffic |
| Task fusion | Lower scheduling overhead | Reduced scheduling flexibility |
| Work-first execution | Efficient dependency chains | Potential fairness and stack-depth concerns |
| Priority scheduling | Better latency for critical tasks | Starvation and coordination complexity |

I would consider **task fusion** when several inexpensive tasks form a short sequential chain.

For example:

```text
Parse → Normalize → Classify → Update
```

If each stage takes only 100 ns, repeatedly enqueueing and dequeuing the next stage may cost more than the work itself.

Executing consecutive eligible tasks on the same worker can eliminate unnecessary scheduling operations.

The tradeoff is reduced flexibility to rebalance work and potentially longer periods before the worker services other tasks.

---

## 7. Follow-up exercise — More workers, worse latency

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

### Principal-level extension

Suppose one task in the graph fails or is canceled.

How do you ensure dependent tasks are not executed with invalid inputs, while guaranteeing that all outstanding task references and scheduler resources are eventually released?

A strong answer defines explicit terminal states for tasks, propagates dependency failure through the graph, and separates **task completion accounting from successful result publication**.

### Interview takeaway

The central lesson is that **correct dependency scheduling requires both an execution-order guarantee and a memory-visibility guarantee**.

At Principal level, the challenge extends beyond atomics: you must understand how DAG structure, synchronization, scheduler overhead, and cache locality jointly determine throughput and tail latency.