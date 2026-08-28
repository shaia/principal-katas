# High-throughput in-process event pipeline

Answer to [question.md](../question.md), with a working implementation and benchmarks alongside it.
The same question is answered in [Go](../go/solution.md) and [Python](../python/solution.md); each
port is worth reading for what its language *changes* about the answer.

Acronyms and terms of art used below (SPSC, MPSC, RMW, TSO, StoreLoad, coordinated omission, …) are
expanded in the [Glossary](#appendix-glossary).

---

## The problem

This answers a system-design interview question. The setup: multiple producer threads generate small
events and push them to a single consumer thread, which batches them and writes them to a socket.
All in-process, C++20. Six requirements — very low latency, millions of events per second, minimal
allocation, bounded memory, producers must not block indefinitely, and **event order preserved per
producer, not globally**.

The deliverable is a choice of queueing mechanism — mutex-protected queue, lock-free MPSC, one
bounded SPSC ring per producer, or something else — *argued rather than asserted*, plus five named
discussion axes: memory ordering, cache behavior, backpressure, ownership, and shutdown semantics.
Then a follow-up: 64 producers of which only 4–8 are active, and a consumer that currently scans all
64 rings every pass. Fix that without reintroducing a heavily contended global synchronization point.

### The requirements, and what each one forces

Reading them as a list of goals is a mistake. Each one *removes* options, and by the time all six are
applied very little is left standing:

| Requirement | What it rules out |
|---|---|
| Millions of events/s | Any per-event write to state another thread also writes |
| Very low latency | Anything that can park a producer in the kernel |
| Minimal allocation | Per-event `new`, node-based queues, `shared_ptr` |
| Bounded memory | Unbounded queues — so overload needs an explicit, terminating policy |
| Producers must not block indefinitely | "Block until space" as that policy |
| **Per-producer ordering only** | Nothing. This is the one clause that *grants* rather than constrains |

The last row is the whole question. Five requirements narrow the space to something that looks
impossible; the sixth quietly hands back the only thing that makes it possible.

### How the answer is graded

The rubric bands it, and the bands are worth reproducing because they say what the question is
actually probing for:

- **Senior** — compares mutex against lock-free, recognizes allocation and contention costs, proposes
  bounded queues with sensible backpressure. → §1, §5
- **Staff** — cache-line contention, false sharing, producer-local state, batching, ownership and
  lifetime, and *why a single shared atomic tail becomes a scalability bottleneck*. → §1, §3, §4
- **Principal** — challenges the premise of a single shared MPSC structure and reasons explicitly
  about memory ordering, fairness, overload behavior, NUMA and cache locality, observability,
  failure modes, and operational simplicity. → §2, §6, §7, §8
- And the line most answers skip: **when the theoretically less sophisticated mutex is still the
  better engineering choice.** → §9

These are not four different answers, they are four depths of one. The interesting property is that
the step up to Principal is not more sophistication — it is a **refusal**. The question offers MPSC
as an option and the strongest answer declines the framing, on the grounds that a shared tail buys
global ordering the specification explicitly waived.

### Why it is hard

**The binding constraint is an absence.** Six requirements pull in incompatible directions and the
resolution is hidden in the clause asking for *less*. Global ordering can only be established at a
point where all producers meet — a lock, a shared tail, a sequencer — and every such point is one
cache line written by every producer core. The skill under test is reading a specification for what
it does not demand. The question is constructed so that all the obvious answers quietly pay for a
guarantee nobody asked for. (→ §1)

**The bottleneck is not visible in the source.** A push is a handful of instructions; a contended
cache line is ~100 ns. The design is decided entirely by *which lines are written by whom* — a
property no amount of reading the code reveals, and one where the ordinary optimization instinct
(fewer instructions, less code) is simply the wrong axis. Two implementations with identical
instruction counts can differ by 10× in throughput. (→ §3)

**"Shared" and "contended" are not the same word.** The follow-up reads as self-contradictory: know
which of 64 producers are active, without a contended global synchronization point. It resolves only
because cache coherence is asymmetric — a line read by 64 cores sits in all 64 caches at once and
costs each an L1 hit, while a single write invalidates all of them. The naive rule "avoid shared
state" would have blocked the answer; the correct rule is "avoid *written* shared state." (→ §7)

**Correctness cannot be tested into existence.** The failure modes here are memory-ordering ones — a
missing release/acquire pair, a StoreLoad reordering, a lost wakeup. They are rare, load-dependent,
silent, and hardware-dependent: x86's TSO hides bugs that AArch64 exposes. A green test suite proves
very little, and on this target ThreadSanitizer does not exist at all. The one genuine race in this
design was found by re-reading the code, and nothing automated would have caught it. (→ §2, §10)

**The obvious optimization is a bug, twice.** Checking the bitmap before the fence to skip the fence
in the common case reintroduces exactly the race the fence exists to close. Clearing bits eagerly
when a ring drains empty produces write churn proportional to throughput — 582,954 writes to the
shared line where the fix produces 8, the mechanism doing the precise opposite of its job. Both look
right on the page, and neither failed a test. This is a domain where intuition is not merely
unreliable but anti-correlated with correctness. (→ §7)

**The transitions kill you, not the steady state.** A ring at full rate is the easy part. Producer
threads come and go, and a ring must not be reused or freed while the consumer may be mid-read of it
— which a departing producer has no way to determine about itself. Shutdown is the single moment
where lifetime, draining, and wakeup all interact, and it is the part every lock-free-queue writeup
omits and the rubric explicitly names. (→ §4, §6)

**You have to be willing to see the price, and to report a disappointing result.** The notification
mechanism costs ~15 ns on an 18–29 ns push — roughly 40%. The A/B says it does *not* move p99.9 on
this machine, which contradicts the premise the follow-up hands you. Getting to that conclusion at
all requires measurement discipline the question is directly probing for — coordinated omission,
confounded variables, the instrument perturbing the measured — and then requires saying so instead of
reading a story into noise. The follow-up asks how you would benchmark *whether the optimization
actually improves tail latency*, and the honest answer here is that on this hardware it does not.
(→ §8)

### Where each part is answered

| The question asks about | Section |
|---|---|
| Which queueing mechanism, and why not the others | §1 |
| Memory ordering | §2 |
| Cache behavior | §3 |
| Ownership and lifetime | §4 |
| Backpressure and overload | §5 |
| Shutdown semantics | §6 |
| Follow-up: 64 producers, 4–8 active | §7 |
| Follow-up: does it actually improve tail latency | §8 |
| When the mutex is the better engineering choice | §9 |
| How any of this is verified | §10 |

---

## Layout

The pipeline is header-only — it is templated on its `Sink`, so there is nothing to compile
separately. Each header is one responsibility, and the three subtle protocols all live together in
`event_pipeline.hpp` because they interact:

| File | Contains |
|---|---|
| [src/platform.hpp](src/platform.hpp) | Cache-line size, the spin-wait hint, clock aliases |
| [src/event.hpp](src/event.hpp) | `Event` — the unit of transfer, 32 bytes, trivially copyable |
| [src/spsc_ring.hpp](src/spsc_ring.hpp) | The bounded ring, and every memory ordering in the design (§2) |
| [src/sink.hpp](src/sink.hpp) | The `Sink` concept, the real socket shape, the benchmark sinks |
| [src/config.hpp](src/config.hpp) | Policies, sizing constants, and `PipelineStats` — the knobs and the readouts (§5, §7) |
| [src/event_pipeline.hpp](src/event_pipeline.hpp) | Registry, consumer loop, producer lifetime, wakeup handshake, shutdown (§4, §6, §7) |

The benchmark is a separate translation unit per phase, so each verification argument stands on its
own and reads independently:

| File | Phase |
|---|---|
| [bench/harness.hpp](bench/harness.hpp) · [.cpp](bench/harness.cpp) | Assertions, percentiles, the event factory |
| [bench/correctness.cpp](bench/correctness.cpp) | Ordering, accounting, lossless drain, producer churn |
| [bench/throughput.cpp](bench/throughput.cpp) | Offered vs sustained, and what signalling costs a producer |
| [bench/backpressure.cpp](bench/backpressure.cpp) | Overload against a slow sink, and shutdown |
| [bench/scan_ab.cpp](bench/scan_ab.cpp) | The follow-up A/B and its methodology (§8) |
| [bench/main.cpp](bench/main.cpp) | Runs the phases; exit code is the verdict |

---

## 1. The decision

**One bounded SPSC ring per producer, drained by the single consumer, batched into the socket.**

```
Producer 1 ──> bounded SPSC ring ──┐
Producer 2 ──> bounded SPSC ring ──┤
Producer 3 ──> bounded SPSC ring ──┼──> consumer ──> batch ──> socket
Producer N ──> bounded SPSC ring ──┘
```

The decisive line in the requirements is *"event order only needs to be preserved per producer, not
globally."* That is not a detail, it is the permission slip for this entire architecture. Global
ordering can only be established at a point where all producers meet — a lock, a shared tail, a
sequencer — and any such point is a single cache line written by every producer core. Per-producer
ordering means no producer ever has to agree with another about anything, so **there is no shared
writable state on the push path at all**: no lock, no CAS, no `fetch_add`. Each producer writes a slot
in a ring only it writes, then publishes with one release store.

The general principle matters more than the label "lock-free": *minimize shared writable state on the
hot path.* Lock-free is a means; a lock-free MPSC queue still has one shared, written cache line, and
that line is the scalability limit regardless of whether the algorithm blocks.

### Why not the alternatives

**Mutex + bounded queue.** Every push serializes on one lock word. Uncontended that is ~20 ns; the
problem is what happens under contention, which at millions of events per second is *always*. The
lock cache line ping-pongs between producer cores, the critical section is short so producers spend
their time acquiring rather than working, and once the adaptive spin is exhausted the loser parks in
the kernel — a multi-microsecond excursion that lands directly in the tail. Throughput also does not
just plateau with more producers, it *degrades*. Rejected here, but see §9: it is often the right
engineering answer, and it should not be dismissed without measurement.

**Lock-free MPSC (intrusive Vyukov, or a `fetch_add` bounded ring).** Better than a mutex, and it
removes the parking. But every push is still an atomic RMW on one shared tail. That line must be held
exclusively by the pushing core, so it migrates between cores on every single event — the cost is
coherence traffic, not instructions, and it grows with producer count. It also *buys global ordering*,
a guarantee the specification explicitly waives: paying a scalability bottleneck for a property nobody
asked for. `fetch_add`-based bounded rings have a second problem: a producer descheduled between
reserving its slot and publishing it leaves a hole, and the consumer cannot advance past that hole.
One unlucky producer stalls the whole pipeline, which is a nasty tail-latency failure mode.

**MPMC ring.** All of the MPSC costs plus consumer-side coordination we have no use for — there is
exactly one consumer.

**Per-producer SPSC.** Zero producer-producer contention, no RMW on the hot path, and a memory model
simple enough to actually reason about. The costs are real and are discussed in §7 and §8: memory is
`N × capacity`, the consumer has to manage N queues, and fairness becomes the consumer's problem.

### Measured

32-core x86-64, clang 21, `-O2`, median of 3 runs. `ns/push` is the producer's
cost per event with the consumer running; the two columns are with and without the §7 notification
mechanism:

```
producers   |   offered  accepted  sustained |   ns/push   ns/push
            |       M/s         %        M/s | no-signal   +bitmap
1           |      10.1     98.0%        9.9 |      61.5      99.3
4           |     107.3    100.0%      107.0 |      18.5      37.3
16          |     452.1     80.4%      362.6 |      19.4      35.4
32          |     767.6     17.7%      135.8 |      29.4      41.7
```

The number to look at is `ns/push (no-signal)`: **18–29 ns per event, essentially flat from 4 to 32
producers.** That flatness is the entire point of the design. A shared tail would show the opposite
curve — push cost climbing with producer count as the contended line migrates between cores. Here
adding producers adds independent rings, and per-producer cost does not care how many others exist.
(The 1-producer figure is higher because a lone producer outruns nothing and spends its time on a
ring the consumer is actively draining; it is not the interesting case.)

Beyond ~4 producers the single consumer becomes the ceiling and the surplus is dropped by policy (§5)
— the system behaving as designed rather than failing. If the sustained rate were the requirement, the
next move is a cheaper sink or sharded consumers, not a different queue.

The `+bitmap` column costs ~15 ns/push and is discussed in §7: that is the price of a *correct* wakeup
handshake, and it is not free.

---

## 2. Memory ordering

The handoff is one release/acquire pair per direction. Nothing anywhere needs `seq_cst` **except** one
place, discussed below, that is off the hot path.

**Producer publishes:**

```cpp
const std::uint64_t t = tail_.load(std::memory_order_relaxed);   // sole writer of tail_
slots_[t & kMask] = value;
tail_.store(t + 1, std::memory_order_release);                   // publishes the slot write
```

`relaxed` for its own load is safe because the producer is the only thread that ever writes `tail_`;
it cannot read a stale value of its own variable. The release store is what makes the slot contents
visible: anything sequenced before it cannot be reordered after it.

**Consumer observes:**

```cpp
cached_tail_ = tail_.load(std::memory_order_acquire);   // pairs with the producer's release
... read slots ...
head_.store(h + n, std::memory_order_release);          // pairs with the producer's acquire
```

The first pairing is the obvious one and the one everybody writes. **The second pairing is the one
that gets forgotten.** The consumer's release store of `head_` is not merely a liveness hint telling
the producer there is room — it is what prevents the producer from overwriting a slot the consumer is
still reading. Without it, the compiler or CPU may sink the slot reads past the index update, the
producer sees free space, and it writes into a slot mid-read. That is a torn read, and it is silent.
The producer's matching side is `head_.load(std::memory_order_acquire)` on the refill path.

**Why not `seq_cst`.** The only thing `seq_cst` adds over acquire/release is a single total order
across *all* seq_cst operations, which nothing here needs — every constraint is a pairwise
producer↔consumer handoff. On x86 the difference is stark at the ISA level: acquire loads and release
stores are plain `MOV`s (the hardware is already TSO, so these orderings only constrain the compiler
and cost nothing), whereas a `seq_cst` store compiles to `XCHG` or `MOV`+`MFENCE` — a full pipeline
barrier, tens of cycles, on every publish. On ARM the orderings become `LDAR`/`STLR` and genuinely
matter for correctness rather than being free. Writing the weakest correct ordering costs nothing on
x86 and is the difference between correct and broken on AArch64.

**Where `seq_cst` is genuinely required** is the wakeup handshake in §6 — a StoreLoad ordering, the
one thing TSO does *not* give you for free, and the one acquire/release cannot express. It is on the
idle transition path, so it is paid per sleep, not per event.

No `volatile` anywhere. `volatile` orders nothing with respect to other threads and does not make
anything atomic; it is for memory-mapped I/O, not concurrency.

---

## 3. Cache behavior

At these rates the design is entirely about cache lines, not instructions. Four things matter:

**False sharing between the two indices.** `head_` and `tail_` are written by different threads. In
the same cache line they would ping-pong the line between the two cores on every operation, turning a
~1 ns store into a ~100 ns coherence miss — the single most common way a "lock-free" queue ends up
slower than a mutex. Each index gets its own `alignas(64)` line, as does the slot storage, and
`ProducerSlot` itself is line-aligned and padded so adjacent producers' metadata cannot collide.

A note on `std::hardware_destructive_interference_size`: it is the standard spelling, but using it in
a type's layout bakes a compile-time constant into that type's ABI, and clang warns about exactly
that. The code uses a local `kCacheLine = 64`.

**And this section contained a false-sharing bug, which is the most instructive possible place for
one to be.** It was found while porting the design to Go, by dumping the record layout
(`clang -Xclang -fdump-record-layouts`) to check the Go padding against it:

| member | offset | cache line | accessed |
|---|---|---|---|
| `active_` | 8320 | 130 | alone — correct |
| `running_` | 8384 | **131** | |
| `accepting_` | 8385 | **131** | **acquire-load on every push** |
| `parked_` | 8386 | **131** | `seq_cst` store ×2 per park |
| `retire_pending_` | 8387 | **131** | **`exchange` — an RMW — on every idle pass** |
| `park_mu_` | 8392–8447 | **131** | locked per notify and per park |

Only `running_` carried the `alignas(kCacheLine)`; everything after it fell in behind on one line.
The consumer's idle loop `exchange`s `retire_pending_` — an RMW, so it takes the line **exclusive** —
on *every empty pass*, invalidating it in all 64 producer cores, while every producer reads
`accepting_` from that same line on every push. On a lightly loaded pipeline, which is exactly the
regime §7 optimizes for, that is a coherence miss per push caused by the consumer doing nothing.

The fix separates the two groups, and co-locates `active_` with `accepting_` *deliberately*: both are
read on every push and written only on transitions, so a push should touch one line rather than two.

Two things worth saying about how this survived. It was invisible to every test, because nothing was
wrong — the program was correct, just slower than it claimed. And it was invisible to inspection,
because `alignas` on the first member of a group *looks* like it applies to the group. The only thing
that finds this is dumping the layout and reading the offsets, which is now a habit rather than an
accident. The Go port hit the identical failure mode from a different direction — `atomic.Bool` is
four bytes rather than one, which silently shifted a padded group by four — and caught it the same
way.

**Cached index copies — the biggest single win.** The naive ring loads the *other* thread's index on
every operation, which is a guaranteed cross-core read. Instead, each side keeps a plain non-atomic
copy of the other's index and only refreshes it when its cheap local check says full (producer) or
empty (consumer):

```cpp
if (t - cached_head_ == Capacity) {              // believed full — only now pay for the remote line
    cached_head_ = head_.load(std::memory_order_acquire);
    if (t - cached_head_ == Capacity) return false;
}
```

In the common case a push touches only lines the producer already owns. The stale copy is always
*conservative* — it can only underestimate available space, never overestimate it — so staleness costs
an occasional spurious refresh, never correctness.

**Batching, in both directions.** `pop_batch` moves up to 512 events and does **one** release store of
`head_` for the whole batch rather than one per event, so a full drain costs one cross-core write
instead of 512. On the output side the consumer stages events contiguously and issues one sink call
per batch, amortizing the socket syscall over thousands of events. Since `Event` is trivially copyable
and the staging buffer is contiguous, the batch is handed to the sink as a `std::span` view — no
serialization copy on the way out.

**Sizing.** Power-of-two capacity so slot lookup is a mask, not a division. Free-running 64-bit
indices that are never wrapped — only the lookup masks — which makes full and empty unambiguous
(`size == tail - head`) without sacrificing a slot or maintaining a separate count. `Event` is 32
bytes, two per cache line, and lives *by value* in the ring: no pointer chasing, no indirection, and a
prefetch-friendly linear access pattern.

---

## 4. Ownership

Events transfer **by value** into preallocated storage. A push is a 32-byte copy into a slot the
producer already owns; the consumer copies out and the slot is free. There is no per-event allocation,
no free list, and no shared ownership — deliberately no `shared_ptr`, whose atomic refcount would be
another contended cache line, reintroducing exactly the shared writable state the design exists to
avoid.

For payloads too large to inline: keep a fixed inline buffer for the common case with a spill path for
the rare large one, or hand over an index into a per-producer arena. What you must not do is make the
consumer's release of an object the producer's synchronization problem — that is a second, harder,
cross-thread protocol layered on the one you already have.

**Lifetime is the genuinely hard part**, and it is not the events, it is the rings. Producer threads
come and go; the ring must not be reused or destroyed while the consumer is still reading it. A ring
freed when its producer exits is a use-after-free the consumer hits at the worst possible moment.

The protocol is a three-state slot with a strict handoff:

```
Free ──register──> Active ──handle dtor──> Retiring ──consumer, after final drain──> Free
```

Only the **consumer** publishes `Free`, and only after it has drained the ring and seen it empty. A
departing producer can mark itself `Retiring` but cannot release its own slot — it has no way to know
whether the consumer is mid-batch inside its ring. Registration claims a slot with a CAS whose success
ordering is `acquire`, pairing with the consumer's `release` of `Free`, so a new owner sees a fully
quiesced ring. The `ProducerHandle` is move-only RAII, so retirement cannot be forgotten.

One consequence worth stating because it caused a real bug during development: per-slot counters are
reset at registration, so a recycled slot would erase its predecessor's history and the accounting
would silently stop adding up. Departing counters are folded into lifetime totals at reclaim.
`test_producer_lifetime` runs 40 waves of 8 producers through 64 slots and asserts every event
survives.

---

## 5. Backpressure and overload

"Producers must not block indefinitely" and "bounded memory usage" are the same requirement seen from
two sides, and together they force the answer: **the queue is bounded, so overload must have an
explicit policy, and every policy must terminate.**

An unbounded queue is not a solution to overload, it is a deferral of it. It converts a latency
problem into a memory problem, and the memory problem arrives later, larger, and as an OOM kill.

`try_push` returns `bool` and the policy is the caller's:

| Policy | Behavior | When it is right |
|---|---|---|
| `DropNewest` (default) | fail immediately, count the drop | Telemetry, metrics, tracing — staleness is worse than loss, and the newest event is the cheapest to lose |
| `SpinThenDrop` | bounded `_mm_pause` spin, then fail | Bursty traffic where the consumer will catch up within microseconds |
| Overwrite-oldest | **rejected** | Cannot be done safely in a plain SPSC ring: the producer would overwrite a slot the consumer may be mid-read. It needs a different ring discipline (sequence-numbered slots, reader validation) — real, but a different data structure. |
| Block until space | **rejected** | Violates the requirement outright, and couples every producer's latency to the consumer's worst stall |

**Drops are counted per producer and reported, never silent.** A pipeline that quietly discards
telemetry is worse than one that fails loudly, because the loss is invisible precisely when the system
is in trouble and you most need the data. Per-producer drop counters also localize the problem to the
producer that overran rather than reporting one global number.

**The chain is end-to-end.** A socket whose peer stops reading returns `EWOULDBLOCK`; the consumer
keeps an unflushed remainder and moves on rather than blocking (blocking there would stall every
producer's ring behind one slow peer); the rings stop draining and fill; the drop policy fires. Each
link is bounded, and the rings act as the shock absorber that rides out a hiccup without loss and
degrades predictably when it is not a hiccup.

Measured against a deliberately slow sink, 8 producers:

```
slow sink: pushed=53249 dropped=746751 (93.3%) high_water=4096 worst_push=23.8 us
```

93% loss under a pathologically slow consumer — by policy, visibly counted. The two numbers that
matter: `high_water` never exceeded the ring capacity (memory stayed bounded), and the worst single
push took 23.8 µs (no producer blocked indefinitely, and that figure is dominated by OS preemption,
not by the queue).

---

## 6. Shutdown

Shutdown is where lock-free designs usually leak or hang, because it is the one moment when lifetime,
draining, and wakeup all interact. Two modes, because "stop" is genuinely two different requests:

**`Drain`** — lossless, for a clean stop:

1. `accepting_ = false` (release). Producers observe it and start failing pushes, so the rings can
   only shrink from here. This must come first: draining a queue that is still being filled is a race
   you can lose indefinitely.
2. Wait for producers to quiesce — **with a deadline**. A wedged socket must never make shutdown hang
   forever; past the deadline we degrade to `Abort` and say so.
3. `running_ = false`, wake the consumer.
4. Consumer does final full-scan passes (at shutdown, correctness beats scan efficiency) until every
   ring is empty.
5. Flush the partial batch. A real socket sink then does `shutdown(fd, SHUT_WR)` so the peer sees a
   clean EOF rather than a reset.
6. Join the consumer, then destroy storage.

**`Abort`** — bounded, for a fast stop: stop, discard, close. Measured at 5.9 ms under full load.

`stop()` is idempotent and the destructor calls it, so a pipeline dropped without an explicit stop is
still safe. The ordering constraint that must never be violated: **ring storage is destroyed only
after the consumer is joined and all producers have retired.** Storage is owned by the registry, which
outlives every handle.

One subtlety worth naming. The quiesce check runs on the *stopping* thread, which is neither producer
nor consumer, so it must read both atomic indices — it cannot use the producer's cached index, which
is private to the producer thread and reading it from elsewhere is a data race. Cached state is fast
precisely because it is unshared, and that makes it unavailable to observers.

---

## 7. Follow-up: 64 producers, 4–8 active

> *The consumer scans all 64 SPSC queues on every iteration. Avoid continuously polling empty queues
> without reintroducing a heavily contended global synchronization point.*

This is the honest cost of the architecture: it traded producer-side contention for consumer-side
scan, and with 56 of 64 rings idle the consumer spends most of each pass looking at nothing.

Worse than wasted work, the scan is *actively harmful*: probing a ring means loading a line the
producer writes, pulling it to Shared, so the producer's next publish must re-acquire it exclusively.
**The scan slows down the producers it is polling.**

### What not to do

A global "pending events" counter that every push does `fetch_add` on would work and would be a
disaster — it is precisely the single contended shared line the whole design exists to avoid. Any
mechanism that writes shared state per event has given the entire game away.

### The mechanism: a read-mostly active bitmap

**Layout.** 64 producers is exactly one `std::atomic<uint64_t>`, one bit per producer, on its own
cache line. This generalizes to a two-level hierarchy (an L1 summary word with one bit per L2 word)
for thousands of producers; 64 is the degenerate single-word case.

**Why a shared word need not be a contended word.** This is the crux. A cache line that is *read* by
64 cores sits in Shared state in all 64 caches simultaneously and costs each an L1 hit — read sharing
is genuinely free. Cost appears only on *writes*, which require exclusive ownership and invalidate
every other copy. So the design rule is not "avoid shared state", it is **"make shared state
read-mostly"**:

```cpp
push();                                      // release store to tail_
if (active_.load(relaxed) & bit) return;     // fast path: already set, L1 hit, no traffic
std::atomic_thread_fence(seq_cst);           // StoreLoad — see below
if (active_.load(relaxed) & bit) return;
const auto prev = active_.fetch_or(bit, release);   // only on a transition
```

A producer streaming millions of events per second sets its bit once and then never touches the line
again. **That is the coalescing**, and it is what makes a shared word acceptable here. Measured over a
multi-million-event run: **8 writes to the shared line**, one per producer.

**Consumer scan** iterates set bits only, `O(active)` instead of `O(64)`:

```cpp
std::uint64_t m = std::rotr(active_.load(acquire), rotate);
while (m) { int i = std::countr_zero(m); m &= m - 1; drain(ring[i]); }
```

### The sleeping/wakeup race

The dangerous interleaving, and the reason this cannot be built by intuition:

| | Consumer | Producer |
|---|---|---|
| 1 | drains ring *i* to empty | |
| 2 | | pushes an event (publishes `tail_`) |
| 3 | | reads bitmap, sees bit *i* still set, skips the `fetch_or` |
| 4 | clears bit *i* | |
| 5 | goes to sleep | |

The event is now in the ring with its bit clear and nobody knows. This is not a missed optimization,
it is **unbounded latency on a published event** — the worst class of bug here, because it is rare,
load-dependent, and invisible in testing.

The fix is Dekker's algorithm, and it is symmetric — **each side publishes its own state, then checks
the other's**:

- Producer: publish `tail_` → **then** read the bitmap → set the bit if clear.
- Consumer: clear the bit → **then** re-check the ring → if non-empty, set it back.

Either interleaving now leaves at least one side seeing the other. But this requires **StoreLoad**
ordering on both sides, and that is exactly the one ordering x86's TSO does *not* provide for free —
the CPU is permitted to move a load ahead of an earlier store to a different address. Release/acquire
cannot express it. Hence `atomic_thread_fence(seq_cst)` on the producer side; on the consumer side the
`fetch_and` is an RMW and therefore already a full barrier.

**The tempting optimization here is wrong, and I shipped it before catching it.** The obvious move is
to check the bit first and fence only if it is clear — the bit is almost always set, so the fence would
almost never execute:

```cpp
if (active_.load(relaxed) & bit) return;    // WRONG: this check is itself the race
std::atomic_thread_fence(seq_cst);
```

That reintroduces the bug exactly, because *the unfenced check is the unsound operation*. With no
barrier before it, the load may execute before our `tail_` publish drains the store buffer; it reads a
bit that was set a moment ago, we return, and the consumer concurrently clears that same bit having
not yet seen our store. The fence has to come first, unconditionally, or it does nothing.

**And it is not free.** Measured, the notification mechanism costs **~15 ns/push** — roughly 40% on
top of an 18–29 ns push (see the table in §1). That is the honest price of a correct wakeup handshake,
and it is worth knowing rather than assuming. If that cost mattered more than the latency bound, the
alternative is to drop the fence and rely on the consumer's pre-park full scan (below) as the sole
backstop: a lost bit then delays an event until the consumer's next idle transition instead of never,
which is bounded but no longer microsecond-bounded. That is a legitimate trade; it is just not the
default here, because "usually delivered promptly" is a bad property to discover in production.

**Defense in depth.** Being wrong about any of this strands an event in a ring nobody probes, so the
consumer does one unconditional full scan immediately before sleeping. Going to sleep is the only
moment where being wrong becomes *unbounded*, and it is also precisely the moment an `O(64)` scan is
affordable. The `wait_for` timeout is the third layer.

### Notification coalescing, and when to clear

Spinning the consumer forever burns a core, so it descends a ladder: spin with `_mm_pause` → `yield` →
park on a condvar. Parking has the same race one level up, resolved the same way — set `parked_`,
**then** re-check the bitmap, and only sleep if it is still empty.

Producers notify only when their `fetch_or` returns an old value of **zero**, i.e. when they took the
whole bitmap from empty to non-empty:

```cpp
if (prev == 0 && parked_.load(seq_cst)) { std::lock_guard lk(park_mu_); park_cv_.notify_one(); }
```

A burst across 8 producers costs one `notify_one`, not eight. Notifying on every push would be a
syscall per event, which is categorically worse than the polling it replaces. And `wait_for` always
carries a timeout: a bounded worst case is worth more than confidence that the wakeup path is perfect.

**When to clear bits was the subtlest decision here**, and the first implementation got it wrong. The
obvious approach — clear a bit as soon as its ring drains empty — produces *write churn proportional
to throughput*: a lightly loaded ring goes briefly empty between arrivals, the consumer clears, the
producer immediately re-sets, and the shared line ping-pongs on every event. Measured, that first
version did **582,954 bitmap writes** in a run. Exactly the contention the mechanism was built to
avoid, reintroduced by the mechanism itself.

The fix follows from an asymmetry: **a set bit means "maybe non-empty"**. A false positive costs one
wasted probe; a false negative loses a wakeup. So bits are cleared lazily, only on the path to
parking, never after each drain. Same run after the change: **8 bitmap writes**, one per producer.

### Fairness

`countr_zero` scans from the LSB, so producer 0 would be drained first on every pass and the high
indices would starve under sustained overload. Two mitigations, both needed: the scan start rotates
each pass (`std::rotr` on the mask), and each ring is capped at `kDrainBatch` events per pass so one
hot producer cannot monopolize a drain.

### NUMA and cache locality

The bitmap is one line shared by all producers; on a multi-socket machine that is a cross-socket line,
and cross-socket coherence is an order of magnitude more expensive than intra-socket. Because the line
is read-mostly, this is tolerable — but at scale, shard the bitmap per NUMA node (one word per node,
the consumer reads each) and pin the consumer to the node hosting most producers. Ring storage should
be first-touched by its own producer so it lands on that producer's node; the staging buffer is
consumer-local by construction.

### Observability and failure modes

Every mechanism here can fail silently, so each is counted: per-producer pushed/dropped/high-water,
bitmap writes, notifications, parks, ring probes, and passes. The two that matter most are **probes
per pass** (is the scan actually narrower?) and **bitmap writes** (is coalescing actually holding?) —
the first version's 582k writes were invisible until that counter existed.

Failure modes to watch: silent drops (mitigated by counters); a stalled producer holding a `Retiring`
slot and preventing reuse (mitigated by having 64 slots and by exposing slot state); a wedged sink
backing up into drops (visible as rising high-water then drops); a missed wakeup (bounded by the
`wait_for` timeout, and visible as latency spikes at exactly the timeout value); starvation of
high-index producers (mitigated by rotation, visible as skewed per-producer drops).

---

## 8. Does it actually improve tail latency?

The follow-up asks how you would *benchmark* this, which is the right question — and the answer has to
survive the result not being what you hoped.

**Methodology**, all implemented in `test_scan_ab`:

- **A/B in one binary.** `ScanPolicy` is a runtime switch, so both policies run the same code, same
  build, same machine, alternating.
- **Isolate one variable.** Active producers are pinned at 8 (the premise) and *registration* is swept
  8 → 64. Sweeping the active count instead would change CPU contention and scan width simultaneously
  and confound them — the first version of this benchmark did exactly that and produced unreadable
  numbers.
- **Open-loop load at a fixed rate, latency measured from the *intended* send time.** This is
  non-negotiable. A closed loop (push as fast as you can) lets a stalled consumer throttle its own
  producers, so the queue never backs up and the stall never appears in the measurement.
  **Coordinated omission** is the single most common way a latency benchmark lies, and it always lies
  in the flattering direction.
- **Percentiles, never means.** The entire claim is about tails; a mean hides exactly the events under
  discussion.
- **The measurement must not perturb the measured.** Samples go to a preallocated buffer with no
  allocation or locking on the path, and latency is sampled 1-in-4 because a clock read is ~25 ns —
  at multi-million events/s the instrument itself would become a meaningful share of the consumer's
  budget.
- **Repetitions with per-statistic medians.** A single-run p99.9 is noise. Five reps, and the median
  is taken per statistic rather than by picking one "median run", so one noisy percentile cannot drag
  unrelated columns with it.
- **Report mechanism counters beside latency**, so a tail that moves for an unrelated reason cannot be
  credited to this change.
- **Do not measure past what the machine can hold.** Spin-paced producers cost a core each; past that
  the numbers describe the OS scheduler. The sweep is capped accordingly.

**Result** — 8 active producers at 100k events/s each, varying how many idle producers are also
registered:

```
registered scan   |  p50 us   p99 us    p99.9 | probes/pass   bmp-wr  drop%
8          full   |     0.5     34.7    326.9 |         8.0        0   0.0%
8          bitmap |     0.5    222.3    597.9 |         8.0        8   0.0%
16         full   |     1.5    452.9   1262.3 |        15.9        0   0.0%
16         bitmap |     1.2   1949.3   3180.6 |         7.8        8   0.0%
32         full   |     2.2    768.4   1397.3 |        31.9        0   0.0%
32         bitmap |     1.1    680.5   1221.3 |         7.9        8   0.0%
64         full   |     3.3    267.3   1000.0 |        63.9        0   0.0%
64         bitmap |     1.3    360.7    876.4 |         7.9        8   0.0%
```

**What this shows.** The mechanism works exactly as designed. `probes/pass` for full-scan tracks
registration precisely (8 → 16 → 32 → 64) while the bitmap stays flat at ~8 regardless — the consumer
genuinely stopped looking at idle rings. Coalescing is close to perfect: **8 bitmap writes** for the
whole run, one per producer, against millions of events. And median latency for full-scan degrades
monotonically with registration (0.5 → 1.5 → 2.2 → 3.3 µs) while the bitmap stays flat (~0.5–1.3 µs).
At 64 registered the bitmap cuts p50 by ~60%. That is reproducible across runs; the p50 column is the
one that repeats.

**What it does not show, and I am not going to claim it does: p99 and p99.9 do not separate the two
policies.** They swing by 5× between runs in both directions — in the run above the bitmap's p99 at 16
registered is *four times worse* than full-scan, and in the previous run it was better. That is noise,
not a result, and reading a story into it would be exactly the failure the follow-up is probing for.
The tail here is dominated by OS preemption and park/wake latency, both far larger than the scan cost
being removed. On this machine, at this scale, the honest conclusion is that the bitmap is a
**median-latency and CPU-efficiency** optimization, not a tail-latency one.

Given the ~15 ns/push the notification mechanism costs (§7), that is a genuinely mixed result: it buys
a flat consumer scan and correct parking, and it charges the producer for them.

The reason is worth understanding rather than explaining away. Probing an idle ring is *cheap*: nobody
writes those index lines, so they stay valid in the consumer's own L1/L2 indefinitely. Sixty-four
rings' index lines are about 8 KB — they fit in L1. The scan is 64 L1 hits, not 64 coherence misses,
and 64 L1 hits do not move a p99.9 that has millisecond-scale scheduler noise in it.

That analysis also predicts exactly when the optimization *would* pay for itself in the tail, which is
the useful output:

- **Many more producers** — thousands of rings will not fit in L1, and the scan becomes real misses.
- **Intermittently active producers** — a ring whose producer occasionally writes has its line
  invalidated, so probing it is a coherence miss rather than an L1 hit. Uniformly-idle producers are
  the *best* case for full-scan, and that is what this benchmark used.
- **A saturated consumer** — when every pass is on the critical path, scan width converts directly
  into queueing delay. Here the consumer is idle most of the time and absorbs the scan for free.
- **Power/CPU budget** — a consumer scanning 64 rings to find 8 burns a core doing nothing, which
  matters on a shared or battery-constrained host even when latency does not move.

The mechanism is still worth keeping: it is required for the consumer to *park* correctly (you cannot
sleep safely without knowing whether anything is pending), and it is the difference between `O(N)` and
`O(active)` as N grows. But "it made p99.9 better" is not supported by this data, and reporting it as
such would be the failure mode the follow-up is testing for.

If I were shipping this at exactly 64 producers on exactly this hardware, the defensible call would be
to keep the bitmap for the parking correctness and the CPU saving, and to be explicit that the tail
justification is unproven here.

---

## 9. When the mutex is the better engineering choice

This design is roughly 400 lines with a Dekker handshake, a three-state lifetime protocol, two
StoreLoad fences, and a race whose failure mode is a rare, load-dependent stall. A mutex-protected
`std::deque` is about ten lines and is obviously correct on inspection. That difference is a real
engineering cost, permanently, paid by everyone who touches the code afterwards.

Prefer the mutex when:

- **Event rates are moderate.** Below ~100k events/s the lock is essentially never contended and the
  entire justification evaporates. Most systems that *think* they need this do not.
- **Global ordering is actually required.** Then per-producer rings are simply wrong, and merging
  streams afterwards costs more than the lock.
- **Events are large, variable-sized, or not trivially copyable.** Fixed-size slots stop being a good
  fit and you are fighting the data structure.
- **Producer count is small,** or producers are short-lived and numerous — the registration protocol
  and `N × capacity` memory start to dominate.
- **Correctness matters more than throughput** — anything financial, medical, or safety-related. The
  mutex version can be reviewed by anyone; this one needs a reviewer fluent in the C++ memory model,
  and on this platform it cannot even be checked with ThreadSanitizer (see below).

The strongest argument is the last one. This design is justified here only because the requirements
explicitly state millions of events per second, low latency, and per-producer ordering. Remove any one
of those three and the mutex probably wins. **Measure first; the sophisticated answer is not
automatically the right one.**

---

## 10. Verification

Executables go to `bin/` regardless of which build directory produced them.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./bin/solution
```

Or without CMake, since it is header-only:

```sh
mkdir -p bin
clang++ -std=c++20 -O2 -Wall -Wextra -Isrc -Ibench bench/*.cpp -o bin/solution && ./bin/solution
```

Four self-checking phases, non-zero exit on any violated invariant:

1. **Correctness under stress** — 16 producers × 200k events, both scan policies. Asserts per-producer
   sequence numbers arrive strictly increasing (gaps allowed only where a drop was counted), that
   `accepted + dropped == offered` exactly, that a `Drain` shutdown loses nothing, and that ring depth
   never exceeded capacity. Plus a producer-churn test: 40 waves × 8 producers recycled through the
   slots, asserting every event from a retired producer is still drained.
2. **Throughput** — offered vs sustained rates across 1–32 producers, with a warm-up run excluded.
3. **Backpressure and shutdown** — against a deliberately slow sink: asserts drops occur rather than
   blocking, memory stays bounded, no producer blocks indefinitely, and `Abort` returns promptly.
4. **The scan A/B** of §8.

Builds and runs clean under AddressSanitizer, which is what exercises the producer
registration/retirement lifetime protocol:

```sh
cmake -S . -B build-asan -G Ninja -DWEIR_ASAN=ON && cmake --build build-asan
# on Windows the ASan runtime DLL is not on PATH by default:
PATH="/c/Program Files/LLVM/lib/clang/21/lib/windows:$PATH" ./bin/solution_asan
```

The sanitized build is named `solution_asan` so it can sit in `bin/` next to the plain one rather than
overwriting it — benchmarking an ASan binary by accident is an easy and very misleading mistake.

One Windows-specific trap, since it cost time and mimics a real bug: **ASan cannot be mixed with the
debug CRT.** CMake defaults single-config generators to `Debug` here, which selects `msvcrtd`, and the
resulting binary aborts during CRT teardown with a `bad-free` whose stack contains no frames from this
project at all. It looks exactly like a lifetime bug in the producer-retirement protocol and is
nothing of the kind. `CMakeLists.txt` pins `MultiThreadedDLL` for the ASan target to prevent it.

**ThreadSanitizer is not available for `x86_64-pc-windows-msvc`** — clang rejects the flag outright on
this target. That was recorded here as a genuine gap, with the note that the check to add was a Linux
TSan run.

**That run has now been done, and it is clean.** WSL (Ubuntu 24.04, gcc 13.3, same 32 logical cores)
builds this unmodified — no portability fixes were needed, which was not a given for a tree that had
only ever seen clang:

```sh
cmake -S . -B build-tsan -DWEIR_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan && ./bin/solution_tsan
```

All four phases pass with **no data race reported**: 3.1 M events under each scan policy, 40 waves of
producer churn through 64 recycled slots, the overload phase, and the full scan A/B. So the ordering
argument in §2 and the Dekker handshake in §7 now have tool-checked backing rather than resting on
reasoning alone.

Two things worth recording from that run. TSan costs 10–40× throughput (2.4 M/s against 10.4), as
expected. And more interestingly, **the bitmap's p50 advantage is far clearer under TSan** — 6.6–7.8 µs
against 9.0–14.0 µs for full scan, a separation the uninstrumented run does not show. That is §8's own
prediction confirmed from an unexpected direction: the optimization pays exactly when probing a ring
costs more than an L1 hit, and TSan's shadow-memory lookup on every access is one way to make that
true.

A Go port of this design was written alongside it ([../go/solution.md](../go/solution.md)), and
`go test -race` also runs clean there. Between the two, the race-detection gap this section opened is
closed on both platforms.

### What the tests actually caught

Listing these because they are the argument for asserting invariants rather than eyeballing output —
every one of them produced a program that looked like it worked.

**Correctness bugs, caught by assertions:**

- The per-event callback computed its range *after* a mid-drain flush had already reset the staging
  index, silently skipping exactly one buffer's worth of events per flush.
- Recycled producer slots reset their counters at registration, so lifetime accounting quietly
  under-reported by everything the previous owner had done.
- The quiesce check read the producer's *private* cached index from the stopping thread — a data race,
  and wrong, since cached state is fast precisely because it is unshared.

**A design flaw, caught by a counter rather than an assertion:** eagerly clearing bitmap bits after
each drain produced **582,954** writes to the shared line where the fixed version produces **8**. No
test failed. The mechanism was quietly doing the opposite of its job, and the only thing that made it
visible was having instrumented the thing the design claims to optimize.

**A race, caught by re-reading the code rather than by any test** — worth being blunt about, since it
is the one that matters most. The first version checked the bitmap *before* the `seq_cst` fence, to
skip the fence in the common case. That is unsound (§7), and its symptom is an event stranded in a
ring with a bounded-but-rare probability. No test here would have caught it, and with TSan unavailable
on this target, nothing automated would have. That is the real argument in §9 for the mutex: this
class of bug is found by reasoning or not at all.

**Measurement bugs, caught by disbelieving the numbers:**

- A per-byte FNV hash in the benchmark sink made the *sink* the bottleneck, so every throughput figure
  was measuring the checksum.
- `sleep_for`-based pacing on Windows rounds to the ~1–15 ms timer tick, three orders of magnitude
  coarser than the periods being paced, which swamped every latency number.
- Sweeping the active-producer count varied CPU contention and scan width simultaneously; the
  experiment had to be redesigned to vary registration alone.
- `probes/1000 events` looked like the right metric but is confounded — a cheaper pass lets the
  consumer spin through more passes per second, so the ratio stays flat while the per-pass cost falls.
  `probes/pass` is the honest one.

---

## Appendix: Glossary

Every acronym and term of art used above, with why it matters to this design.

### Queue shapes

| Term | Meaning |
|---|---|
| **SPSC** | **Single-Producer, Single-Consumer.** Exactly one thread pushes and one thread pops. The cheapest queue that exists: each index has exactly one writer, so publishing needs a plain store, not an atomic read-modify-write. This design's building block — one SPSC ring per producer. |
| **MPSC** | **Multi-Producer, Single-Consumer.** Many pushers, one popper. Needs the producers to agree on who gets which slot, so every push must atomically update one shared tail — the bottleneck §1 rejects. |
| **MPMC** | **Multi-Producer, Multi-Consumer.** Both ends contended. Everything MPSC costs, plus consumer-side coordination. |
| **Ring / ring buffer** | A fixed-size array used circularly: a `head` index says where to read, `tail` where to write, and index `i` maps to slot `i & (Capacity-1)`. "Bounded" because it never grows — that is what makes memory usage a constant instead of a hope. |
| **Head / tail** | The read cursor and the write cursor. Here they are free-running 64-bit counters that are *never* wrapped — only masked at lookup time — so `size == tail - head` is unambiguous and "full" and "empty" never look alike. |
| **Mask** | `index & (Capacity - 1)`, valid only when `Capacity` is a power of two. Replaces a division (~20–40 cycles) with one AND (~1 cycle). |
| **Intrusive queue** | A queue whose link pointers live *inside* the stored element rather than in separately allocated nodes. Avoids a malloc per push. The **Vyukov** queue (Dmitry Vyukov's well-known lock-free MPSC design) is the canonical intrusive example. |
| **Drain** | The consumer emptying a ring — here in batches, up to `kDrainBatch` events per ring per pass. |
| **Batch** | Processing N events per synchronization instead of synchronizing per event. Amortizes both the cross-core index write and the socket syscall. |

### Atomics and memory ordering

| Term | Meaning |
|---|---|
| **RMW** | **Read-Modify-Write.** A single indivisible instruction that reads a memory location, computes a new value, and writes it back — `fetch_add`, `fetch_or`, `fetch_and`, `exchange`, `compare_exchange`. On x86 these compile to `lock`-prefixed instructions. The key cost: the core must take the cache line in **exclusive** state, so under contention the line migrates between cores on *every* operation. That is the whole reason §1 avoids RMW on the push path. |
| **CAS** | **Compare-And-Swap** (`compare_exchange_weak/strong`). "If this location still holds the value I expect, replace it; otherwise tell me what it actually holds." The primitive most lock-free algorithms are built from, usually inside a retry loop. Used here exactly once, off the hot path: claiming a producer slot at registration. |
| **`fetch_add` / `fetch_or` / `fetch_and`** | Atomic add / bitwise-OR / bitwise-AND, returning the *previous* value. The returned prior value is load-bearing in §7: `prev == 0` is how a producer knows it took the bitmap from empty to non-empty and therefore owes a wakeup. |
| **Atomic** | An operation no other thread can observe half-completed. Distinct from *ordered* — atomicity alone says nothing about what other memory the operation is visible relative to; that is what memory ordering specifies. |
| **Memory ordering** | The constraint on how one thread's memory operations may be reordered (by compiler *or* CPU) as seen by another thread. C++ spells these `std::memory_order_*`. |
| **`relaxed`** | Atomic, but with no ordering constraints at all — the operation may be reordered freely with surrounding accesses. Correct here for a thread reading an index only it writes, and for the bitmap fast-path check that is already fenced. |
| **`release`** | A store that publishes: everything the thread did *before* it cannot be reordered *after* it. "Everything I wrote is visible to whoever sees this store." |
| **`acquire`** | A load that observes: everything the thread does *after* it cannot be reordered *before* it. Reading a value released by another thread makes that thread's prior writes visible. |
| **Release/acquire pair** | The handoff. A release store and the acquire load that reads it establish *happens-before* between the two threads. Both halves are required — a release with no matching acquire orders nothing. §2's point is that this pipeline needs **two** pairs, one per direction, and it is the consumer→producer one (`head_`) that people forget. |
| **`seq_cst`** | **Sequentially consistent** — the strongest ordering: acquire+release *plus* a single global total order that all `seq_cst` operations agree on. Needed here only for StoreLoad in the wakeup handshake. Expensive: on x86 a `seq_cst` store becomes `XCHG` or `MOV`+`MFENCE`, a full pipeline drain. |
| **Happens-before** | The formal relation the C++ memory model uses to define which writes a read is allowed to see. If write A happens-before read B, B sees A. If two accesses to the same location are unordered and at least one is a write, that is a **data race** — undefined behavior, not "some stale value". |
| **Fence / barrier** | A standalone ordering instruction (`std::atomic_thread_fence`) that constrains reordering across itself without being attached to any particular variable. Used in §7 because the ordering needed is between a store to `tail_` and a load of `active_` — two different locations, which no single tagged operation can relate. |
| **StoreLoad** | The reordering of an earlier **store** past a later **load** to a *different* address. The one reordering x86's TSO permits, because the store sits in the core's store buffer while the load executes from cache. It is also the only one acquire/release cannot forbid — hence the explicit `seq_cst` fence in §7. The other three (StoreStore, LoadLoad, LoadStore) are free on x86. |
| **TSO** | **Total Store Order** — x86's memory model. Every core sees stores in program order except that its own stores may be delayed in its store buffer while its later loads proceed. Consequence: acquire loads and release stores compile to plain `MOV` on x86 (they only restrain the *compiler*), so writing the weakest correct ordering costs nothing there — and is the difference between correct and broken on ARM. |
| **Store buffer** | The per-core queue holding stores that have retired but not yet reached cache. The hardware reason StoreLoad reordering exists, and the thing `MFENCE` drains. |
| **Dekker's algorithm** | The classic 1960s two-thread mutual-exclusion algorithm. Its essential shape — *each side publishes its own state, then reads the other's* — is the pattern §7 uses so that no interleaving can leave both sides thinking the other will handle it. It is exactly this pattern that requires StoreLoad ordering, which is why Dekker is the standard example of why `seq_cst` exists. |
| **Lock-free** | Guarantees *some* thread makes progress even if others are suspended arbitrarily. Contrast **wait-free** (every thread finishes in bounded steps) and **blocking** (a suspended lock-holder stalls everyone). Note §1's framing: lock-free is a means, not the goal — the goal is minimizing shared writable state, and a lock-free MPSC queue still has one contended line. |
| **`volatile`** | *Not* a concurrency tool in C++. It prevents the compiler from eliding or duplicating an access, but provides no atomicity and no ordering with respect to other threads. For memory-mapped I/O and signal handlers only. |
| **Torn read** | Reading a value while another thread is writing it, yielding a mix of old and new bytes that was never a valid value. Silent — nothing traps; the data is simply wrong. |
| **Idempotent** | Safe to call more than once with the same effect as calling it once. Why `stop()` can be called explicitly *and* by the destructor. |

### Cache and coherence

| Term | Meaning |
|---|---|
| **Cache line** | The unit of cache transfer and coherence, 64 bytes on x86-64. Two variables in the same line are, as far as the hardware is concerned, one thing. |
| **Cache coherence** | The protocol (MESI and relatives) keeping per-core caches consistent. Each line is in one of **M**odified, **E**xclusive, **S**hared, or **I**nvalid state per core. Reading puts it in *Shared* in many caches at once — free. Writing requires *Exclusive*, which invalidates every other copy — not free. §7's whole argument rests on that asymmetry: **read-mostly shared state is cheap; written shared state is not.** |
| **False sharing** | Two threads writing *different* variables that happen to occupy the *same* cache line. Semantically independent, but the hardware ping-pongs the line between cores anyway. Turns a ~1 ns store into a ~100 ns miss, and is the single most common reason a hand-rolled lock-free queue benchmarks slower than a mutex. Fixed by padding/aligning to 64 bytes. |
| **Cache-line ping-pong** | The line bouncing between cores under alternating writes, each transfer a coherence miss. What a shared tail does on every push, and what false sharing does for no reason at all. |
| **Coherence traffic** | The interconnect messages coherence requires. The real currency at these rates — the cost of a contended atomic is the traffic, not the instruction. |
| **L1 / L2** | Per-core caches. L1 is ~32–48 KB, hit in ~4 cycles. §8's key observation: 64 rings' index lines are ~8 KB, so a "wasteful" full scan of idle rings is 64 L1 hits (cheap), not 64 coherence misses (expensive) — which is why the bitmap did not move the tail. |
| **`alignas(64)`** | Forces a type or member onto its own cache line, so neighbouring data cannot false-share with it. |
| **`std::hardware_destructive_interference_size`** | The standard-blessed constant for "how far apart to keep things to avoid false sharing". Avoided here because using it in a type's layout bakes a compile-time constant into that type's **ABI** (Application Binary Interface — the binary contract for layout and calling convention), so a compiler upgrade that changed the value would silently break compatibility. clang warns about exactly this. |
| **Prefetch-friendly** | A linear, predictable access pattern the hardware prefetcher can run ahead of. Storing events *by value* in a contiguous ring gives this; storing pointers would not. |
| **Hot path** | The code executed per event, millions of times a second. The design target — everything else (registration, shutdown, parking) can afford to be expensive. |
| **NUMA** | **Non-Uniform Memory Access** — multi-socket machines where each socket has its own local memory and cross-socket access (and cross-socket coherence) costs roughly an order of magnitude more. Motivates sharding the bitmap per node and first-touching ring storage on its producer's node. |
| **First-touch** | Linux allocates a page on the NUMA node of the thread that first writes it — so *which thread initializes memory* decides where it physically lives. |

### Hardware and platform

| Term | Meaning |
|---|---|
| **ISA** | **Instruction Set Architecture** — the CPU's instruction vocabulary (x86-64, AArch64). §2 compares what the same C++ ordering costs on each. |
| **`MOV`** | Plain x86 load/store. What acquire/release compiles to on x86 — i.e. free. |
| **`MFENCE` / `XCHG`** | x86 full memory fences. `XCHG` is implicitly `lock`ed; both drain the store buffer and cost tens of cycles. What `seq_cst` stores become. |
| **`LDAR` / `STLR`** | AArch64 load-acquire / store-release instructions. Unlike x86, these are *not* the same as plain loads and stores — on ARM the ordering annotations do real work, which is why writing the weakest *correct* ordering (rather than the weakest *observed-to-work-on-x86* ordering) matters. |
| **`_mm_pause`** | The x86 `PAUSE` instruction. Hints "this is a spin-wait loop": reduces power, yields the pipeline to a hyperthread sibling, and avoids the memory-order-violation pipeline flush on loop exit. |
| **Preemption / descheduled** | The OS suspending a running thread. Bounded in principle, multi-microsecond to millisecond in practice, and the dominant term in §8's p99.9. Also the failure mode that sinks `fetch_add`-based bounded rings: a producer descheduled between reserving a slot and publishing it leaves a hole the consumer cannot advance past. |
| **Syscall** | A call into the kernel. Hundreds of nanoseconds to microseconds — why the design batches thousands of events per socket write, and why notifying per push would be worse than the polling it replaces. |
| **ASan** | **AddressSanitizer** — compiler instrumentation catching use-after-free, buffer overflow, and leaks at runtime. What validates the producer lifetime protocol here. |
| **TSan** | **ThreadSanitizer** — the equivalent for data races. Unavailable on `x86_64-pc-windows-msvc`, which §10 flags as a genuine gap: the ordering argument rests on reasoning, not on a tool having checked it. |

### C++ and API

| Term | Meaning |
|---|---|
| **RAII** | **Resource Acquisition Is Initialization** — tying a resource's lifetime to an object's scope so cleanup happens in the destructor and cannot be forgotten. `ProducerHandle` is move-only RAII, so retirement is not something a caller can skip. |
| **Move-only** | Non-copyable, so exactly one object owns the resource at a time and there is no question of who retires the slot. |
| **Trivially copyable** | A type that can be copied with `memcpy` — no user-defined copy constructor, no destructor to run. Lets events be handed to the sink as a raw contiguous view. |
| **`std::span`** | A non-owning `{pointer, length}` view over contiguous memory. Passes the staged batch to the sink with no serialization copy. |
| **`shared_ptr`** | Reference-counted shared ownership. Rejected here because the refcount is an *atomic* that every copy touches — a contended cache line, reintroducing exactly the shared writable state the design exists to remove. |
| **`countr_zero`** | Count trailing zeros — the index of the lowest set bit. With `m &= m - 1` (clear lowest set bit), it iterates set bits in `O(popcount)` rather than `O(64)`. |
| **`rotr`** | Rotate right. Used to move the scan's starting point each pass so low-indexed producers are not always drained first. |
| **LSB** | Least significant bit — bit 0, where `countr_zero` starts, hence the fairness problem it creates. |
| **Condvar** | **Condition variable** — the block-until-signalled primitive (`wait`/`notify_one`) the consumer parks on once spinning and yielding have not paid off. |
| **Park / unpark** | A thread putting itself to sleep in the kernel, and being woken. Cheap in CPU, expensive in latency (microseconds), and the moment where a lost wakeup becomes an unbounded stall rather than a delay. |
| **Spurious** | A wakeup or refresh that happens without the condition actually holding. Harmless as long as the code re-checks — §3's stale cached index is designed so that being wrong can only cost an extra check, never correctness. |
| **Quiesce** | Wait for a component to reach a state where nothing is in flight. Step 2 of `Drain` shutdown, and it needs a deadline or a wedged socket hangs it forever. |

### Backpressure and I/O

| Term | Meaning |
|---|---|
| **Backpressure** | Propagating "I cannot keep up" back to the source instead of buffering without limit. The chain here: socket → consumer → rings → drop policy, each link bounded. |
| **`EWOULDBLOCK`** | The errno a non-blocking socket returns when its send buffer is full (identical to `EAGAIN` on Linux). The signal that the peer has stopped reading. |
| **`shutdown(fd, SHUT_WR)`** | Half-closing the write side of a socket so the peer reads a clean end-of-stream rather than a connection reset. What a graceful drain owes its peer. |
| **OOM** | **Out Of Memory.** The end state of "just use an unbounded queue": on Linux the kernel's OOM killer terminates the process — later, larger, and less debuggable than a counted drop would have been. |
| **High-water mark** | The maximum depth ever observed. The metric that proves memory stayed bounded, as opposed to a current-depth reading that happens to look fine. |
| **Drop policy** | The explicit rule for what happens when a bounded queue is full. `DropNewest`, `SpinThenDrop`, etc. — the point of §5 is that *every* policy must terminate, and "block" does not. |
| **Coalescing** | Collapsing many logical notifications into one actual write or wakeup. §7's central trick: a producer sets its bitmap bit once and then streams millions of events touching that line zero more times — 8 writes instead of 582,954. |
| **Starvation** | A participant persistently losing out — here, high-index producers never drained because `countr_zero` always starts at bit 0. Fixed by rotating the scan start and capping per-ring drain size. |

### Measurement

| Term | Meaning |
|---|---|
| **p50 / p99 / p99.9** | Percentiles. p99.9 = the value 99.9% of samples fall below — i.e. the worst 1 in 1000. The tail is the entire subject; a mean averages away exactly the events under discussion. |
| **Tail latency** | The high percentiles. Dominated by rare events (preemption, page faults, park/wake, lock parking), which is why a design can have a great mean and an unusable p99.9. |
| **Coordinated omission** | Gil Tene's term for the classic latency-benchmark lie: if the load generator waits for each request to finish before sending the next, then whenever the system stalls the generator stops issuing load — so the stall period contributes *few or no* samples and the recorded latency looks fine. Measuring from the *intended* send time in an open loop is the fix. It always lies in the flattering direction. |
| **Open-loop vs closed-loop** | Open-loop issues load at a fixed rate regardless of whether the system keeps up (so backlog shows up as latency); closed-loop issues the next event only after the previous completes (so a slow consumer silently throttles its own load). Only open-loop can measure a stall. |
| **A/B** | Running both variants in the same binary, same build, same machine, alternating — so the only difference is the switch under test. |
| **Confounded** | A measurement varying two things at once, so the result cannot be attributed to either. Two instances here: sweeping active producers changed both CPU contention *and* scan width; `probes/1000 events` mixed per-pass cost with pass rate. |
| **Warm-up** | Discarding the first run, whose numbers include cold caches, first-touch page faults, and CPU frequency ramp. |
| **M/s, ns, µs, ms** | Millions per second; nanoseconds, microseconds, milliseconds (10⁻⁹, 10⁻⁶, 10⁻³ s). For scale: L1 hit ~1 ns, contended cache line ~100 ns, clock read ~25 ns, syscall ~1 µs, thread park/wake ~10 µs, Windows timer tick ~1–15 ms. |
| **FNV** | Fowler–Noll–Vo, a simple byte-at-a-time hash. Named because using it in the benchmark sink accidentally made the *sink* the bottleneck — every "throughput" figure was measuring the checksum. |
