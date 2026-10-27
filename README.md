# principal-katas

Systems-design questions at the level where the answer is an argument rather than a data structure —
each worked end to end, with running code that measures whether the argument actually holds.

The format is one question answered independently in more than one language. Not a port: each track
re-derives the design from its own language's cost model, and the tracks are allowed to reach
different conclusions. Where they do, the disagreement is the finding.

## The katas

| | the question | tracks |
|---|---|---|
| [**mpsc-event-pipeline-kata**](mpsc-event-pipeline-kata/) | Many producers push small events to a single consumer that batches them into a socket. Millions of events/sec, bounded memory, producers must never block indefinitely, and order is preserved **per producer, not globally** — the clause that permits an architecture with no shared coordination point at all. Follow-up: 64 producers but only 4–8 active, so stop polling 64 empty queues without reintroducing a contended global. | C++ · Go · Python |
| [**plugin-dispatch-kata**](plugin-dispatch-kata/) *(C++ answered)* | 50–100 protocol handlers, some built in and some loaded from shared libraries, selected by linear scan at millions of packets/sec. Profiling blames `dispatch`; the team proposes four polymorphism mechanisms. **The profile never said the virtual call was the problem**, and all four options change how `process` is called rather than how many times `matches` is. Then the half it leads to: the right mechanism at the ABI boundary and the right mechanism on the hot path are not the same one, and nothing requires them to be. | C++ · Go · Python |
| [**mpmc-work-queue-kata**](mpmc-work-queue-kata/) *(questions only)* | A security agent's bounded work queue: 32 producers, 16 consumers, 5 M events/sec, 65,536 fixed slots, consumers that sleep, producers that block or reject, a lossless shutdown, and a p99 enqueue under 2 µs — plus a ten-line queue made of two atomic counters. Its bugs are one bug: **it cannot tell a slot that is reserved from one that is published**, and with 32 producers finishing out of order no pair of counters can. Then the half that follows: once the distinction is representable, preemption, lost wakeups, shutdown races and tail latency are all about how long a slot stays reserved. | C++ · Go · Python |

## Long-form

- [**Your profiler said `dispatch`. It did not say `virtual`.**](plugin-dispatch-kata/doc/your-profiler-said-dispatch.md)
  — the plugin-dispatch C++ answer walked through end to end, with generated figures and animations,
  including the three claims that did not survive being measured.

## How a kata is laid out

- **`question.md`** — the question staged as a kata: numbered steps that build the argument in the
  order it has to be built, each with probes, a hint, and a self-assessable gate. The answer sketch
  is folded at the bottom, so the question is usable before you read the answer.
- **`<lang>/question.md`** — what the shared question cannot ask, because it only exists in that
  language.
- **`<lang>/solution.md`** — the worked answer and the measurements it rests on.
- **`<lang>/`** — the implementation, plus a benchmark binary that asserts the invariants under
  shared labels and **exits non-zero if one breaks**. Every number quoted in a solution comes from
  running it.

Each kata's README carries its own build, run, and race/thread-checking instructions.
