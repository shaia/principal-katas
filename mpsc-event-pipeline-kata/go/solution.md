# High-throughput in-process event pipeline — the Go answer

Answer to [question.md](../question.md), built from Go's own idioms and cost model. The
[C++](../cpp/solution.md) and [Python](../python/solution.md) answers are worth reading, but the
reasoning below is Go's, and it reaches its main conclusion from a Go-specific limitation the other
two do not have.

---

## The answer, up front

**Start with one buffered channel per producer.** It is ordinary Go — `make(chan Event, 4096)` per
producer, a non-blocking send, a consumer that ranges over the slice — it needs no `unsafe`, no
manual padding, no memory-model reasoning, and it satisfies every clause of the specification. It is
[`ChannelPipeline`](internal/weir/chanpipe.go), and it is about a hundred lines.

**Escalate to a hand-rolled SPSC ring only when the *consumer* becomes your bottleneck**, which is a
specific and measurable condition, not a vibe. It is [`Pipeline`](internal/weir/pipeline.go), it is
roughly nine hundred lines including package assembly, and §11 has the numbers that justify it.

The decisive clause in the requirements is *"event order only needs to be preserved per producer, not
globally."* Both designs honour it, so it does not discriminate between them — which is worth saying
plainly, because in the C++ answer that clause is the whole argument.

---

## 1. Why sharding, and why the obvious Go answer is half right

The reflexive Go design is one shared buffered channel. It is wrong here, and the reason is in the
runtime source rather than in anyone's opinion. A buffered channel is an `hchan`:

```go
type hchan struct {
    qcount, dataqsiz uint
    buf              unsafe.Pointer
    sendx, recvx     uint
    recvq, sendq     waitq
    lock             mutex        // <- here
}
```

A mutex plus a circular buffer, with every field in the same two cache lines. Every successful send
takes that lock — the lock-free fast path in `chansend` covers only the *failed* non-blocking case.
On Windows that mutex is `lock_sema.go`: spin, `osyield`, then sleep on a kernel semaphore, so losers
park in the kernel.

Measured in the full pipeline, one shared channel: **86.7 ns per send at one producer, 1312.7 ns at
thirty-two.** A 15× collapse. (An isolated `BenchmarkChanPush` with a consumer that trivially keeps
up reports 36.5 ns — the difference is the consumer competing for the same cores, and the in-situ
figure is the honest one.)

**Go's answer to a contended resource is not to hand-roll a lock-free structure. It is to shard so
nothing is contended.** One channel per producer means each `hchan.lock` has exactly one sender and
one receiver, so it is uncontended by construction, and an uncontended Go mutex is a single CAS.

That works, completely, on the side it addresses:

| producers | one shared chan | **one chan per producer** |
|---|---|---|
| 1 | 86.7 ns/push | **31.5** |
| 8 | 204.9 | **87.0** |
| 32 | 1312.7 | **117.9** |

Flat-ish instead of collapsing, and *faster than the SPSC ring's producer side* (178.6 ns at 32).
For a hundred lines of ordinary Go with no `unsafe` in it, that is the right default and most systems
should stop here.

### Where it runs out, and why it is Go's fault rather than the design's

Sustained throughput — what the consumer actually got through the sink — tells the other half:

| producers | chan per producer | SPSC rings |
|---|---|---|
| 1 | 8.2 M/s | 8.6 |
| 8 | 10.9 | **56.4** |
| 32 | 6.6 | **143.9** |

The sharded channels plateau around 10 M/s and their acceptance falls to 2.4%, while the rings scale
to 144 M/s. The producer side was never the problem; **the drain is.**

**Go has no bulk channel receive.** There is no `recvn(ch, buf)`, and `select` cannot take a dynamic
number of cases without `reflect.Select`, which allocates a `[]SelectCase` per call. So a consumer
draining N channels pays an `hchan.lock` acquire, a copy, and a release **per event** — about 30 ns
each — no matter how it is written.

The ring's `PopBatch` moves up to 512 events with one bounds-checked copy loop and **one** atomic
store for the whole batch. That is the entire difference, and it is why the escalation exists.

> The rule this gives you is sharper than "channels are slow": **shard your channels, and measure
> the consumer.** If the consumer keeps up, you are done. If it does not, no arrangement of channels
> will fix it, because the missing primitive is a bulk receive that Go does not have.

The Python answer reaches the identical conclusion from a completely different direction — its
`deque` is fast to append to and slow to drain, and the drain sets the ceiling for the same reason.
Two languages, two idiomatic containers, same failure mode: the consumer cannot be sharded, so its
per-event cost is the only cost that ultimately matters.

---

## 1a. The escalation: hand-rolled SPSC rings

When the consumer is the bottleneck, the fix is to stop paying a lock per event on the drain. One
bounded single-producer/single-consumer ring per producer, free-running indices, and a `PopBatch`
that moves up to 512 events with a single atomic store.

This is the same structure the C++ answer arrives at, but for a different reason. There it is chosen
to keep producers off a shared cache line. Here producers were already fine — sharded channels
handled that — and the ring is chosen because it is the only way to get a **bulk drain** in a
language that does not offer one.

Measured, 400k events per producer, one consumer, on a 32-logical-core i9-13980HX:

| producers | offered M/s | accepted | sustained M/s | ns/push (no signal) | ns/push (+bitmap) |
|---|---|---|---|---|---|
| 1 | 10.4 | 96.7% | 10.1 | 89.2 | 96.0 |
| 4 | 34.0 | 99.8% | 33.9 | 112.1 | 117.6 |
| 16 | 106.3 | 94.6% | 100.5 | 139.4 | 150.6 |
| 32 | 209.9 | 81.6% | 171.4 | 164.6 | 152.4 |

**A prediction made before running it, and confirmed: the two `ns/push` columns are nearly
identical.** In C++ the bitmap costs ~15 ns/push, about 40% on top of an 18–29 ns push, because
`signal()` opens with an explicit `seq_cst` fence. Here that fence does not exist — the publish store
is already a lock-prefixed `XCHGQ` providing the same StoreLoad edge — so the bitmap costs one extra
`MOVQ` load and a predicted-not-taken branch. **The notification mechanism is relatively cheaper in
Go than in C++, because Go already charged for it on every push whether you use it or not.**

Also worth stating plainly: `ns/push` here is 89–165 ns where C++ reports 18–29 ns, and it is *not*
flat the way the C++ column is. Some of that is Go's `XCHGQ` publish; some is goroutine scheduling
and the consumer's spin competing for the same cores. An isolated `BenchmarkRingPush` is **10.33
ns/op**, so the gap is contention and scheduling, not the ring. Reporting the in-situ number rather
than the microbenchmark is the point.

---

## What else changes when the language changes

| The C++ answer relies on | Go | Section |
|---|---|---|
| choosing `memory_order_release`/`acquire` | does not exist; every atomic is sequentially consistent | [§2](#2-memory-ordering-the-collapse) |
| a `seq_cst` fence for StoreLoad in the wakeup path | free — fused into the publish store, and **the bug it guards is unwritable** | [§2](#2-memory-ordering-the-collapse), [§7](#7-follow-up-64-producers-48-active) |
| `alignas(64)` | no equivalent, and the allocator will not give it either | [§3](#3-cache-behaviour) |
| no garbage collector | `Event` must stay pointer-free or 8 MiB joins the mark path | [§3a](#3a-garbage-collection) |
| RAII destructors | `Close()` + `defer`; finalizers are the wrong tool | [§4](#4-ownership) |
| ThreadSanitizer unavailable on this target | `-race` **works, and closes that gap** | [§10](#10-verification) |
| "the mutex might be better" as a caveat | a `chan` **is** a mutex plus a ring, so the caveat is the default — and the escalation trigger is consumer saturation, not producer count | [§9](#9-when-the-simple-answer-wins), [§11](#11-the-four-designs-measured) |
| `steady_clock` = QPC, ~100 ns | `time.Now()` is ~750,000× coarser and would produce a flattering lie | [§8a](#8a-measuring-at-all) |

Two of those change the shape of the code rather than just its spelling: the clock (§8a) forced an
entire subsystem the other answers do not need, and the memory model (§2) deletes the single hardest
paragraph in the C++ design — along with the bug it guards against.

---

## 2. Memory ordering: the collapse

**This is the section where Go differs most, and the difference runs in both directions.**

Go's memory model states: *"All the atomic operations executed in a program behave as though executed
in some sequentially consistent order."* There is no acquire, no release, no relaxed. You do not
choose. On amd64 that asymmetry is stark:

| operation | Go emits | C++ emits | measured |
|---|---|---|---|
| `tail.Store(t+1)` — **the publish** | `XCHGQ AX, (CX)` — lock-prefixed, full barrier | `MOV` (release store) | **12.01 ns** |
| plain `uint64` store | `MOVQ` | `MOV` | **1.10 ns** |
| `tail.Load()` | `MOVQ` | `MOV` (acquire load) | **1.90 ns** |

Verified by disassembly (`go build -gcflags=-S`), and the emitted code shows the cache-line layout
too — the head publish is `XCHGQ DX, 64(AX)`, that `64` being the padding in §3 made visible in an
instruction.

So **the single hottest operation in the design — once per event — is more expensive in Go than in
C++, and the language offers no way out.** Not even internally: the runtime's own `StoreRel64` is
`JMP Store64` on amd64. The cheap release store does not exist to be borrowed.

Two honesty notes. First, the isolated 12.01 ns overstates the in-situ cost: a tight loop hammering
one address serialises maximally, and the *entire* `TryPush` benchmarks at 10.33 ns. Second, the
`PopBatch` side pays this once per batch of up to 512, so it is ~0.02 ns/event and irrelevant.

### What that buys

The C++ §7 spends a page on one line. Its `signal()` must open with
`atomic_thread_fence(seq_cst)`, and the writeup explains at length why it cannot be moved below the
bitmap check: x86 is TSO, which permits exactly one reordering — StoreLoad — so an unfenced load may
execute before the tail publish drains the store buffer, and the event ends up published with its
bit clear and nobody looking. The author shipped the "obvious optimization" (check the bit first,
fence only on the slow path) before catching it, and concludes that this class of bug *"is found by
reasoning or not at all"* — noting that ThreadSanitizer is unavailable on that target, so nothing
automated would have caught it.

**In Go that fence does not exist, and neither does the bug.** The proof is four operations in one
total order. Let A1 be the producer's `tail.Store` and A2 its `active.Load`; B1 the consumer's
`active.And` and B2 its `tail.Load` inside `EmptyNow`. Program order gives A1 <ₛ A2 and B1 <ₛ B2.
Suppose the bad outcome — A2 reads the bit set so the producer returns, and B2 reads the ring empty
so the consumer parks. A2 seeing the bit set means A2 <ₛ B1, since B1 is the only operation that
clears it. Then A1 <ₛ A2 <ₛ B1 <ₛ B2, so B2 must observe A1 and the ring is *not* empty.
Contradiction.

The tempting optimization is not merely wrong here, it is **not expressible**: there is no relaxed
load to write. The only unsound thing available is a plain non-atomic read of `active`, which is a
data race by definition and which `-race` reports.

> Go removes the choice, charges for it on every push, and hands back the hardest part of the design
> for free. Whether that trade is good depends entirely on who is maintaining the code.

The C++ §2 closes with *"writing the weakest correct ordering costs nothing on x86 and is the
difference between correct and broken on AArch64."* That sentence has no Go analogue. Go pays the
AArch64 price on x86 and gives you a portability you cannot decline.

---

## 3. Cache behaviour

The cached-index trick, the batching, and the power-of-two masking all port unchanged and are still
the biggest wins. Two things differ.

### Padding, and what Go will not guarantee

Go has no `alignas`. The idiom is explicit `_ [N]byte` fields — reliable, because the spec fixes
struct field order. `SpscRing` puts `tail` on line 0, `head` on line 1, `slots` on line 2, and
`unsafe.Sizeof` is 131,200 bytes, byte-identical to the C++ layout.

**But absolute alignment is not available, and asserting it would be asserting something false.**
Measured: `SpscRing` is 131,200 bytes, exceeds 32 KiB, takes the page-allocated large-object path,
and comes back 64-byte aligned on every allocation. `Pipeline` is 4,472 bytes, takes the size-class
allocator, and came back at `base%64 == 8` on **every single allocation**. So the "objects ≥512
bytes get ≥64-byte alignment" size-class property does not hold for this path.

What padding *does* guarantee is the property that actually matters: two 64-byte groups land on
different cache lines whatever the base offset. `New()` asserts that — and asserts that no atomic
straddles a line — rather than asserting alignment it cannot have.

A first version of this file did panic on absolute alignment, and it fired immediately. That was
worth having.

### Two layout bugs, one in each language

`atomic.Bool` is **four bytes, not one** — it wraps a `uint32`. Getting that wrong put `running` at
offset 4164 (4 mod 64) and silently broke the separation the padding exists to create. Caught by
asserting offsets.

Writing that assertion is what prompted dumping the C++ layout, which found the same class of bug
**in the shipped C++ answer**:

| member | offset | line | accessed |
|---|---|---|---|
| `active_` | 8320 | 130 | alone — correct |
| `running_` | 8384 | **131** | |
| `accepting_` | 8385 | **131** | **acquire-load on every push** |
| `parked_` | 8386 | **131** | `seq_cst` store ×2 per park |
| `retire_pending_` | 8387 | **131** | **`exchange` — an RMW — on every idle pass** |
| `park_mu_` | 8392–8447 | **131** | locked per notify and per park |

Only `running_` carried the `alignas(64)`; the rest fell in behind it. The consumer's idle loop takes
that line **exclusive** on every empty pass while all 64 producers read `accepting_` from it on every
push. A design whose §3 is about false sharing contained false sharing, in the one place a layout
dump finds it and no test ever would. Fixed in [cpp/src/event_pipeline.hpp](../cpp/src/event_pipeline.hpp).

The Go version co-locates `active` and `accepting` *deliberately* — both are read on every push and
written only on transitions, so a push touches one line rather than two — and keeps the
consumer-written state on its own.

---

## 3a. Garbage collection

**A section with no C++ counterpart, and the place the Go answer earns its keep.**

`Event` is `{uint32, uint32, int64, [16]byte}`: 32 bytes, offsets 0/4/8/16, and **pointer-free**. A
`[4096]Event` array is therefore a *noscan* allocation the collector never walks.

This is not a nicety. Measured over 64 rings × 4096 events at an identical 8.4 MiB heap:

| ring element type | 20× forced GC | avg STW pause |
|---|---|---|
| pointer-free (noscan) | 3.63 ms | **~0 µs** |
| one pointer per event | 9.84 ms | **26.8 µs** |

A single pointer costs 2.7× the mark work and injects 26.8 µs of stop-the-world — landing directly
in the p99.9 the whole design exists to protect.

**The trap, in bold, because it is the obvious translation: do not use `time.Time` for the
timestamp.** It embeds `loc *Location`. One field, and 8 MiB of ring storage silently joins the mark
path. The same applies to `string`, `[]byte`, `error`, `any`, a map or a channel. `TestEventIsPointerFree`
walks the type reflectively and is the guard that survives a future edit by someone who has not read
this paragraph.

Related: `SpscRing` holds `[RingCapacity]Event` inline rather than `[]Event`. A slice field would put
a pointer in the struct and forfeit the property.

**Zero allocation on the hot path** is asserted rather than claimed: `testing.AllocsPerRun` over
`TryPush` must return 0, and `-benchmem` reports `0 B/op  0 allocs/op`. The two things that would
break it in an obvious translation are `time.After` in the park loop (a `*Timer` and a `chan Time`
per park — thousands of allocations per second on an idle pipeline) and boxing an `Event` into an
`any`. Both are avoided; the timer is created once and `Reset`.

---

## 4. Ownership

The three-state protocol — `Free → Active → Retiring → Free`, where **only the consumer may publish
`Free`** — ports unchanged, and the counter-fold-at-reclaim subtlety with it.

What weakens is its justification. In C++ the protocol prevents a use-after-free: a ring freed when
its producer exits is memory the consumer is still reading. In Go the collector makes that impossible
— the consumer holds a reference, so the ring cannot go away underneath it. The protocol survives as
a **correctness** protocol (do not hand a slot to a new producer while the previous owner's events
are undrained; do not lose their counters) but not a memory-safety one. Worth saying rather than
implying the same stakes.

### No RAII, and that is a real loss

The C++ `ProducerHandle` is move-only RAII: the destructor retires the slot, so retirement cannot be
forgotten, and a pipeline dropped without an explicit stop is still safe. Go has neither destructors
nor scope-bound cleanup. `Close()` plus `defer` is the substitute, and forgetting it leaks one slot
out of 64. `Stop` is likewise mandatory where the C++ destructor made it optional.

`runtime.AddCleanup` and `runtime.SetFinalizer` are **not** the answer, for four separate reasons:

1. **Timing.** They run "some time after ptr is no longer reachable", on another goroutine, with no
   ordering guarantee and no guarantee they run before exit. The protocol exists precisely to make
   the handoff deterministic; a slot released eventually, against a budget of 64, is a resource leak
   with a latency attached.
2. A C++ destructor is a **scope** guarantee; a Go cleanup is a **reachability** hint. Substituting
   one for the other is a category error.
3. `AddCleanup` never runs if `ptr` is reachable from the cleanup or its argument — so the natural
   spelling, a closure over the handle, silently never fires.
4. It moves retirement onto an arbitrary goroutine, adding a third participant to a protocol whose
   entire value is having exactly two.

Cleanups are the right tool for *diagnosing* a missed `Close`, and wrong for performing it.

---

## 5. Backpressure and overload

Essentially unchanged, and the most transferable section of the C++ answer. Bounded queue, explicit
terminating policy, drops counted per producer and never silent, the end-to-end chain from socket to
consumer to rings to drop policy with every link bounded.

Measured against a deliberately slow sink, 8 producers:

```
slow sink: pushed=130048 dropped=669952 (83.7%) high_water=4096 worst_push=351.0 us
abort shutdown returned in 4.3 ms
```

83.7% loss under a pathologically slow consumer — by policy, visibly counted. `high_water` never
exceeded ring capacity (memory stayed bounded) and the worst single push was 351 µs, dominated by
scheduling rather than by the queue.

One Go-specific addition. A non-blocking channel send —

```go
select {
case ch <- e:
default: // drop, and count it
}
```

— *is* `DropNewest`, and is the only channel discipline that satisfies "producers must not block
indefinitely". The reflexive Go answer, a blocking send, violates a hard requirement outright. Worth
saying because it is the single most common way this requirement gets lost in a Go codebase.

---

## 6. Shutdown

Same two modes, same ordering constraint, same deadline. `sync.Once` for idempotency, a `done`
channel in place of `thread::join`.

The constraint that must never be violated is unchanged and worth repeating, because the C++ shipped
a bug here: the quiesce check runs on the *stopping* goroutine, which is neither producer nor
consumer, so it must read both atomics via `SizeNow()`. Using `ProducerSizeHint()` reads the
producer's private cached index and is a data race. **In Go, `-race` catches that outright** — where
the C++ found it by re-reading the code.

The loss is the destructor safety net: a `Pipeline` dropped without `Stop` leaves a goroutine
running. Documented, and `Stop` is idempotent so `defer` plus an explicit call is fine.

---

## 7. Follow-up: 64 producers, 4–8 active

The mechanism ports directly. `atomic.Uint64.Or(mask) (old uint64)` and `.And` exist, so `fetch_or`,
`fetch_and`, and the `prev == 0` coalescing check all translate one-for-one. Scan iteration is
`bits.TrailingZeros64` with `bits.RotateLeft64(x, -k)` for `std::rotr`, and the fairness rotation and
per-ring drain cap are unchanged.

Three things differ.

**The Dekker section shrinks to a paragraph.** See §2: Go's forced sequential consistency already
provides the StoreLoad edge, so the fence is deleted and the bug is unwritable.

**Parking is forced into a channel, not chosen.** `sync.Cond` is the direct port of
`std::condition_variable` and it is wrong here for a structural reason: **`sync.Cond` has no timed
wait.** It offers `Wait`, `Signal`, `Broadcast` and nothing else. The C++ design's third layer of
defence is `wait_for(lk, park_timeout)` — a bounded worst case the writeup says is worth more than
confidence that the wakeup path is perfect. Reproducing that with `sync.Cond` needs a second
goroutine broadcasting on a ticker. A capacity-1 channel gives it in a `select`, and the capacity
*is* the coalescing: N concurrent notifies collapse to one pending token, which expresses "at most
one pending wakeup" more directly than `prev == 0` does. The idiomatic answer here is also the forced
one.

**The eager-clearing trap survives completely, and Go does nothing about it.** Clearing a bit as soon
as its ring drains empty produces write churn proportional to throughput — the C++ measured 582,954
bitmap writes against 8 for the deferred version, and *no test failed*. That is not an ordering bug;
it is a design bug about write frequency on a shared line, and no language feature prevents it. So Go
closed one of the two traps in this mechanism and not the other, and the second one is the one that
was invisible.

`TestBitmapCoalescingHolds` asserts it rather than trusting it. Measured: **8 bitmap writes for
800,000 events**, one per producer — 0.00001 per event.

---

## 8. Does it actually improve tail latency?

Methodology carried over unchanged — it is the most transferable part of the C++ answer. A/B in one
binary as a runtime switch; registration swept 8→64 with active producers pinned at 8 (sweeping the
active count would vary CPU contention and scan width together and confound them); open-loop load at
a fixed rate with latency measured from the **intended** send time; percentiles never means;
preallocated sample buffer with 1-in-4 sampling; five repetitions with the median taken per
statistic; mechanism counters reported beside latency.

```
registered scan   |  p50 us   p99 us    p99.9 | probes/pass   bmp-wr  drop%
8          full   |     0.3     83.2    327.1 |         8.0        0   0.0%
8          bitmap |     0.3     73.9    283.4 |         8.0        8   0.0%
16         full   |     0.4     87.8    254.7 |        16.0        0   0.0%
16         bitmap |     0.4     93.2    304.9 |         8.0       16   0.0%
32         full   |     0.4     92.2    277.1 |        32.0        0   0.0%
32         bitmap |     0.4     88.7    245.6 |         8.0       32   0.0%
64         full   |     0.5     93.8    261.7 |        64.0        0   0.0%
64         bitmap |     0.4     69.1    239.9 |         8.0       64   0.0%
```

**The mechanism works exactly as designed.** `probes/pass` for full scan tracks registration
precisely (8 → 16 → 32 → 64) while the bitmap stays flat at 8.0 regardless — the consumer genuinely
stopped looking at idle rings. Coalescing holds: one bitmap write per registered producer, against
millions of events.

**And, as in C++, p99 and p99.9 do not separate the two policies.** They move by tens of microseconds
in both directions between rows. That is noise, and reading a story into it would be exactly the
failure the follow-up is probing for. p50 shows the same mild, reproducible full-scan degradation the
C++ saw (0.3 → 0.5 µs) against a flat bitmap, but at this scale it is a fraction of a microsecond.

The reason is worth understanding rather than explaining away, and it is unchanged from C++: probing
an idle ring is *cheap*. Nobody writes those index lines, so they stay valid in the consumer's own L1
indefinitely. Sixty-four rings' index lines are about 8 KB — they fit. The scan is 64 L1 hits, not 64
coherence misses, and 64 L1 hits do not move a p99.9 that has scheduler noise in it.

**One Go-specific caveat that makes the negative result stronger, not weaker.** There is no way in Go
to obtain a thread the scheduler will not preempt: a goroutine running more than 10 ms without
yielding is forcibly preempted by `sysmon`, and `runtime.LockOSThread` does not exempt it. So Go's
p99.9 has a structural contributor the C++ version does not have, and the design should be neither
credited nor blamed for it. This machine's hybrid P-core/E-core topology adds another.

**A confound fixed relative to the C++.** That version increments `rings_probed_` with an atomic
`fetch_add` *per probe* — a `lock xadd`, ~18–20 cycles, instrumenting a probe that costs ~4 cycles.
The counter is roughly four times the cost of the thing it measures and it scales with registration,
so part of its reported full-scan p50 degradation is the instrument. Here the consumer accumulates
into plain local fields and publishes once per pass. The Go full-scan degradation (0.3 → 0.5 µs) is
indeed milder than the C++ (0.5 → 3.3 µs), which is consistent with that being part of the
explanation — though the two runs differ in enough other ways that this is a hypothesis, not a
demonstration.

---

## 8a. Measuring at all

**This section exists because the obvious Go port produces a flattering lie, and it has no C++
counterpart.**

On Windows, Go's `time.Now()` monotonic reading does not come from `QueryPerformanceCounter`. It
reads `KUSER_SHARED_DATA.InterruptTime` and scales by 100 ns (`runtime/time_windows_amd64.s`), so it
advances only at the system clock interrupt — nominally every 15.625 ms, or ~1 ms when some process
holds `timeBeginPeriod(1)`.

Measured on this machine: **237,201 calls to `time.Now()` across 5 ms produced six distinct values.**
The read itself is cheap (4.4 ns); the *value* advances about a thousand times a second.

The C++ `steady_clock` is QPC at ~100 ns, so its reported p50 of 0.5–3.3 µs is a real measurement. A
Go port using `time.Now()` would report `p50 = 0.0 µs` with a tail quantized to whole milliseconds —
and would look **faster** than the C++. That is the worst kind of wrong: a flattering number with no
bug to find.

So the port brings its own clock: `RDTSC` in package assembly, calibrated once against the wall clock
with both endpoints pinned to clock *edges* so the coarse reference's quantization does not
accumulate. Phase 0 prints the instrument before any result depends on it:

```
clock   time.Now() smallest observable tick :      307.5 us  (measured)
clock   TSC resolution                      :      0.413 ns  (2.419 GHz, invariant=true)
clock   TSC read cost                       :       6.57 ns
clock   ratio                               :     743899x coarser if we used time.Now()
timer   time.Sleep(100us) best actual        :      522.3 us  (so the pacer spins)
```

Calibration lands within **0.03%** against a known 200 ms sleep, and the counter yields 7,311
distinct values per millisecond against `time.Now()`'s six per five milliseconds. Invariant TSC is
probed via `CPUID.80000007H:EDX[8]`; without it, calibrating once and extrapolating would be unsound
and the phase says so.

The pacer must **spin**, not sleep: measured, `time.Sleep` requests of 1, 10 and 100 µs all return in
~510 µs. And it must spin rather than `Gosched`, which would deschedule the producer past its own
deadline and inject exactly the jitter an open-loop pacer exists to expose.

This is the direct descendant of the C++ writeup's *"measurement bugs, caught by disbelieving the
numbers"*, and it is the single strongest argument for having done the port at all.

---

## 9. When the simple answer wins

The C++ §9 argument survives and gets **stronger**, for a reason specific to Go: here the
sophisticated design is ~900 lines with package assembly, `unsafe`, hand-computed padding, a Dekker
handshake and a three-state lifetime protocol — against `make(chan Event, 4096)` per producer.

One half of that argument gets *weaker*, and it is worth conceding: the reviewer-fluency point. C++
§9 says the ring "needs a reviewer fluent in the C++ memory model". Go's sequentially consistent
atomics remove most of what there was to be fluent about — there is no ordering to get wrong (§2) —
and `-race` checks the rest (§10). So the ring is genuinely more reviewable in Go than in C++.

The total-complexity argument is much stronger, though, and it is the one that decides. Nine hundred
lines against a hundred is not a rounding error, and the hundred needs no `unsafe`, no assembly, and
no calibrated clock to benchmark honestly.

**But the crossover is not where §11's C++ counterpart would put it.** It is not a producer count.
Sharded channels win `ns/push` at *every* count measured, including 32 — the producer side never
becomes the reason to escalate. The condition is:

> **Is the consumer saturated?** If it is keeping up, sharded channels are the answer at any producer
> count. If it is not, no channel arrangement will fix it, because the missing primitive is a bulk
> receive, and that is the only thing the ring is buying you.

Concretely: measure `sustained` against `offered`. While acceptance stays high, stop. When acceptance
starts falling and the consumer goroutine is pinned, the drain is your bottleneck and the ring is
justified. Reaching for it before observing that is not sophistication, it is a mistake.

Prefer the channel outright — sharded or not — when global ordering is actually required, when events
are large or not trivially copyable, or when correctness matters more than throughput.

---

## 10. Verification

```powershell
go vet ./...                                   # includes copylocks; `go test` does NOT run it
go test ./...                                  # invariants, seconds
$env:CGO_ENABLED=1; $env:CC="gcc"
go test -race ./...                            # the gap-closing run
go build -o bin/weir.exe ./cmd/weir; .\bin\weir.exe    # exit code is the verdict
```

**Test split.** Invariants — correctness, ordering, accounting, lifetime churn, lossless drain,
bounded depth, wakeup liveness, layout, allocation-freedom, and the `ChannelPipeline`'s own
accounting and registry-exhaustion behaviour — are real `go test`, because that is what
gets `-race`, `go vet` and CI for free. Measurements are `cmd/weir`, because they take minutes,
saturate the machine, and produce a table a human reads. The C++ single-binary design is a constraint
(C++ has no standard runner), not a choice; porting it faithfully would be cargo-culting.

### The race detector closes the C++'s stated gap

The C++ §10 says plainly: *"ThreadSanitizer is not available for `x86_64-pc-windows-msvc` — clang
rejects the flag outright on this target. That is a genuine gap: the race argument here rests on the
documented ordering pairs and on runtime invariants, not on a tool having checked it."*

`go test -race` runs clean on this design, on this machine. The race runtime ships **inside the
toolchain** (`GOROOT/src/runtime/race/internal/amd64v1/race_windows.syso`) rather than needing an
ASan DLL on `PATH`, which is a real improvement on the C++ story.

Three caveats, stated with the same prominence as the claim:

1. **`-race` requires `CGO_ENABLED=1` and a GCC-compatible C compiler.** This machine had neither by
   default — `CGO_ENABLED=0`, no gcc, and clang 21 rejects cgo's `-mthreads` when targeting
   `x86_64-pc-windows-msvc`. Installing MinGW-w64 (`winget install BrechtSanders.WinLibs.POSIX.UCRT`)
   is a prerequisite, not a detail.
2. **`-race` de-intrinsifies `sync/atomic`** into real ThreadSanitizer calls, so a race build is a
   different program by 5–20×. `cmd/weir` refuses to print a results table when `raceEnabled`.
3. **`-race` cannot find missing-ordering bugs** — but Go's sequentially consistent atomics make
   those unwritable (§2), so the language and the tool cover complementary halves. Between them the
   gap is genuinely closed.

**A clean run only means something if the tool is engaged**, so `TestSPSCContractViolationIsDetected`
deliberately pushes from two goroutines onto one handle and is verified to be reported:

```
WARNING: DATA RACE
Write at 0x00c0002026a0 by goroutine 10:  spsc_ring.go:78
Previous write at 0x00c0002026a0 by goroutine 9:  spsc_ring.go:78
```

That is worth more than the clean run itself. **The SPSC contract the C++ can only document, Go
enforces** — and it is a much easier contract to break in Go, because "goroutine per unit of work" is
the default idiom and nothing in the type system objects.

### What the tests actually caught

- **`atomic.Bool` is four bytes, not one.** The padding arithmetic assumed one, which put `running`
  at offset 4164 and silently broke the cache-line separation. Caught by asserting offsets, which is
  the only thing that would have caught it.
- **Go will not give a 4.5 KB struct cache-line alignment.** An assertion demanding it fired on the
  first run — `Pipeline` came back at `base%64 == 8` on every allocation. The assertion was wrong,
  not the allocator, and it was replaced with the relative-separation check that is actually
  guaranteed.
- **A wakeup test that reported `parks=0` and passed.** The producer pushed the next event while the
  consumer was still spinning, so the park path never ran and the race the test exists for was never
  attempted. It now waits for the park counter to advance before each push, and reports 396 parks
  across 300 rounds. A green tick from a test that never ran the code under test is worse than no
  test.
- **A prototype baseline benchmark reporting 775 M/s for the rings against 3.8 M/s for a channel.**
  The rings were dropping under overload while the channel blocked, so a large part of that gap was
  drops counted as pushes. Every row now reports offered, accepted and sustained separately.

---

## 11. The four designs, measured

Everything above, in one table. 400k events per producer, identical harness, sink and pacer,
non-blocking sends throughout so every design obeys "must not block indefinitely".

| producers | transport | offered M/s | accepted | **sustained M/s** | ns/push |
|---|---|---|---|---|---|
| 1 | rings | 8.7 | 99.0% | 8.6 | 115.2 |
| 1 | **chan/producer** | 31.7 | 26.2% | 8.2 | **31.5** |
| 1 | one shared chan | 11.5 | 100.0% | **11.5** | 86.7 |
| 1 | one shared mutex | 23.1 | 100.0% | **23.1** | 43.3 |
| 8 | **rings** | 56.4 | 100.0% | **56.4** | 141.7 |
| 8 | chan/producer | 92.0 | 11.9% | 10.9 | 87.0 |
| 8 | one shared chan | 39.0 | 14.0% | 5.2 | 204.9 |
| 8 | one shared mutex | 15.8 | 100.0% | 15.8 | 506.1 |
| 32 | **rings** | 179.1 | 80.4% | **143.9** | 178.6 |
| 32 | chan/producer | 271.4 | 2.4% | 6.6 | **117.9** |
| 32 | one shared chan | 24.4 | 4.9% | 1.2 | 1312.7 |
| 32 | one shared mutex | 14.7 | 100.0% | 14.7 | 2175.6 |

**The two columns say different things and both matter.**

`ns/push` is the *producer's* experience, and sharding wins it outright: 31.5 → 117.9 ns, better than
the rings at every count above one. Sharding is the correct Go fix for contention and it works.

`sustained` is what actually reached the socket, and it inverts: sharded channels plateau at ~10 M/s
while the rings reach 144. The producers were never the bottleneck once sharded; the consumer is, and
no channel arrangement fixes it, because Go has no bulk receive.

**At one producer, everything beats the rings.** A single shared channel manages 11.5 M/s sustained
against the rings' 8.6, with a third of the code and none of the `unsafe`. If you have one or two
producers, write the channel.

**The shared designs degrade exactly as the C++ §1 predicts** — 36 → 1313 ns for the channel, 43 →
2176 for the mutex — a shared coordination point does not plateau, it collapses. But note the mutex
holds 100% acceptance throughout: not because it is keeping up, but because contention throttles its
producers below what its consumer can absorb. Both are "backpressure"; only one is the system working
as intended, and a comparison reading only the drop rate would rank them backwards.

### What the channel still does not give you

This is the part a throughput comparison misses, and it is the durable argument:

- **Capacity isolation.** One runaway producer consumes the entire shared channel's headroom and
  causes *everyone else's* pushes to drop. N per-producer rings bound each producer's memory
  independently. No amount of channel tuning fixes this; it is a semantic difference.
- **Per-producer accounting.** Per-producer `pushed`/`dropped`/`high_water` localise an overload to
  the producer that caused it. A channel gives one global drop count, which tells you a problem
  exists and nothing about where — precisely when you most need to know.
- **Bounded, attributable memory.** `N × capacity` is a number that goes in a capacity plan per
  producer.

### The decision procedure

1. **One or two producers?** One shared buffered channel. Eleven lines, and it is the fastest thing
   in the table at that scale.
2. **Many producers, and `ns/push` matters?** One channel per producer. Sharding removes the
   contention completely, needs no `unsafe`, and the race detector has nothing to look at.
3. **Consumer saturated?** Only then the rings, and only because Go has no bulk channel receive.
   Budget nine hundred lines, package assembly, and a reviewer fluent in what `-race` does and does
   not cover.

**Measure step 2 before doing step 3.** The escalation is justified by a specific, observable
condition — the consumer is the bottleneck and cannot be sharded — and not by the size of the
producer count.

---

## Appendix: Go glossary

Terms the C++ [glossary](../cpp/solution.md#appendix-glossary) does not cover.

| Term | Meaning |
|---|---|
| **Goroutine** | A green thread multiplexed onto OS threads by the runtime. Cheap to create (~4 KB stack), which is why "one per unit of work" is idiomatic — and why the SPSC contract is easier to break here than in C++. |
| **GOMAXPROCS** | How many goroutines may run Go code simultaneously. Go 1.25 updates it dynamically, so a benchmark must pin it or the value can change mid-run. |
| **`hchan`** | The runtime struct behind a channel: a mutex plus a circular buffer plus two wait queues. §11's entire argument. |
| **noscan / ptrdata** | A type with `ptrdata == 0` contains no pointers, so the collector never scans allocations of it. §3a's headline property. |
| **Write barrier** | Compiler-inserted bookkeeping on pointer stores, so the collector can track mutation concurrently. Pointer-free types skip it entirely. |
| **gcshape stenciling** | Go instantiates a generic function once per *GC shape*, not per type, so all pointer-typed arguments share one instantiation and dispatch through a runtime dictionary. Why `Pipeline` takes a `Sink` interface rather than a type parameter: generics would buy nothing and cost a viral type parameter. |
| **`noCopy` / copylocks** | A marker with `Lock`/`Unlock` methods embedded in `sync/atomic` types, so `go vet -copylocks` rejects copying a struct that contains one. Free enforcement of what C++ needs `= delete` for — but `go test` does not run copylocks, so CI must invoke `go vet` separately. |
| **Asynchronous preemption** | Since Go 1.14 the runtime can preempt a goroutine mid-loop by signal, so a spin loop cannot wedge the scheduler or block a GC. It also means no thread here is preemption-free. |
| **`sysmon`** | The runtime's monitor thread. Forcibly preempts any goroutine running >10 ms without yielding. A structural contributor to p99.9 that C++ does not have. |
| **`KUSER_SHARED_DATA` / InterruptTime** | The Windows shared page and the tick counter Go's `time.Now()` actually reads on that platform, rather than QPC. The reason §8a exists. |
| **Invariant TSC** | `CPUID.80000007H:EDX[8]`: the timestamp counter runs at a constant rate regardless of core frequency and C-state. A precondition for calibrating once and extrapolating. |
| **`unsafe.Slice` / `unsafe.SliceData`** | Build a slice from a pointer and length, and get the pointer behind a slice. Used once, to reinterpret `[]Event` as `[]byte` for the sink with no copy — defensible only because `Event` is pointer-free and has no padding. |
| **`runtime.AddCleanup` vs `SetFinalizer`** | Reachability-triggered callbacks. Both wrong for a lifetime protocol that needs a deterministic handoff; right for detecting that one was skipped. See §4. |
| **De-intrinsification under `-race`** | The compiler skips its intrinsic table for `sync/atomic` under `-race`, turning every atomic into a real TSan call. Why a race build must never be benchmarked. |
