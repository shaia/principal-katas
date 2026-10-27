# A bounded MPMC work queue, and the slot that is reserved but not yet there

Answer to [../question.md](../question.md) and the [C++ track](question.md), with a working
implementation and benchmarks alongside it. The Go and Python tracks are not written yet; their
questions are at [../go/question.md](../go/question.md) and
[../python/question.md](../python/question.md).

Every number below came from running [`bin/solution.exe`](bench/), whose exit code is the verdict,
on an i9-13980HX laptop (8 P-cores, 16 E-cores, 32 logical CPUs, one socket) under Windows 11 with
clang 21.1.7; the ThreadSanitizer figures came from the same sources built with gcc 13.3 under WSL.
Unless a section says otherwise, figures are from one full run, the second of two back to back. Terms
of art (TSO, EcoQoS, LL/SC, …) are expanded in the [Glossary](#appendix-glossary).

---

## The problem

A security agent's work queue: 32 producer threads, 16 consumer threads, 5 M events/sec sustained, a
fixed 65,536 slots, no allocation on the hot path, producers that must never overwrite unread events,
consumers that sleep when it is empty, producers that may block or reject when it is full, a shutdown
that loses nothing accepted, and a p99 enqueue below 2 µs. The brief supplies a candidate:

```cpp
bool push(T value) {
    auto pos = tail_.fetch_add(1);
    buffer_[pos % N] = std::move(value);
    return true;
}
bool pop(T& result) {
    if (head_.load() == tail_.load()) return false;
    auto pos = head_.fetch_add(1);
    result = std::move(buffer_[pos % N]);
    return true;
}
```

and asks four things: why atomic indices alone are insufficient, how to publish initialized elements
safely, how to avoid lost wakeups, and what happens when a producer is preempted after reserving a
slot — all of which are one question: **how do you tell a reserved slot from a published one?**

### What the brief gives you, and what it withholds

| The brief states | What it forces |
|---|---|
| 32 producers, 16 consumers | 48 threads. On this machine's 32 CPUs, oversubscription — preemption inside a window is not hypothetical (§7) |
| 5 M events/sec | 200 ns between consecutive operations on any one shared counter |
| 65,536 slots | 13.1 ms of traffic at 5 M/s: enough for a burst, nothing for a deficit (§5) |
| consumers sleep when empty | a wake that costs the waker 2.86 µs at the median — more than the whole p99 budget (§6) |
| producers may block *or* reject | two reservation primitives, not one: look-then-claim, and claim-then-look (§5) |
| shutdown loses nothing accepted | a close and a reservation must be one decision, or a reservation must be payable after it (§9) |
| p99 enqueue < 2 µs | a statement about contention on one line and about the consumers — not about the queue alone (§10) |
| — | **no ordering requirement at all**, and with 16 consumers FIFO dequeue is not FIFO processing (§7) |

### How the answer is graded

- **Senior** — overflow, concurrent slot access, consumers reading unpublished data. → §2
- **Strong Senior** — reservation separated from publication, with acquire/release. → §3
- **Staff** — per-slot sequence numbers, bounded capacity, safe blocking and wakeup. → §1, §4, §5, §6
- **Principal** — progress guarantees, preemption, cache contention, fairness, shutdown races, tail
  latency. → §7, §8, §9, §10
- And the line the kata adds: **when a mutex is still the better call.** → §11

Each band is a more complete answer to the central question. The Principal step is the one that
notices the distinction, once representable, has a *duration* — and that every hard problem in the
brief is about how long a slot stays reserved and who is waiting while it does.

### Why it is hard

**The bugs look different and are the same bug.** Overwrite, overtaking, a half-written read: three
symptoms of one counter doing two jobs. Fixing them one at a time produces a patched version of the
wrong design. (→ §2)

**The fix turns a safety bug into a progress bug.** A per-slot sequence stops a consumer reading a
half-built event by making it wait for one. With no lock anywhere, one descheduled producer then stops
every consumer, and that makes the design *blocking* by definition. (→ §7)

**The wakeup costs more than the budget.** Waking a sleeping consumer cost the waker 2.86 µs at the
median on this machine. Any design in which more than one push in a hundred wakes somebody has
already missed the p99. (→ §6)

**The two full policies want different instructions.** Refusing on full needs look-then-claim, which
is a CAS loop that retries 3.7 times per push at 32 producers; blocking can use a ticket that never
retries — and then cannot be refused, cannot time out, and cannot be closed without a tombstone.
(→ §5, §9)

**And the platform lies, twice.** Windows throttled the unfocused benchmark to a quarter of its
throughput, and the machine's power profile drops its clock under sustained load. Neither shows in a
single-thread pinned measurement. (→ §12)

### Where each part is answered

| Question | Section |
|---|---|
| What the proposed queue gets wrong, measured | §2 |
| Why atomic indices are insufficient; the two release/acquire pairs | §3 |
| The slot, its object, its lifetime, its layout | §4 |
| Block or reject; capacity in milliseconds; drops as an attack surface | §5 |
| Sleeping without losing a wakeup; what a wake costs | §6 |
| A producer preempted after reserving; the progress guarantee | §7 |
| Cache lines, retries, fairness | §8 |
| Graceful shutdown | §9 |
| Does it meet the brief? | §10 |
| When to ship the mutex | §11 |
| Verification, and what this platform cannot measure | §12 |

## Layout

``` text
cpp/
  src/    platform (TSC clock, timed wait-on-address, EcoQoS), the event, the brief's queue verbatim,
          the event count, turnstile::Queue, the mutex arm
  bench/  nine phases, one per translation unit, plus --bug  -> bin/solution.exe
```

Run it:

```sh
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe"
cmake --build cpp/build
cpp/bin/solution.exe                      # exit code is the verdict; about five minutes; run twice, see §12
cpp/bin/solution.exe --bug no-fence       # one row of the bug x tool table (§12)
```

---

## 1. The decision

**Put reservation and publication in different places.** Reservation lives in a shared counter;
publication lives in the slot, as a sequence number stamped with the lap:

``` text
slot i, lap k, position p = k·N + i

  seq == p          free for the producer that reserves p
  seq == p + 1      published: the consumer that claims p may read it
  seq == p + N      released: free for the producer of p + N, one lap on
```

"Reserved but not yet published" is not a value of `seq` at all — it is `seq == p` while the tail
has passed `p`, visible only by reading both. That is the whole answer to the central question, and
most of the rest of this document is about what follows from it.

The implementation is [`turnstile::Queue`](src/mpmc_queue.hpp):

- **Two entry points on one tail word.** `try_push` reserves by compare-and-swap after checking the
  slot is free, so it can refuse a full queue before claiming anything. `push` reserves with a
  `fetch_add` ticket that never fails and never retries, and then waits for its slot. Both claim
  positions from the same counter, so they mix freely: kernel-fed producers that must never block use
  the first, and everything else uses the second.
- **A closed bit in that word**, so a CAS reservation and a close are one atomic decision. A ticket
  cannot be refused, so a ticket that lands after the close is paid with a **tombstone** — a
  publication with no object behind it — which consumers claim, skip and release.
- **Raw storage**, `std::construct_at` and `std::destroy_at`, so that the sequence certifies an
  object exists; `T` must be nothrow-movable, because nothing between reserve and publish may throw.
- **An event count** for sleeping — a waiter count, a generation, and at most one wake in flight —
  with a fence on each side of the handshake, and wakes to blocked producers sent once per 64
  releases rather than on every one.
- **Padded slots**, one per cache line.

### Why not the alternatives

**A single "published up to here" counter** would need producers to publish in order, so a fast
producer waits behind a slow one — and a descheduled one stalls every producer behind it as well as
every consumer. Per-slot state is the alternative that does not serialize the publish.

**A null-means-empty pointer per slot** cannot tell a slot that is free for this lap from one that a
producer of the *previous* lap has reserved and not yet written; when the ring wraps while a slot is
held, two producers write it. The sequence's lap stamp is the information the flag lacks.

**A lock-free bounded queue** (Nikolaev's SCQ is the usual reference) exists, and works by letting a
consumer take a slot away from a producer that was too slow — so a reservation stops being a promise
and a push can fail after reserving. §7 measures how often the case it fixes actually occurs here:
six times in eight million pushes.

**A mutex and two condition variables** makes the distinction unobservable rather than
representable, and answers the wakeup and shutdown questions by construction. §11 measures it and
says when it wins.

---

## 2. The proposed queue, broken

Phase 1 runs the brief's code verbatim. The races are undefined behaviour; with a trivially copyable
32-byte `Event` the compiled code does plain loads and stores, so this is the hardware doing what the
source said.

**One producer, no consumer.** Nine pushes into eight slots, then nine pops, return
`8 1 2 3 4 5 6 7 8`. `push` never checks for room: event 0 is overwritten by event 8, which is then
delivered twice. This is deterministic — the failure that needs no concurrency at all.

**Four producers, four consumers, 1,024 slots, 800,000 pushes:**

| outcome | events |
|---|---|
| lost — never delivered | 91,222 (11.4 %) |
| delivered more than once | 90,192 |
| read torn, or never written | 1,030 |

The three kinds are the three bugs:

- **Capacity.** Nothing stops a producer lapping the consumers.
- **Check-then-act in `pop`.** Two consumers both see `head_ != tail_` with one element present, and
  both increment `head_`. After that `head_` has passed `tail_`, and every `pop` succeeds on a slot
  no producer has written. The position each such pop consumes is also a position whose real event,
  when a producer later writes it, nobody will read — which is where most of the losses come from.
- **Announcement before contents.** `tail_.fetch_add` makes the position visible as available
  before `buffer_[pos]` is written. The 1,030 torn reads are consumers that believed it.

The first two kinds yield to more conditions on the counters. The third does not: with 32 producers
finishing in a different order from the one they started in, no pair of counters can say which
positions between them hold finished events. That is why atomic indices alone are insufficient — not
because they are not atomic enough, but because there are two of them and the state they would have
to describe has a value per slot.

The two "overflows" the rubric names, priced: the 64-bit counters wrap after 117,000 years at 5 M/s,
which is not a bug; the brief's capacity is exceeded after 65,536 unconsumed pushes, which is. Narrow
the counters to 32 bits and they wrap after 14.3 minutes — harmless here, because every comparison in
the redesign is a modular difference and 65,536 divides 2³².

---

## 3. Reservation, publication, and the two pairs

The happens-before edge from the producer's construction of an event to the consumer's read of it
must run through a store made **after** construction. The proposed code's only shared store,
`tail_.fetch_add`, comes **before** construction, and making it `seq_cst` (as the default already
does) changes nothing: ordering constrains what other threads may observe, not what this thread has
done yet.

So the edge is the slot's sequence:

```cpp
std::construct_at(slot_ptr(pos), std::move(v));
s.seq.store(pos + 1, std::memory_order_release);         // producer: publish
...
if (s.seq.load(std::memory_order_acquire) == pos + 1)    // consumer: observe, then read
```

and a second pair runs the other way, which is the one that gets forgotten: the consumer's
`seq.store(pos + N, release)` after it has read and destroyed the object, against the next lap's
producer's `seq.load(acquire) == pos + N` before it constructs. Without it, the next producer may
construct into storage this lap's consumer is still reading.

**The reservation is `relaxed`.** No thread reads anything on account of having observed a value of
`tail_`; the slot carries the data. The same holds for the head CAS. Both are a single
read-modify-write on their own line and order nothing else.

**The difference is computed unsigned, then converted:** `static_cast<std::int64_t>(seq - pos)`.
Unsigned subtraction is defined modulo 2⁶⁴ and the conversion has been modular since C++20; signed
subtraction that overflows is not defined at all. That spelling is also what makes a 32-bit counter
survive its wrap.

**Neither side reads the other side's counter** to decide full or empty: a producer's "full" is a
slot still holding the previous lap, a consumer's "empty" is a slot not yet holding this one. So the
tail's line is written only by producers and the head's only by consumers, and the line that has to
move between them is the slot's, with the event in it. A consumer reads the tail only on its way to
sleep, to tell a hole from an empty queue and to see a close (§7, §9).

Whether the orderings are right is not something a test on x86 can establish — TSO hides both
relaxed-sequence bugs completely. §12 has the table.

---

## 4. The slot and its object

```cpp
struct alignas(64) Slot {
    std::atomic<std::uint64_t> seq;
    alignas(T) std::byte       storage[sizeof(T)];
};
```

The brief's `std::array<T, N>` with move-assignment has every slot hold a live `T` from construction
to destruction, so "published" stops meaning "an object is here" and comes to mean "the object here
has a new value". That costs a default constructor, 65,536 constructions up front, a moved-from
residue in every consumed slot, and an assignment that runs with whatever the slot held. Raw storage
makes the sequence certify lifetime instead: a live `T` exists exactly when `seq == pos + 1`, which is
how the destructor knows what to destroy, and why a tombstone (§9) carries a separate bit.

**Nothing in the window may throw** — a reservation is a debt, and an exception defaults on it,
leaving a position no consumer will ever get past. `T` is required to be nothrow move-constructible,
move-assignable and destructible, and the event is built by the caller before `push` is called. That
is also the argument against an `emplace`: it saves one move and runs arbitrary user code — which may
allocate, lock or throw — while a slot is held.

**The ring prefaults itself.** No slot's initial sequence is zero, so the constructor writes all
65,536 of them, which faults in every page of the ring on the constructing thread — no producer takes
a first-touch page fault inside its window. The design cannot avoid this, and it is the right thing
for it to do.

**Padding wins.** One slot per 64-byte line against 40-byte slots packed, saturated at 32 × 16,
median of five, interleaved:

| layout | footprint | throughput |
|---|---|---|
| padded, 64 B per slot | 4.0 MiB | **4.41 M/s** |
| packed, 40 B per slot | 2.5 MiB | 2.95 M/s |

49 % for 1.5 MiB. Packed, a producer publishing one slot and a consumer releasing its neighbour write
the same line.

---

## 5. Full: two policies, two primitives

**Capacity is a duration.** 65,536 slots at 5 M/s is 13.1 ms. Measured with no consumers at all, at
1 M/s so that the producers can actually keep pace (§12): exactly **65,536** accepted and every later
one refused — "never overwrite" as a count — and the first refusal **69.2 ms** after the start, against
65.5 ms by division.

So the queue absorbs bursts and never deficits. With consumers that top out at 1.6 M/s against
2 M/s offered:

| policy | refused | consumed | push p99 |
|---|---|---|---|
| reject on full | 20.1 % | 1.53 M/s | 9.74 µs |
| block on full | 0 | 1.33 M/s | 196 µs |

The refused fraction is the deficit, 20 %, to within a tenth of a percent. Blocking turns refusals
into waiting, and the p99 becomes a statement about the consumers — two orders of magnitude over the
budget, on a queue that has done nothing wrong. That is the sense in which the brief's p99 is not
only a property of the queue.

**The policy picks the instruction.** Refusing on full has to look before it claims, and
look-then-claim on a shared counter is a CAS loop in which every loser retries. Blocking can claim
first with `fetch_add`, which on x86 is one `LOCK XADD` that never fails. At 32 × 16 saturated:

| reservation | retries per push | throughput |
|---|---|---|
| CAS (look, then claim) | 3.67 | 4.80 M/s |
| ticket (claim, then look) | 0 | 5.30 M/s |

And at the brief's rate, in §10, the ticket's p99 is a tenth of the CAS's. The ticket's price is that
the claim cannot be taken back: a ticket holder cannot refuse, cannot honour a deadline, and cannot
be stopped by a close — which is why both entry points exist, on one word, and why a ticket after a
close is paid with a tombstone.

**For a security agent, a drop is a detection gap an attacker can schedule.** Cheap, benign events
can be generated to fill the queue at the moment something worth hiding happens. Rejection is
therefore never silent here: `accepted + rejected + refused == offered` is asserted on every run, and
a production version would count drops per source and alert on them. And blocking does not remove the
drop for a producer fed by a kernel buffer — it moves it upstream, into a buffer the agent may not be
counting. The Linux audit subsystem exposes exactly that choice as configuration (a backlog limit and
a wait time that blocks the *audited* process), which is the strongest argument that the choice is
the agent's to make per source, not the queue's.

---

## 6. Sleeping, and the wake that costs more than the budget

**What a wake costs**, from phase 0:

| | cost |
|---|---|
| a fence and a load of a shared line — the event count with nobody asleep | 4 ns |
| `WakeByAddressSingle` with nobody blocked | 6 ns |
| **the waker, when somebody is asleep** | **p50 2.86 µs, p99 18.6 µs** |
| until the woken thread runs | p50 22.0 µs, p99 135 µs |

A push that wakes somebody exceeds the 2 µs budget at the median. So the design question is not how
to wake but how rarely.

**The protocol** ([`src/eventcount.hpp`](src/eventcount.hpp)): the sleeper reads a generation,
announces itself in a waiter count, fences, re-checks the queue, and only then blocks while the
generation is unchanged. The producer publishes, fences, reads the waiter count, and only if it is
non-zero advances the generation and wakes. Each side stores, then loads the other's location — the
store-buffering shape, which is the one reordering x86 performs, and which release/acquire cannot
forbid. Whichever fence comes first in the single total order, either the re-check sees the event or
the producer sees the sleeper. The generation is a counter rather than a flag, because a wait that
compares a value can miss a flag that flips back.

**One wake in flight.** A woken thread takes tens of microseconds to run, and until it does it still
counts as a sleeper, so every push in that window would wake again. A `pending` flag lets the first
producer's wake stand for all of them; every thread entering or leaving the wait clears it and fences
before re-checking, so a push that skipped its wake is seen by whoever cleared the flag after it. The
first version of the event count did not have it. Both, at 32 × 16 with consumers asleep most of the
time, interleaved, median of three:

| rate | wakes | wakes per push | push p99 | end-to-end p99 |
|---|---|---|---|---|
| 20 k/s | on every push | 1.00 | 57.6 µs | 57.6 µs |
| 20 k/s | one in flight | 0.021 | **2.43 µs** | 163 µs |
| 200 k/s | on every push | 1.00 | 2.43 µs | 230 µs |
| 200 k/s | one in flight | 0.006 | 2.12 µs | 461 µs |
| 1 M/s | on every push | 0.41 | 8.04 µs | 1.84 ms |
| 1 M/s | one in flight | 0.007 | 8.47 µs | 1.03 ms |

**This is a trade, not a win**, and the table is clearer than the expectation was. Coalescing takes
the producer's p99 at low load from 57.6 µs to 2.43 µs — which is the brief's number — and costs the
*consumer* side: at low and moderate rates, events wait longer to be picked up, because fewer
consumers are woken for them. A consumer that wakes and finds more work behind its first event wakes
the next one itself, which caps the producer's cost at one wake per idle-to-busy transition; that
chain is why the end-to-end cost is a factor of two or three and not worse.

**Waking blocked producers has the same economics, inverted.** A producer blocked on full can use any
free slot, so waking it on every release makes every pop pay for a wake. Under a 0.4 M/s deficit,
interleaved, median of three:

| consumers wake a producer | consumed | consumer-issued wakes | push p99 |
|---|---|---|---|
| on every release | 0.98 M/s | 1,167,247 | 271 µs |
| once per 64 releases | **1.31 M/s** | 21,709 | 196 µs |

54 times fewer wakes and 34 % more throughput. Batching can delay a blocked producer but cannot lose
it — the backstop and a producer-side chain wake bound the delay — and the independent review of this
code (§12) found that bound to be "the next batch boundary or the 50 ms backstop", which the code now
says. Ticket holders are the exception: each needs its *own* slot, so one arbitrary wake usually
reaches the wrong one, and they are woken all at once per batch instead. The first version woke them
one at a time and, on a four-slot ring, left most of them to a millisecond timer: during development
phase 2 took eight minutes, where it now takes half a minute.

**Whether the fence matters is measurable.** Phase 5 engineers the race: a hook announces that the
consumer is about to sleep, both sides pause a random few nanoseconds, the producer pushes, and the
consumer either sees it on re-check, sleeps and is woken, or sleeps through it until a 2 ms backstop.
Eighty thousand iterations each, sweeping the consumer's pause so that the arrivals overlap:

| | lost wakeups |
|---|---|
| fenced — the real protocol | **0** |
| the producer's fence removed | **14** |

Rare, and real, and on x86 — the C++ track predicted an x86 stress test could catch this one because
store-to-load is the reordering x86 actually performs, and it did. ThreadSanitizer cannot (§12).

---

## 7. Preemption: blocking, with no lock in it

**How long is the window?** From winning the reservation to storing the publication, at 32 × 16
saturated, eight million pushes: **p50 10 ns, p99 15 ns**, p99.99 172 ns. And six of the eight million
took more than a millisecond, the longest 11.6 ms — a producer descheduled inside its window, with
nothing injected, because 48 threads are sharing 32 CPUs.

**What one does to everyone else**, injected: a 100 ms stall at position 200,000, at 1 M/s.

| | producer stalled | consumer stalled |
|---|---|---|
| first refusal after the stall began | 63.5 ms | 62.9 ms |
| N / rate | 65.5 ms | 65.5 ms |
| occupancy at the first refusal | 59,563 | **833** |
| end-to-end p99 | 111 ms | 1.08 ms |

A stalled **producer** stops every consumer at once: they reach the hole and wait, the queue reports
empty while it fills behind them, and once the ring has come round to the hole — N / rate later —
producers are refused. At the brief's 5 M/s that is 13.1 ms, about one scheduler timeslice.

A stalled **consumer** costs nothing at first. The other fifteen carry on and the queue stays nearly
empty — until the producers have gone once round and reach the stalled slot, which still holds the
previous lap. Then the queue reports full **with 833 events in it**. The asymmetry is the shape of
the steady state: consumers are right behind producers, so a producer's hole is felt immediately and a
consumer's only a lap later.

**The progress guarantee is blocking.** Lock-free means that some thread always completes an
operation in a finite number of steps, whatever the others do. Here a suspended thread prevents every
consumer from completing a pop, and with no lock anywhere in the code. "It has no mutex" is not the
definition. What the design does instead is bound the window: a nothrow move and one store, no page
fault (the ring prefaults itself), no user code — and measure how often the bound is broken by the
scheduler, which is six times in eight million.

**Nothing in the brief asks for FIFO.** The hole stalls consumers because they take slots in order —
and with sixteen consumers processing concurrently, the order in which events are *taken* is not the
order in which they *finish*. Anything downstream that needs per-entity order (exec before connect
before exit, for one process) cannot get it from this queue at all, FIFO or not; it needs routing by
key. Not measured here, and worth saying plainly: the property the hole exists to protect is one
nobody downstream can observe.

---

## 8. Cache lines, contention, fairness

**The unit cost.** Phase 0 hammers one line with `fetch_add`:

| threads | per op, per thread | aggregate |
|---|---|---|
| 1 | 4 ns | 235 M/s |
| 8 | 86 ns | 92.7 M/s |
| 32 | 481 ns | 66.6 M/s |

The aggregate leaves thirteen times the brief's 5 M/s on one line — but one thread's single
operation at 32-way contention takes 481 ns, a quarter of the p99 budget, before the CAS loop multiplies
it. One line transfer between two cores costs 97 ns here; across sockets it would cost more, and this
machine has one socket.

**Retries, as producers are added**, saturated, consumers = producers / 2, interleaved:

| producers | CAS | retries/push | CAS + backoff | retries/push | ticket | retries |
|---|---|---|---|---|---|---|
| 1 | 12.8 M/s | 0 | 13.2 M/s | 0 | 11.4 M/s | 0 |
| 4 | 9.34 M/s | 1.06 | 9.28 M/s | 0.57 | **16.8 M/s** | 0 |
| 32 | 4.47 M/s | 3.56 | 3.48 M/s | 3.94 | 5.61 M/s | 0 |

**Backoff did not help.** It is the textbook lever for a contended CAS, and at 32 producers it cost
22 % of the throughput and raised the p99 (47.4 µs against 30.5 µs) while lowering the median. It
stays in the code as an option, off. The ticket never retries, peaks at four producers, and stays
ahead at every count above one.

**Fairness and tail tolerance pull against each other**, and not the way the folklore says. At
32 × 16 saturated, the spread between the fastest and slowest producer:

| | slowest | fastest | spread | push p99 |
|---|---|---|---|---|
| CAS | 0.13 M/s | 0.17 M/s | 1.28× | 30.5 µs |
| ticket | 0.18 M/s | 0.31 M/s | 1.77× | **4.02 µs** |

CAS shares the line more evenly; the ticket has the far better tail. Every push takes one ticket, so
no single push waits long — but a descheduled ticket holder's turn still has to come round, and the
producers that happened to be running get through more of theirs.

---

## 9. Shutdown

**"Accepted" means `push` returned `ok`** — for a CAS reservation, the CAS succeeded with the closed
bit clear; for a ticket, the `fetch_add` returned a word with the closed bit clear. A position
reserved before the close is owed and will be published; the drain waits for it.

**A close and a CAS reservation are one decision**: the closed bit is in the word the CAS operates
on, so any reservation ordered after the close fails. **A ticket cannot be refused**, so a ticket
that lands after the close pays its slot with a tombstone and returns `closed`. Consumers are finished
when the head equals the tail with the closed bit set — a condition on positions, never "empty",
because a hole looks exactly like empty (§7). After a close nobody parks; the remaining work is finite
and every position below the tail is owed by a producer that already holds it.

Phase 8, all under a watchdog:

| shutdown | result |
|---|---|
| close after 300 ms, 32 producers that never stop, consumers too slow for them | CAS: 807,816 accepted, 807,816 consumed, backlog 65,505, drained in 17.7 ms; ticket: 1,136,546 = 1,136,546, drained in 17.7 ms; all 32 producers refused |
| close while a producer is stalled in its window | stall 21 → 325 ms, close at 154 ms, last event consumed at 333 ms; 165,536 accepted = 165,536 consumed |
| close-versus-reserve, 20 rounds, close at 1 to 20 ms, 8 × 8 on 64 slots | CAS: 20 of 20 clean; ticket and CAS mixed: 20 of 20 clean |

The ticket arm's backlog at close (65,540) is larger than the capacity: tickets taken and waiting for
their slots, positions owed but not yet storable. The drain delivered all of them.

---

## 10. Does it meet the brief?

At the brief's rate, paced, consumers spending 1 µs per event, interleaved, median of three:

| | push p50 | **p99** | p99.9 | producers behind schedule, p99 |
|---|---|---|---|---|
| **32 × 16 at 5 M/s** — CAS | 1.80 µs | 28.8 µs | 88.0 µs | 236 ms |
| 32 × 16 at 5 M/s — ticket | 846 ns | **2.96 µs** | 14.4 µs | 5.63 ms |
| 32 × 16 at 5 M/s — mutex | 1.59 µs | 339 µs | 650 µs | 4.77 s |
| 16 × 8 at 5 M/s — ticket | 502 ns | **1.90 µs** | 7.20 µs | 3.90 ms |
| 32 × 16 at 1 M/s — ticket | 238 ns | **1.22 µs** | 10.6 µs | 379 µs |

"Behind schedule" is from when an event could first have been pushed to when it was accepted — the
coordinated-omission view. At 5 M/s the CAS producers fell a quarter of a second behind; the ticket
producers stayed within six milliseconds.

| requirement | on this machine |
|---|---|
| 5 M events/s sustained | **at the line.** Saturated at 32 × 16: tickets 5.30 M/s; CAS 4.80 M/s in this run and 5.59 M/s in the one before |
| p99 enqueue below 2 µs | **missed at 32 × 16 by 50 %** (tickets, 2.96 µs); **met** at 16 × 8 (1.90 µs) and at 1 M/s (1.22 µs). The CAS reservation misses everywhere, by five to fifteen times |
| never overwrite unread events | met — exactly 65,536 accepted with no consumer (§5) |
| consumers sleep when empty | met, with a wake that costs more than the budget whenever one is needed (§6) |
| graceful shutdown loses nothing | met, under every shutdown phase 8 could construct (§9) |
| no allocation on the hot path | met — the ring and every histogram are allocated before release |

The miss at 32 × 16 is oversubscription: the same code at 16 × 8, which fits in this machine's 32
CPUs, meets it. With 48 threads on 32 CPUs the tail is set by the scheduler, which §7 shows
descheduling producers inside their windows. On a 48-core server the question is open, and the
number to measure first is the one in §7.

---

## 11. When the mutex is the better engineering choice

A ring under one `std::mutex` with two condition variables is the textbook monitor, and it answers
by construction what §§6 and 9 needed protocols for: the check and the sleep happen under the lock the
notifier takes, so no wakeup can be lost, and the closed flag and the push are checked under the same
lock, so no reservation can slip in after a close. Reservation, construction and publication are one
critical section — the state from step 0 still exists, and no other thread can see it.

Measured, at 32 × 16 saturated:

| | throughput |
|---|---|
| mutex, one event per acquisition | 0.66 M/s |
| **mutex, 64 events per acquisition** | **16.7 M/s** |
| turnstile, ticket | 5.30 M/s |

One event per lock is eight times short of the brief. Sixty-four per lock is three times *past* it,
and faster than either lock-free arm, because one acquisition now carries 64 events' worth of
coordination. Its weaknesses are the ones batching cannot touch: per-event latency is a batch's, a
producer with one event in hand cannot batch at all, and a preempted holder stops every producer and
every consumer, not only the consumers behind one slot.

So the mutex is the right call when producers naturally have batches — a reader draining a kernel
ring buffer does — when per-event tail latency is not the requirement, and when the people who will
maintain this would rather review twenty lines of monitor than this document. The per-slot design is
the right call when the requirement is the brief's: single events, a per-push p99, and thread counts
that make one lock the bottleneck. With sixteen consumers, both keep a shared head — and that is not
overhead in either: it is the load balancer.

---

## 12. Verification, and what this platform cannot measure

The exit code asserts, on every run and under shared labels: every offered event accounted for as
accepted, rejected or refused; every accepted event consumed exactly once (two independent hash sums
of identity, plus counts); nothing read torn or unpublished (a digest in every event); and, from each
consumer's point of view, each producer's events in order. Phase 2 runs these against every arm, at
the brief's thread counts and on a four-slot ring that wraps every few operations.

### The bug × tool table

The C++ track's step C5: four deliberate bugs, each one line of the protocol made wrong, compiled in
with `Options::bug` and run with `--bug <name>`.

| bug | x86 stress | ThreadSanitizer | what that means |
|---|---|---|---|
| none (control) | clean | 0 reports | the instruments are not lying in the other direction |
| slot-sequence **load** relaxed | not detected | **4 races** | TSO hides it; the model does not |
| slot-sequence **store** relaxed | not detected | **3 races** | the same |
| **full check** removed | **hangs** (watchdog) | 2 races | consumers spin forever at a slot that skipped a lap |
| **fence** removed | **14 lost wakeups** | 0 reports | the one x86 performs; TSan does not model fences |

ThreadSanitizer reasons about the C++ happens-before relation, not the hardware, so it finds both
relaxed-sequence bugs on x86 where the hardware hides them — the reports name the producer's
`construct_at` and the consumer's read. It does not model standalone fences, and says so: gcc prints
"`atomic_thread_fence` is not supported with `-fsanitize=thread`" at every instantiation, 263 times,
and the TSan build silences that warning with a comment saying why. So the wakeup protocol — the one
place the design depends on a fence — is the one place TSan cannot judge in either direction, and the
x86 stress test, the weakest instrument in the table for everything else, is the one that catches it.

Under TSan the correctness phases (2 and 8) run clean, with event counts divided by twenty.

An independent review of the queue and the event count, done by reading and constructing
interleavings rather than by running anything, found no bug in the protocol and two bounded-latency
risks, both fixed: blocked producers could idle with free slots until the backstop (now bounded by a
producer-side chain wake), and the ticket mode could not honour a close (now paid with tombstones). It
also noted one formal gap that no x86 or AArch64 execution exhibits — a consumer's re-check reading
a stale head — which the code now documents rather than fixes.

### Four measurement problems, and what they cost

**Windows throttled the benchmark for not being in the foreground.** Midway through, every
multi-threaded number fell by about two thirds while single-thread pinned ones did not move: Windows
11 treated the console process as background work and applied EcoQoS. Phase 0 now measures it inside
one run — 8 × 4 saturated: **2.16 M/s with execution-speed throttling forced on, 8.75 M/s with it
off** — and every later phase runs with it off. Nothing in the program's output would have said so
without the A/B.

**The machine's power profile drops its clock under sustained load.** It runs a vendor "Silent"
profile, and a pause instruction — a fixed cycle count — reads 33 to 39 ns when the clock is up.
Every phase now starts after a five-second pause and prints that reading, so a throttled phase shows
in the log; within a phase, arms are interleaved so drift is common to all of them. Across runs,
nothing is comparable: the CAS ceiling was 5.59 M/s in one full run and 4.80 M/s in the next.

**The first version of every before-and-after was across runs.** The first event count, the
per-release producer wakes and the fixes were measured an hour apart, under different throttling,
and the ratios were not the ones a side-by-side run gives — batching's throughput gain, for one, came
out at 2.0× across runs and is 1.34× side by side. Both first versions are now options, run beside the
fixes in the same process (§6), and only those numbers are quoted.

**The paced producers' timer.** Producers sleep between bursts so that 32 of them do not occupy 32
CPUs; at 1 ms timer resolution they wake about once a millisecond and push what is due. When a design
cannot keep up, bursts grow into tens of thousands of events and the backlog feeds itself, which is
the 236 ms of "behind schedule" in §10 — a real failure to keep pace, measured from when each event
was due rather than from when it was pushed, so that it is not hidden.

### What cannot be measured here

- **AArch64.** No machine to run it on. Both relaxed-sequence bugs would be candidates to appear in
  a stress test there; on x86 only TSan sees them.
- **A model checker.** Relacy, CDSChecker or GenMC would explore the four-slot configuration
  exhaustively; none is installed, and the protocol's verification rests on TSan, the stress tests,
  and the review.
- **Cross-socket contention.** One socket; phase 0 says so as it measures.
- **The brief's machine.** 48 threads on 32 CPUs is oversubscribed, and §10's miss is largely that.

### Known gaps

- No `emplace`, by design (§4).
- Ticket holders sleep on a 200 µs timer (a millisecond, after Windows rounds it) once their spin
  and yields run out; a full queue's latency is set by its consumers anyway, and a precise per-slot
  wake would cost a wake per release.
- Per-entity ordering (§7) is not provided and not measured.
- The FIFO-versus-processing-order inversion count was planned and not built.

---

## Appendix: Glossary

**Reservation / publication** — claiming a position (a counter operation) and making the event at it
visible to consumers (a store to the slot's sequence). The proposed code does both with one
operation; the redesign separates them, which is the whole answer to the central question.

**Lap** — one trip round the ring. Position *p* is slot *p mod N* on lap *p / N*; the sequence number
is what tells laps apart.

**Hole** — a position reserved and not yet published. Indistinguishable from an empty queue at the
head without reading the tail.

**Tombstone** — a publication with no object behind it, used to pay for a ticket taken after a close.

**Event count** — a sleep primitive made of a generation number and a waiter count, used here so that
a producer with nobody to wake pays a fence and a load.

**Store buffering** — the litmus pattern in which two threads each store to one location and then
load the other's, and both see the old values. The one reordering x86's memory model permits, and
the shape of every lost-wakeup race.

**TSO** — total store order, x86's memory model. Stronger than C++'s: it hides most missing
acquire/release orderings in practice, which is why ThreadSanitizer, not an x86 test, finds them.

**LL/SC** — load-linked / store-conditional, the instruction pair older AArch64 uses for atomic
read-modify-writes. A contended store-conditional can fail and retry, so a `fetch_add` there is not
wait-free; ARMv8.1's LSE atomics are single instructions.

**EcoQoS** — Windows 11's efficiency quality of service: lower clocks and E-core placement for
processes it judges to be background work. A benchmark has to opt out (§12).

**Blocking / lock-free / wait-free** — progress guarantees. Wait-free: every operation completes in
bounded steps. Lock-free: some operation always completes. Blocking: a suspended thread can prevent
others from completing. This queue is blocking, without a lock (§7).

**Coordinated omission** — measuring only how long the operations that ran took, rather than how
late each event became; it hides exactly the stalls a latency requirement is about. §10's "behind
schedule" column is the correction.
