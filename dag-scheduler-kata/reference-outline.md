# The reference outline

The answer outline that came with the question, reproduced as it was given; only the formatting is
restored. [cpp/solution.md §11](cpp/solution.md#11-the-reference-outline-checked) checks it claim by
claim against measurements.

Read [question.md](question.md) and commit to an answer first. This is an answer.

---

## Concise answer outline

The last predecessor must acquire the effects of earlier predecessors before publishing the
successor. An acq_rel RMW chain on the dependency counter can establish that relation, assuming every
predecessor decrements exactly once and the ready queue correctly transfers publication. Use work
stealing carefully; tiny tasks may need fusion or inline continuation.

## Key tradeoffs

| choice | buys | costs |
|---|---|---|
| tiny tasks | more flexibility | higher scheduler overhead |
| task fusion | lower overhead | less balancing freedom |
| critical-path priority | lower event latency | starvation risk |
| work stealing | good utilization | potential cache/NUMA damage |

## Evaluation rubric

- **Senior:** understands dependency counts.
- **Strong Senior:** identifies missing publication of predecessor writes.
- **Staff:** uses correct synchronization and work-stealing queues.
- **Principal:** reasons about critical path, task size, NUMA, priority, and cancellation/failure
  propagation.

## Follow-up exercise

Going from 16 to 64 workers raises throughput but worsens p99 from 4 ms to 18 ms while steal attempts
explode. Explain stealing amplification and redesign around task granularity, NUMA, priority, and
worker budgets.
