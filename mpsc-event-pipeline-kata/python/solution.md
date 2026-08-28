# High-throughput in-process event pipeline — the Python answer

Answer to [question.md](../question.md). Written from Python's own cost model, not
translated from the [C++](../cpp/solution.md) or [Go](../go/solution.md) answers — those are worth
reading, but nearly every decision below is made for a different reason, and two of them come out
the opposite way.

---

## The answer, up front

**If your producers are coroutines — and for anything emitting telemetry at volume in Python, they
usually are — use [`weir.aio`](src/weir/aio.py) and stop reading.** One event loop, per-producer
lists, batch `b"".join`, an awaitable backpressure hook. No locks, no atomics, no wakeup protocol,
and no atomicity question at all, because nothing preempts anything. It is about 150 lines and most
of this document does not apply to it.

**If your producers are genuinely threads,** the design is one bounded `bytearray` per producer that
the producer packs into with `struct.pack_into`, drained by a single consumer whose entire drain is
a `memoryview` slice, with an active-producer set so the consumer never touches an idle buffer.

**And know the ceiling before you start.** Measured on a 32-core i9-13980HX, the whole interpreter
sustains **1.2–2.7 M events/s**, flat-to-declining as producers are added. The question asks for
millions per second. You can just barely have *one* million. If you need ten, no arrangement of
Python objects will get you there and the answer is to move the hot path out of Python — which is
§10, and is not a cop-out but the actual engineering answer.

---

## 1. What Python changes about the question

The C++ answer's central move is to refuse a shared queue, because a single shared tail is a cache
line written by every producer core, and that line is the scalability limit. It is a good argument
and **it does not apply here**, because there are no producer cores. There is one interpreter
executing one bytecode at a time.

Measured: eight threads pushing through *one shared* `queue.SimpleQueue` cost 51.8 ns/op against
48.7 ns for eight *completely independent* deques. **Sharing costs 6%.** Producer-producer
contention, the thing the entire C++ architecture exists to eliminate, was already serialised away
before you wrote a line.

So the question becomes a different one, and it has a clean answer:

> Under one interpreter lock, the only currency is **how much interpreted bytecode runs per event**.
> Every design decision is about moving work into C calls, and about moving work off the consumer,
> which is the one participant you cannot shard.

Everything below follows from that sentence.

---

## 2. Why a `bytearray` ring, decided by measurement

Four candidate structures, measured end to end — a producer submitting one event *plus* the
consumer draining it into the wire bytes a socket would actually take:

| design | ns/event | notes |
|---|---|---|
| **`bytearray` ring + `pack_into` + memoryview drain** | **131** | no lock |
| `bytearray.extend` + whole-buffer swap, **no lock** | 151 | unsafe; a cost floor, not a design |
| `list.append(bytes)` + swap + `b"".join`, with lock | 207 | |
| `bytearray.extend` + whole-buffer swap, with lock | 267 | |
| — an uncontended `threading.Lock`, on its own — | **96** | |

**That last row decides it.** An uncontended lock costs about as much as the entire rest of the
push, so any design taking one per event has spent its budget before doing any work. This is the
single most useful number in the document and it is not intuitive: "lock-free" in Python is not
about contention, it is about the 96 ns you pay even when nobody is competing with you.

The ring avoids the lock, and not through cleverness. The producer owns `_tail`, the consumer owns
`_head`, and each update is one `STORE_ATTR` — and a single bytecode cannot be interrupted. Single-
writer index ownership is what makes the lock unnecessary. That is the entire justification for the
index bookkeeping, and it is a Python reason, not a memory-model one.

The second reason is the drain, and it is larger:

| drain + serialise, per event | ns |
|---|---|
| **memoryview slice (this design)** | **~1** |
| whole-list swap + `b"".join` | 85 |
| `popleft` pre-packed bytes + `b"".join` | 128 |
| `popleft` tuple + `pack_into` | 220 |

`pop_into` moves up to 512 events with one slice. The consumer executes *no per-event bytecode at
all*, and since the consumer sets the ceiling for everything, that is worth spending producer-side
bytecode to buy.

**The trade, stated plainly:** per push, this ring is more expensive than `deque.append` (137 ns
against 76 ns). It wins end to end because the deque pays that back with interest on the thread that
cannot afford it. Measured in the full pipeline at 8 producers, `BytesRing` holds **100% acceptance**
where `DequeRing` collapses to **24%**.

The buffer is also already in wire format, so there is no serialisation pass at all — the batch
handed to the sink is a `memoryview` a socket can take directly.

### What did *not* survive the port

Things that are load-bearing in C++ and are noise here, deleted rather than translated:

- **Cache-line padding.** Two Python ints are two heap objects behind a pointer. There is nothing to
  pad and nothing to false-share.
- **The `_mm_pause` spin rung.** A busy-spin in the consumer's idle loop holds the GIL and starves
  the producers it is waiting for. Not merely useless — *negative*. `time.sleep(0)` is the only
  correct yield.
- **`SpinThenDrop` as a spin.** It is a `sleep(0)` yield loop; there is nothing to spin on.
- **All memory-ordering annotations.** See §3.

---

## 3. What "atomic" means here, and why the obvious answer is wrong

There is one interpreter lock, so there is sequential consistency by construction: no `release` to
write, no `acquire` to pair with it. The question that *does* need answering is which operations are
indivisible — and the received wisdom is wrong in a way worth the space.

The naive claim is that this loses updates, because it is a read-modify-write across several
bytecodes:

```python
self._bitmap |= bit
#  LOAD_ATTR _bitmap / LOAD_FAST bit / BINARY_OP | / STORE_ATTR _bitmap
```

**It does not.** Eight threads × 50,000 increments of exactly that shape produced exactly 400,000,
zero lost, with the switch interval pinned to 1 ns. CPython checks the eval breaker only at specific
instructions — `JUMP_BACKWARD`, `RESUME`, `CALL` — and a straight-line load/modify/store contains no
checkpoint, so no thread switch can land inside it.

Now insert a checkpoint:

```python
box.v = ident(box.v) + 1        # a CALL now sits between the load and the store
# 8 threads x 20,000 -> expected 160,000, got 45,595.  71.5% LOST.
```

Identical logical operation. Refactored in a way any reviewer would wave through. **Loses most of
its updates.**

So the integer bitmap is not safe because of anything you reasoned about — it is safe because of
where CPython happens to place its checkpoints, and it stops being safe when someone extracts a
helper. That is a far worse property than being outright broken, and it is invisible to testing
because the unsafe version passes.

> **The rule this package follows:** never rely on a multi-bytecode sequence being atomic. Use forms
> that are atomic *by construction* — one bytecode dispatching to one C call that neither releases
> the GIL nor re-enters the interpreter.

Hence [`ActiveSet`](src/weir/active_set.py) uses a `list` of flags, where `flags[i] = 1` is a single
`STORE_SUBSCR`. It is also measurably cheaper: 46 ns on the fast path against 87 ns for the int
bitmap, and 271 ns/pass to scan against 1105 ns.

Both experiments are in [`tests/test_atomicity.py`](tests/test_atomicity.py) as real tests, so the
claim is executable rather than argued.

**And the sequencing still matters even though the annotations are gone.** Deleting them makes it
look as though statement order stopped mattering. It did not: a thread switch can land between two
Python statements exactly as a CPU can reorder two instructions, so the slot write must still
precede the index publish, and the copy-out must still precede the index release. Same hazard, same
required order, no annotation to remind you. That is strictly more dangerous than C++, not less.

---

## 4. Bounded buffers and overload

This is the part of the C++ answer that transfers completely, and it transfers because it was never
about concurrency. Bounded buffer, explicit terminating policy, drops counted per producer and never
silent.

Measured against a deliberately slow sink:

```
slow sink: pushed=42314 dropped=37686 (47.1%) high_water=4096 worst_push=380.2 us
abort shutdown returned in 32.5 ms
```

`high_water` capped at capacity, worst push in the hundreds of microseconds, drops counted.

Two Python-specific traps, both of which will bite someone:

**`collections.deque(maxlen=N)` silently discards the *oldest* element and tells you nothing.**
That is the overwrite-oldest policy the C++ answer rejects as unimplementable — handed to you as a
one-word default, uncounted, and it looks like exactly what you want. If you use a deque, bound it
with an explicit length check.

**`out = list(d); d.clear()` is the fastest drain measurable (13 ns/event) and is wrong.** An
`append` landing between those two statements is lost. Only `popleft` in a loop is safe — or a
design that does not need to drain a shared structure at all.

And one that disqualifies the fastest option outright: **`queue.SimpleQueue` is unbounded.** It is
the quickest thing in the comparison below, and it fails the specification, because an unbounded
queue is not a solution to overload — it is a deferral of it, converting a latency problem into a
memory problem that arrives later, larger, and as an OOM kill.

### Against what a Python engineer would actually write

60,000 events per producer, identical harness and sink:

| producers | transport | sustained M/s | accepted | ns/push | bounded |
|---|---|---|---|---|---|
| 1 | this design | 1.44 | 100% | 693 | yes |
| 1 | `queue.Queue` | 0.39 | 100% | 1,953 | yes |
| 1 | `queue.SimpleQueue` | **2.71** | 100% | **337** | **no** |
| 1 | `deque` per producer | 2.69 | 100% | 371 | yes |
| 4 | **this design** | **1.54** | **100%** | 2,593 | yes |
| 4 | `queue.Queue` | 0.08 | 45.5% | 15,489 | yes |
| 4 | `queue.SimpleQueue` | 1.08 | 100% | 882 | no |
| 4 | `deque` per producer | 1.21 | 28.7% | 734 | yes |
| 8 | **this design** | **1.23** | **100%** | 6,528 | yes |
| 8 | `queue.Queue` | 0.04 | 22.7% | 34,127 | yes |
| 8 | `queue.SimpleQueue` | 0.47 | 100% | 1,919 | no |
| 8 | `deque` per producer | 0.59 | 17.3% | 1,614 | yes |

**At one producer, the simple answers win by 1.9×.** A `deque` per producer gets 2.69 M/s against
this design's 1.44. If you have one or two producers, write the deque and go home.

**At four and above, this design is the only one that both stays bounded and loses nothing.** It has
the highest sustained rate of any bounded option, and it is the only one holding 100% acceptance —
the deque drops 71% of events at four producers and 83% at eight, because its consumer pays
100+ ns of bytecode per event and cannot keep up.

**`queue.Queue` is a catastrophe and should be the headline warning.** 34 µs per push at eight
producers, 22.7% accepted, 0.04 M/s — **fifty times worse** than the alternatives. It is also the
first thing most Python engineers reach for. The cost is not contention; it is a `Condition`
acquire/notify/release per item.

---

## 5. Waking the consumer

The consumer must sleep when idle rather than burn the interpreter, and waking it correctly is the
subtle part.

**The protocol:** each side publishes its own state before reading the other's. The producer appends
to its buffer, *then* sets its flag; the consumer clears a flag, *then* re-checks the buffer and
sets it back if it was wrong. No interleaving can leave both sides believing the other will act.

In C++ this requires an explicit `seq_cst` fence, because the hardware may reorder a store past a
later load. Here there is no reordering to prevent — only interleaving — so the protocol is the same
shape with nothing to annotate. **The statement order is the whole mechanism.**

**Flags are cleared only on the path to sleeping, never after each drain.** Clearing eagerly
produces flag-write churn proportional to throughput: a lightly loaded buffer goes briefly empty
between arrivals, the consumer clears, the producer immediately sets it again. Measured here: **4
flag writes across a whole run**, one per active producer. (The C++ version's first implementation
cleared eagerly and did 582,954 — and no test failed. This is why it is counted.)

### The 15.5 ms condvar, and the rung that exists because of it

`threading.Condition.wait(timeout)` on Windows sleeps **~15.5 ms regardless of the timeout
requested** — 200 µs, 1 ms and 5 ms all measured 15.45–15.50 ms, the scheduler tick. `threading`
timed waits go through `WaitForSingleObjectEx` and never received the high-resolution timer
`time.sleep` gained in 3.11.

So a missed-wakeup backstop of 200 µs, which is what you would write, is actually 15.5 ms — off by
77×. The idle ladder therefore has a rung with no counterpart in the other two answers:

```
time.sleep(0)  x200   ->  clear flags  ->  one full scan  ->  Condition.wait(timeout)
```

with a `time.sleep(0.0005)` poll rung before the condvar, so a missed wakeup costs ~560 µs instead
of 15.5 ms. Defence in depth, sized to the platform's actual behaviour rather than to the number in
the config.

---

## 6. Narrowing the scan — and here the answer inverts

The follow-up asks: with 64 producers but only 4–8 active, how do you stop the consumer polling 64
empty buffers, without reintroducing a contended synchronisation point?

The C++ answer builds an active bitmap, and then reports honestly that **it does not improve the
tail** — because probing an idle ring there is an L1 cache hit, and 64 L1 hits do not move a p99.9
that has OS preemption in it.

**In Python it is the largest optimization in the document.** 4 active producers at 20 kHz each,
sweeping how many idle producers are also registered:

```
registered scan   |  p50 us   p99 us    p99.9 | probes/pass  flag-wr
         4 full   |    80.3    322.5    486.8 |         3.9        0
         4 bitmap |    82.8    645.5   1048.6 |         3.8        4
        16 full   |   149.6    641.5   1178.4 |        15.9        0
        16 bitmap |    75.9    478.8    845.7 |         3.8        4
        64 full   |   278.0    780.8   1281.6 |        63.4        0
        64 bitmap |    73.2    433.5    787.4 |         3.9        4
```

At 64 registered: **p50 falls 278 → 73 µs, p99 781 → 434, p99.9 1282 → 787.** All three percentiles
separate, and full-scan p50 degrades monotonically with registration while the active-set version
stays flat.

**And the C++ answer predicts this exactly.** Its §8 says the optimization would pay when probing a
ring costs more than an L1 hit. In Python a probe is not a cache access — it is a list index, an
attribute load, a method call and a comparison, ~100 ns of interpreted bytecode. It always costs
more than an L1 hit. The condition the C++ author identified as hypothetical is Python's default
state.

**I predicted the opposite before running it**, reasoning from the instrument's noise floor, and
wrote that prediction into the benchmark. It was wrong, and the reason it was wrong is the
interesting part: I reasoned about the *effect size* in nanoseconds and forgot that the effect
scales with the consumer's per-pass cost, which in Python is enormous.

What does *not* transfer is the justification. The C++ argument is "make shared state read-mostly,
because a line read by 64 cores sits Shared in 64 caches for free" — a cache-coherence argument.
Python has no such asymmetry, and every attribute read is a refcount *write* to a shared object
header, so "read-mostly" is not a category the language offers. Same mechanism, entirely different
reason, much bigger payoff.

---

## 7. The GIL switch interval

The C++ answer's cache-behaviour section has a Python replacement, and it is not a smaller effect —
it is a bigger one. `sys.setswitchinterval` defaults to 5 ms. Four producers:

| interval | accepted | sustained |
|---|---|---|
| 5 ms (default) | 13.7% | 0.55 M/s |
| 1 ms | 17.1% | 0.64 M/s |
| **0.5 ms** | **100%** | **1.74 M/s** |
| 0.1 ms | 100% | 0.95 M/s |

At the default, a producer holds the GIL for 5 ms, fills its buffer in the first fraction of that,
and spends the rest dropping — while the consumer cannot get the GIL to drain. **A 3.2× throughput
difference and a 7× acceptance difference from one line**, dwarfing every data-structure choice
above.

Note it is not monotonic: 0.1 ms is *worse* than 0.5 ms, because handoff overhead starts to dominate.
There is a real optimum, and it is worth measuring on your own workload rather than copying 0.0005
from here.

This package sets it at import, in [`platform.py`](src/weir/platform.py), with the measurements in
the module docstring. Tuning a global interpreter setting from a library is normally rude; here it
is the single largest lever available and hiding it would be worse.

---

## 8. Ownership, lifetime, shutdown

Producer slots go `FREE → ACTIVE → RETIRING → FREE`, and **only the consumer publishes `FREE`**,
after draining the buffer. A departing producer cannot release its own slot, because it cannot know
whether the consumer is mid-batch inside it.

In C++ this protocol prevents a use-after-free. Here the collector makes that impossible — the
consumer holds a reference, so nothing can be freed underneath it. The protocol survives as a
*correctness* protocol: do not hand a slot to a new producer while the previous owner's events are
undrained, and do not lose their counters at handover. Real, but lower stakes, and worth saying
rather than implying otherwise.

**One accommodation that is purely Python's.** Reclaiming runs on the consumer's idle passes, which
is correct — it is an O(64) scan that would otherwise cancel out the active-set optimization. In C++
and Go, producers are fast enough relative to the consumer that idle passes are plentiful. In Python
they are not: a churn workload retires eight producers and immediately registers eight more while
the consumer, sharing one interpreter with all of them, has had no idle pass in which to reclaim
anything. So `register_producer` waits briefly and wakes the consumer. The invariant is untouched —
the consumer is still the only thread that publishes `FREE` — but a design that never blocks in C++
must block here, and that is a scheduling reality rather than a protocol change.

**Shutdown** is two modes with a deadline. Stop accepting *first*, because draining a buffer that is
still being filled is a race you can lose indefinitely; then wait, bounded, for quiesce; then stop
the consumer and do final full-scan passes. `stop()` is idempotent and the pipeline is a context
manager. The consumer thread is a daemon, so a forgotten `stop()` cannot hang interpreter exit.

The quiesce check runs on the stopping thread, which is neither producer nor consumer, so it must
read the owned indices rather than the producer's private cached copy. Cached state is fast
precisely because it is unshared, and that makes it unavailable to observers.

---

## 9. Measuring, and the instrument

Same methodology as the other two answers, because it is the most transferable thing in the kata:
open-loop load at a fixed rate, latency measured from the **intended** send time, percentiles never
means, preallocated sample buffers, medians taken per statistic.

Three Python-specific facts that change how it must be built:

**`time.sleep` cannot pace anything.** Requests of 1, 10, 50 and 100 µs all return in ~510–560 µs.
At the 50 µs periods this benchmark uses, sleeping would put every send ten periods late. The pacer
spins on `perf_counter_ns`.

**But spinning costs more here than it does in C++.** There, a spinning pacer costs a core. Here it
costs the *interpreter*, stalling the very consumer whose latency you are measuring. That caps the
number of paced producers at about four — a limit set by the GIL, not by core count.

**A per-event callback on the consumer destroys the run.** 16 producers × 200k with an inline
ordering check in the consumer: **173,931 accepted, 3,026,069 dropped — 94.6% loss.** The same run
without it: 3,200,000 accepted, 0 dropped. The C++ `std::function` hook is affordable; its Python
equivalent is off by a factor of eighteen.

So verification moves entirely off the hot path: the consumer captures raw batch bytes (~1 ns/event)
and ordering and accounting are checked afterwards with `struct.Struct.iter_unpack` (159 ns/event,
untimed). This is the single most important structural difference between this benchmark and the
C++ one, and it is not stylistic. It also makes full parity affordable — 16 producers × 200k runs in
~1.6 s plus ~0.5 s to verify — so the stress tests are *not* scaled down.

`perf_counter_ns` itself costs 40–86 ns against a ~200 ns push budget, so latency is sampled 1-in-8
rather than the C++'s 1-in-4.

---

## 10. When not to use any of this

**The ceiling is real.** 1.2–2.7 M events/s for the entire interpreter, flat-to-declining with
producer count. The C++ answer on the same machine reaches 362 M/s and *rises*. If the requirement
is genuinely millions per second, no arrangement of Python objects reaches it, and the honest
answers are:

- **Move the hot path out of Python.** A C, Cython or Rust extension holding the ring, releasing the
  GIL around the batch drain. This is the only change that makes the C++ reasoning apply again,
  because it is the only one that restores real producer parallelism.
- **Go cross-process.** `multiprocessing.shared_memory` gives N processes with N interpreter locks
  and genuine parallelism. Measured: 418 ns/event to push (slower than in-process, because
  shared-memory buffer access costs more) but ~1 ns/event on the consumer side, so it wins above
  ~2 producers. Its real problem is not throughput: cross-process there is no GIL, so the entire
  memory-ordering question that §3 says evaporates **comes back in full** — and Python offers no
  primitive to express it. You would be relying on CPython implementation details across a process
  boundary, which is a much weaker position than this document's.
- **Use asyncio.** If the producers are I/O-bound, this is not a fallback, it is the right answer —
  see below.
- **Do not use Python for this.** A legitimate engineering conclusion, and the one to reach for if
  the requirement is firm.

**What a free-threaded build would change.** Nothing here is tested on 3.14t (not installed), and
that is a stated gap in the same way the C++ answer states its missing ThreadSanitizer. It *would*
give real producer parallelism, which would make the C++ design's premise true again. It would also
break every atomicity claim in §3 — `flags[i] = 1` and `self._tail = t + 1` would need real atomics,
which Python does not offer at any width. And per-object locks, biased refcounting and a ~40%
single-thread slowdown mean a 4-producer free-threaded build may well not beat GIL CPython at all.
That is a claim to measure, not to assert, and this document does not assert it.

---

## 11. The asyncio answer

[`weir.aio`](src/weir/aio.py) is not an appendix. For the workload this question actually describes
in most real Python systems — request tracing, structured logging, metrics — the producers are
coroutines, and then:

- One thread. No GIL contention to design around, no switch interval to tune.
- Cooperative scheduling. Nothing preempts between two statements, so **§3 does not merely collapse,
  it ceases to be a question.**
- No locks, no atomics, no wakeup handshake. The event loop *is* the handshake.
- The drain is `b"".join` over a list handed over by rebinding — O(1), no copy, no per-event loop.

What survives is exactly the part that was never about concurrency: bounded buffers, an explicit
terminating overload policy, counted drops, batching, per-producer accounting.

And one thing gets *better* than either the C++ or the threaded Python can manage:

```python
async with AsyncEventPipeline(sink) as pipe:
    p = pipe.producer("worker-1")
    p.push(seq)
    await pipe.drain_pressure(p)     # yield instead of dropping
```

**Backpressure becomes something you can await.** A coroutine can be told to wait without blocking
anything else, so overload becomes a scheduling decision rather than data loss — and it still
satisfies "producers must not block indefinitely", because the loop keeps running. The threaded
design can only drop. Verified in
[`tests/test_aio.py`](tests/test_aio.py): with `drain_pressure` in the loop, 20,000 events through a
512-slot buffer produce **zero drops**; without it, the bound holds and every drop is counted.

---

## 12. Verification

```powershell
uv venv --python 3.13; .\.venv\Scripts\Activate.ps1
uv pip install -e ".[dev]"

pytest -q                  # 26 invariant tests, ~10 s
pytest -q -m slow          # full 16 x 200k parity stress, ~39 s
python -m weir_bench       # the measurement phases; exit code is the verdict
python -m weir_bench --phase gil        # the switch-interval sweep
python -m weir_bench --phase baselines  # against queue.Queue / SimpleQueue / deque
```

**Split, deliberately.** pytest owns everything whose output is a verdict — ordering, exact
accounting, lossless drain, lifetime churn, bounded depth, shutdown modes, the asyncio pipeline, and
the atomicity experiments — parametrised over both ring backends and both scan policies. A single
`python -m weir_bench` owns everything whose output is a *number*. The C++ single-binary design is a
constraint of that language, not a choice; copying it into a language with a standard test runner
would be cargo-culting.

There is no race detector, and none is needed: under the GIL the races it would look for are
unwritable. The honest equivalent gap is a free-threaded run, named in §10.

### What building this actually caught

- **A `time.sleep`-based slow sink that never triggered the drop policy.** At 400 µs per batch —
  copied from the C++ — the sink is still *faster* than four GIL-bound Python producers, so nothing
  backed up and the backpressure test asserted on a system that was never under pressure. Scaling
  the constant to the language rather than copying it is what made the test test something.
- **A registry that ran dry under churn.** Reclaim happens on consumer idle passes, and in Python
  those do not arrive fast enough. Failed on the first run.
- **A `flag_writes` counter reading zero**, because it counted the consumer's clears and not the
  producers' sets — so the coalescing claim in §5 was untested while appearing verified.
- **A prediction in §6 that was flatly wrong**, written into the benchmark before running it. Left
  in the code, and in this document, because the reasoning error is more useful than the conclusion.
- **An `asyncio` producer packing its own event count as the producer id**, which the ordering
  assertion caught immediately — the one bug here a type checker would not have found and a test
  did.
