# A bounded MPMC work queue in Go — the runtime already wrote one, and it does not meet the brief

Answer to [../question.md](../question.md) and the [Go track](question.md), with a working
implementation and benchmarks alongside it. The C++ answer is [../cpp/solution.md](../cpp/solution.md);
the Python track is not answered yet.

Every number below came from this directory's code, on the same i9-13980HX laptop as the C++ track (8
P-cores, 16 E-cores, 32 logical CPUs) under Windows 11 with Go 1.25.5:
- **Per-operation costs** come from `go test -bench`, the median of three counts.
- **Everything else** comes from [`bin/turnstile.exe`](cmd/turnstile/), whose exit code is the
  verdict. Unless a section says otherwise, its figures are from one full run, the second of two back
  to back; where the first run of the pair disagrees in a way that matters, both are given.
- **Arms compared in a table** ran interleaved in that run.
- **Nothing here is compared with a C++ figure as a ratio.** Those came from other runs, with another
  harness, and §6 shows the harness alone moves the answer.

Terms of art are expanded in the [Glossary](#appendix-glossary).

---

## The answer, up front

- **The channel answers every correctness step and misses the brief by a factor of 240.** One lock
  makes reservation, copy and publication a single critical section, so nothing in steps 2–8 can go
  wrong. But at 32 × 16 with consumers spending 1 µs per event it moved **1.97 M/s against 5 M/s
  offered**, and its p99 send was **488 µs**. The lock is the reason. One receive, timed by itself
  with events always waiting, has a p99 of 64–210 µs whether one producer feeds it or thirty-two. The
  runtime's own contention profile puts 59 % of the waiting in receives and 40 % in sends. (§1)
- **The ring ports intact, gets simpler, and meets the p99.** There are no orderings to choose, the
  value is a plain field, and consumed slots are zeroed for the collector. At 32 × 16 and 5 M/s its
  p99 push was **899 ns** (1.11 µs in the other run of the pair), with no allocation and no
  collection in the measured window. The rule written before the run held by a factor of 500: the
  ring's worst repetition had to beat the channel's best. (§2, §6)
- **One token channel shared by sixteen sleepers recreates the channel's problem.** At 200 k/s its
  p99 push was **88.0 µs**. A token channel per sleeper, with a bitmap saying who is asleep, brought
  that to **1.90 µs**. The runtime's profile shows 6.66 s of lock waiting for the first and none for
  the second. (§3)
- **A producer descheduled inside its window, with every P held by a spinning consumer, waits for
  the runtime's forced preemption.** That was 20.6 ms at the median for a spin with no calls in it,
  and **forever** — until the spinners gave up at 1 s — with asynchronous preemption off. Consumers
  that park let it back in **12.3 µs**. (§4)
- **Shutdown is a separate signal, a count of producers, and the data channel closed last.** The
  textbook `select` over the send and the signal accepted 165 events after `Close` returned; every
  other arm accepted none. Review found a hang in the ported ticket close, which the C++ queue shares;
  the Go ring records where the tail stood at the close and drains to there. (§5)
- **The comparison with the C++ track is a comparison of pacers.** Both harnesses ask producers to
  nap 200 µs between bursts. Go's timer returns in about 0.6 ms; on this machine C++'s `sleep_for`
  returned after 37 ms, so each C++ producer pushed about 5,800 events back to back. Given that same
  arrival process, the Go ring's p99 was **2.22 µs**, more than twice its own. (§6)
- **The API is Go's, not the C++ one's:** handles, `(T, error)`, sentinel errors, `context` where
  waiting can be abandoned. Replacing the C++-shaped first version changed none of the conclusions
  above. ([The shape of the Go code](#the-shape-of-the-go-code))

### Where each step is answered

| Step | Section |
|---|---|
| The API, and what it does not copy from C++ | [The shape of the Go code](#the-shape-of-the-go-code) |
| G1 — the runtime already wrote this queue | §1 |
| G2 — port the slot protocol | §2 |
| G3 — build the sleep without a futex | §3 |
| G4 — preemption you did not schedule | §4 |
| G5 — close is a panic on the send side | §5 |
| G6 — measure the tail you promised | §6 |
| Verification: the bug × tool table, and what the review found | §7 |
| What I predicted, and where it was wrong | §8 |
| When the channel is the right call | §9 |

## Layout

``` text
go/
  internal/turnstile/  the queue: Ring and its Producer and Consumer handles, the sleep, Event and
                       LineEvent, Variant (the comparison arms and test seams); tests, benchmarks,
                       a runnable Example
  internal/tsc/        the timestamp counter, in assembly, and its calibration
  internal/platform/   the EcoQoS opt-out, thread pinning, process CPU time (Windows)
  internal/bench/      the driver and the channel adapter, seven phases (0 is the instrument, 1–6
                       are G1–G6), --bug; channel tests and benchmarks
  cmd/turnstile/       -> bin/turnstile.exe
```

Run it, from `go/`:

```sh
go vet ./... && go test ./...
go test -run '^$' -bench . -count 3 ./internal/...   # the per-operation costs
go build -o bin/turnstile.exe ./cmd/turnstile
bin/turnstile.exe                       # exit code is the verdict; about three and a half minutes
bin/turnstile.exe -phase 6              # the tail, and the decision
bin/turnstile.exe -bug late-announce    # one row of the bug x tool table (§7)
```

Two kinds of measurement, by what they need:
- **Ordinary Go benchmarks** cover everything one goroutine does, or that many goroutines do
  identically. On Windows `testing.B` times with `QueryPerformanceCounter`, so its clock is fine.
  Their "parallel" rows run one goroutine per P and report `ns/op/goroutine`: what one operation costs
  the goroutine making it while the other 31 make theirs. Go's own `ns/op` for those rows is wall time
  over every goroutine's operations.
- **The binary** covers what a benchmark cannot express: 32 producers and 16 consumers in different
  roles at a paced rate, tail percentiles, child processes with a different `GODEBUG`.

The race detector needs cgo, and cgo needs gcc: clang targeting MSVC rejects the `-mthreads` that cgo
passes. With [WinLibs](https://winlibs.com/) on `PATH`:

```sh
CGO_ENABLED=1 CC=gcc go test -race ./...
CGO_ENABLED=1 CC=gcc go build -race -o bin/turnstile_race.exe ./cmd/turnstile
bin/turnstile_race.exe -phase 2,3,5     # the correctness phases; a race build reports no latency
TURNSTILE_SHOW_RACE=1 CGO_ENABLED=1 CC=gcc go test -race -run RaceDetectorSeesPublishFirst ./internal/turnstile/
```

## The shape of the Go code

The queue is used like this:

```go
q := turnstile.New[turnstile.LineEvent](1 << 16)

go func() { // each consuming goroutine takes its own handle
	c := q.Consumer()
	for {
		e, err := c.Pop() // waits, like a channel receive
		if errors.Is(err, turnstile.ErrClosed) {
			return // closed, and everything accepted before the close has been taken
		}
		handle(e)
	}
}()

p := q.Producer() // and so does each producing goroutine
err := p.TryPush(e)                // reject on full: nil, ErrFull or ErrClosed
err = p.Push(e)                    // block on full: nil or ErrClosed
err = p.PushContext(ctx, e)        // block on full, until ctx is done
q.Close()
```

**Why the API is shaped this way, against what the C++ version does:**
- **Handles instead of state passed on every call.**
  - **The C++ way:** every call took an `OpStats&`.
  - **What a handle owns:** a goroutine's counters, the timer its parks reuse, and its place in the
    sleepers' bitmap.
  - **What that fixed:** a review of the first version flagged two traps. An `OpStats` shared by two
    goroutines would confuse the sleepers' bitmap, and so would one used on two queues. That version
    could only panic when it noticed. With handles, neither mistake can be written.
  - **The limit:** at most 64 producers and 64 consumers per ring, checked when the handle is made.
- **Values returned, errors as errors.**
  - **The C++ way:** `pop(T& out)` returned an enum.
  - **The Go way:** `Pop() (T, error)` and `TryPop() (T, bool)`, with `ErrFull` and `ErrClosed` as
    sentinels.
  - **The bug it removed:** the first version's out-parameter caused a measured one: passing `&e`
    through an interface method moved `e` to the heap, one allocation per event, until the driver
    declared it outside its loop. Returning the value has no such trap.
- **A context where waiting can be abandoned, and not where it cannot.**
  - **`PushContext(ctx, v)` reserves by compare-and-swap.** It looks first and claims second, so it can
    give up at any moment.
  - **`Push(v)` reserves with a ticket**, one `Add` that never fails. A ticket cannot be handed back,
    so `Push` takes no context: once it holds a position it waits for that slot. That is the same
    contract as a channel send, and the ticket is what met the brief (§6).
  - **Pops** take a context because a pop can always give up.
- **The comparison arms and test seams are a separate constructor.**
  - **What they are:** the shared token, spinning or non-spinning consumers, unzeroed slots, the hooks
    and the deliberate bugs.
  - **Where they live:** `NewVariant(n, Variant{...})`. `New` takes no options.
  - **What they cost `New`'s ring:** one predictable nil check per push and per pop. C++ compiles them
    out as template parameters; Go has no equivalent short of build tags.
- **Padding is the element's job.** Go cannot size padding from a type parameter, so the ring stores
  each value next to its 8-byte sequence and lets the element say how big it is. `LineEvent` pads an
  `Event` so that a slot is exactly one cache line, and a compile-time check fails the build if that
  stops being true. `Ring[Event]` is the packed comparison.
- **The channel is not wrapped in the queue package.** The benchmark adapts it to the driver's
  interface. Code that wants a channel should use one.

The change did not move any conclusion. Every table below was measured again on this API.

---

## 1. The runtime already wrote this queue (G1)

`make(chan Event, 65536)` is a bounded MPMC queue. `runtime.hchan` is a ring buffer under one runtime
mutex, with a queue of parked senders and one of parked receivers. Everything the main kata spends
steps 2–8 on happens inside that lock. A send takes `c.lock` (`runtime/chan.go:222`), copies the
event into the buffer, advances the index and unlocks. So **reservation and publication are one
critical section**, and "reserved but not yet published" is never a state any goroutine can see.

**Two details change what "accepted" means.**
- **A send can skip the buffer entirely.** When a receiver is already parked, `chansend` copies the
  event straight onto that receiver's stack (`chan.go:229`). The event was never "in the queue". So
  "accepted" is best defined as **the send returned**, which is the only point a producer observes.
- **A refusal never takes the lock.** A non-blocking send to a full channel returns at the lock-free
  full check (`chan.go:213`), in 2.0 ns. With every P doing it at once it costs each goroutine 6.7 ns,
  because they share a line they only read.

| one goroutine, nothing contended (`BenchmarkChannel`, `BenchmarkPushPop`) | per call |
|---|---|
| send, then receive — room, nobody parked | 27.6 ns |
| `select { send; <-done }`, then receive | 50.2 ns |
| non-blocking send to a full channel | 2.0 ns |
| non-blocking receive from an empty channel | 2.6 ns |
| the ring: `TryPush`, then `TryPop` | 33.5 ns |

**Saturated, the channel is not slow.** Time per event, no work per event, median of 3:

| senders \ receivers | 1 | 4 | 16 |
|---|---|---|---|
| 1 | 146 ns | 270 ns | 356 ns |
| 8 | 258 ns | 180 ns | 220 ns |
| 32 | 347 ns | 377 ns | 159 ns |

At 32 × 16 that is 6.29 M/s, against 8.08 M/s for the ring in the same phase. In §6's saturated table
the channel reached 6.43 M/s with a bare send, and in the other run of the pair 6.99 M/s with the
non-blocking-first send, against the ring's 7.12. The raw ceiling is not the problem. Saturated, every
receive also hands a parked sender's event into the buffer, under one lock acquisition.

**At the brief's workload it falls over.** At 32 × 16 at 5 M/s, block on full, consumers spending
1 µs per event, median of 3:

| send | achieved | push p50 | **p99** | p99.9 | behind schedule, p99 | e2e p99 |
|---|---|---|---|---|---|---|
| bare `ch <- e` | 1.67 M/s | 198 ns | 515 µs | 921 µs | 2.00 s | 46.8 ms |
| `select { ch <- e; <-done }` | 2.19 M/s | 82 ns | 18.6 µs | 5.85 ms | 1.33 s | 23.4 ms |
| non-blocking first, then that select | 1.95 M/s | 79 ns | **515 µs** | 1.41 ms | 1.55 s | 41.6 ms |

None of them keeps up. "Behind schedule" is the coordinated-omission view: from when an event could
first have been pushed to when it was accepted. At the 99th percentile, events were accepted more than
a second after they were due, in a one-second run.

**The cause is the receive.** One receive was timed by itself, with 16 consumers and a buffer kept
full by producers that reject and yield, so every receive finds an event waiting:

| producers | consumer work | throughput | receive p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1 | none | 6.94 M/s | 33 ns | 64.3 µs | 156 µs |
| 1 | 1 µs | 2.91 M/s | 138 ns | 115 µs | 230 µs |
| 8 | none | 12.88 M/s | 31 ns | 3.60 µs | 312 µs |
| 8 | 1 µs | 2.55 M/s | 138 ns | 210 µs | 433 µs |
| 32 | none | 1.89 M/s | 43 ns | 35.6 µs | 433 µs |
| 32 | 1 µs | 1.53 M/s | 191 ns | 64.3 µs | 420 µs |

With one producer, sixteen consumers alone give a receive whose p99 is above 100 µs. That is fifty
times the brief's whole budget, spent getting an event that was already there.

The runtime's lock-contention profile of the brief's run is consistent with that. It needs
`GODEBUG=runtimecontentionstacks=1` to name the waiter, so it runs as a child process.
- **The estimate:** 218 s of waiting for runtime locks across a 3.27 s run. 59 % of it was in channel
  receives, 40 % in sends, and 1 % elsewhere.
- **Why the total is not a count:** the runtime does not time each waiter. At a contended unlock it
  multiplies the mean wait of the oldest and newest waiter by the number waiting
  (`runtime/lock_spinbit.go`, in `unlock2`). The total can therefore exceed goroutines × seconds, and
  the split is the finding.

The mutex is the spin-bit lock that Go 1.24 introduced. Its own comment describes the design: one
waiting M at a time may spin, and "most other Ms must attempt to spend their time sleeping". On
Windows that sleep is an OS semaphore. I did not instrument the sleep itself, so the link between it
and the tails above is an inference, but it is the one the source and both measurements point to.

**The answer to G1.**
- **What a channel answers by construction:**
  - step 2, because publication is the unlock;
  - step 3, because there is no per-slot state to get wrong;
  - step 5, because parking happens under the same lock as the check;
  - step 4's reject policy, as `select` with `default`.
- **Step 8 is only half answered.** Close is not idempotent and panics senders, so shutdown needs a
  protocol (§5).
- **What it cannot be configured to meet:** 5 M/s sustained at 32 × 16 with consumers doing work, and
  with it the 2 µs p99. Its lock is not a parameter.
- **No allocation on the hot path is met:** 0 allocations per event and 0 collections across every
  paced run. Parked senders reuse cached `sudog`s.

The number that decides whether to go further is the p99 at 32 × 16: 515 µs against 2 µs.

## 2. Port the slot protocol (G2)

[`internal/turnstile/ring.go`](internal/turnstile/ring.go) is the C++ `turnstile::Queue`'s protocol
in Go, with no `unsafe`.

**What survives:**
- **The lap-stamped sequence and the four tests on it.** Each slot holds `seq == p` when free,
  `p + 1` when published and `p + N` when released.
- **The closed bit in the tail word.**
- **The CAS reservation and the ticket,** sharing that word: `TryPush` and `PushContext` use the
  first, `Push` the second.
- **The signed difference,** written `int64(seq - pos)`. Go defines unsigned wraparound, so the
  subtraction is correct across a wrap with no cast-order subtlety. The C++ track needs "subtract
  first, convert second" to stay out of undefined behaviour.

**What disappears is every ordering, and the step-5 fence with them.** The Go memory model
(`go_mem.html`) says:

> If the effect of an atomic operation *A* is observed by atomic operation *B*, then *A* is
> synchronized before *B*. All the atomic operations executed in a program behave as though executed
> in some sequentially consistent order.

- **The value can be a plain field.** A plain write sequenced before the sequence store is visible
  to a plain read sequenced after a load that observes it.
- **Both handoffs through a slot are ordered without annotation:** to the consumer, and back to the
  next lap's producer.
- **The store-buffering outcome that loses a wakeup cannot be written.** It needs two loads that each
  miss the other side's store, and no single total order of four operations produces that.
- **The cost:** every publication compiles to `XCHGQ` (checked in the disassembly), where the C++
  release store is a plain `MOV`.

**Generics, and the line a slot sits on.** `Ring[T any]` stores each value next to its sequence and
leaves the size to the element.
- **The layout check fails the build, not a benchmark.** `LineEvent` pads an `Event` to 56 bytes, so
  a slot is one line. `var _ [64 - unsafe.Sizeof(slot[LineEvent]{})]byte` and its mirror stop
  compilation if that stops being exact.
- **The padding's cost, saturated at 32 × 16:** 7.54 M/s for `Ring[LineEvent]` against 4.19 M/s for
  `Ring[Event]`, packed (7.12 against 5.35 in the other run).

**What a consumed slot must look like.** A consumer that leaves a pointer in its slot keeps the
referent alive until the next lap overwrites it. That is 13 ms at 5 M/s, and never on an idle queue.
The test pushed 1,000 events, each pointing at 1 KiB, consumed them, and ran a GC:

| | referents still alive |
|---|---|
| ring, consumed slot zeroed | 0 of 1000 |
| ring, consumed slot left as it was (`Variant{Retain: true}`) | 1000 of 1000 |
| channel | 0 of 1000 — `chanrecv` clears the slot it read (`chan.go:621`) |

**The invariants, on every arm.** Phase 2 runs each arm and checks it under the C++ track's labels:
- every accepted event consumed exactly once;
- none read torn;
- per consumer, each producer's events in order;
- with no consumer, exactly 65,536 accepted.

The arms:
- at 32 × 16 on 65,536 slots: both blocking pushes, the reject policy, half of each, consumers that
  never sleep, and packed slots;
- four-slot rings at 8 × 8, where a lap wraps every four events;
- the channel three ways.

Every one passed, in both runs.

**The race detector, and what it cannot say.** It reports a data race when the publication comes
before the payload write (§7), so a clean run is not vacuous. It can say nothing about memory
ordering, because there is none to get wrong: every atomic is sequentially consistent, and it judges
only plain accesses. The bug C++ cannot catch with ThreadSanitizer, a missing fence, has no Go
spelling. Go's version of that bug is two statements in the wrong order, and the race detector
cannot see that either (§3).

## 3. Build the sleep without a futex (G3)

Go has no wait-on-address. What it has for sleeping:
- a channel receive;
- `sync.Cond`, which needs a `Locker` and has no timeout;
- `select`, which can wait on a token, a timer (the backstop) and a context's `Done` at once. That is
  how a park gets its backstop and `PushContext` and `PopContext` their deadlines.

**With nobody asleep — which at 5 M/s is nearly every push — the wake decision must not touch a
lock** (`BenchmarkWakeDecision`, `BenchmarkWakeWithoutACount`):

| a push's wake decision, nobody asleep | alone | per goroutine, every P at once |
|---|---|---|
| a bitmap of sleepers (the ring's) | 1.0 ns | 4.6 ns |
| a sleeper count, in front of one shared token | 1.0 ns | 4.2 ns |
| a token on every push, the token resting in the channel | 2.8 ns | 6.1 ns |
| a token on every push, the token never resting | 13.3 ns | 558 ns |
| `sync.Cond`: Lock, Signal, Unlock | 10.1 ns | 1.65 µs |

The resting state of a coalescing token decides the cost. A token left in a full channel makes each
send fail at the lock-free full check. A token that is always taken makes every send succeed, and
take the lock: 558 ns each with 32 producers at it.

**With someone asleep, a wake costs about 2 µs whatever carries it.** Over 2,000 trials, the waker's
own time and the time until the sleeper ran:

| | waker p50 | p99 | sleeper runs, p50 | p99 |
|---|---|---|---|---|
| a push, to a consumer parked in `Pop` | 1.90 µs | 4.44 µs | 2.43 µs | 6.56 µs |
| a send to a receiver parked on it | 1.64 µs | 2.86 µs | 1.80 µs | 3.17 µs |
| `sync.Cond.Signal` | 1.48 µs | 4.02 µs | 1.64 µs | 4.23 µs |

It is the same order as the C++ futex wake, and the same conclusion follows: the expensive case has to
be rare. A woken consumer that finds more behind its event wakes the next sleeper itself, so a burst
costs the producers one wake.

**Sixteen sleepers: one token channel, or sixteen.**
- **The obvious design** is one token channel with a sleeper count in front of it, plus a closed
  channel for the broadcast. It is correct, and it is `Variant{SharedToken: true}`.
- **What it costs:** every sleep's `select` locks both shared channels, and every waking push locks
  one of them. At low rates nearly every push finds a sleeper, so those locks are as contended as the
  channel's own.

The ring instead gives each sleeper a token channel of its own:
- **Announcing:** a sleeper sets its bit in an `atomic.Uint64` and re-checks the ring before sleeping
  on its own channel.
- **Waking:** a producer reads the bitmap, clears one sleeper's bit with `And` (whoever clears it owns
  the wake), and sends to that sleeper's channel.
- **The result:** each channel's lock is shared by two goroutines, and a sleep selects only on its own
  token, its own timer and its caller's context.

The comparison ran at 32 × 16, 1 µs per event, with the queue empty most of the time; median of 3:

| rate | design | push p50 | **p99** | p99.9 | e2e p99 | CPUs |
|---|---|---|---|---|---|---|
| 20 k/s | ring, a token per sleeper | 191 ns | **2.54 µs** | 10.2 µs | 7.20 µs | 0.4 |
| 20 k/s | ring, one token, shared | 291 ns | 8.04 µs | 37.2 µs | 8.47 µs | 0.4 |
| 20 k/s | ring, consumers never sleep | 224 ns | 767 ns | 2.86 µs | 1.27 µs | 15.9 |
| 20 k/s | channel, its own parking | 224 ns | 2.75 µs | 11.0 µs | 5.93 µs | 0.1 |
| 200 k/s | ring, a token per sleeper | 224 ns | **1.90 µs** | 7.62 µs | 17.8 µs | 1.2 |
| 200 k/s | ring, one token, shared | 251 ns | 88.0 µs | 183 µs | 10.6 µs | 1.7 |
| 200 k/s | ring, consumers never sleep | 178 ns | 608 ns | 1.80 µs | 5.50 µs | 16.0 |
| 200 k/s | channel, its own parking | 251 ns | 25.4 µs | 64.3 µs | 77.9 µs | 0.7 |

At 200 k/s, 58 % of pushes woke someone. The other run of the pair gave the same two 200 k/s figures,
88.0 µs and 1.90 µs. The runtime's contention profile of the two ring designs at 200 k/s, each run as
a child:
- **one shared token:** 6.66 s of estimated waiting, 79 % of it inside sleepers' selects and 21 %
  inside wakes' sends;
- **a token per sleeper:** none to two decimal places.

Spinning consumers buy the lowest tail for 16 CPUs. That is the bill the brief's "consumers should
sleep" exists to refuse.

**The lost wakeup, engineered.** This ports the C++ track's race:
- **The setup:** one consumer, one producer and an eight-slot ring. The consumer's parking hook
  announces itself and pauses a random spread, and the producer, watching for the announcement,
  pauses its own random few and pushes.
- **What counts as lost:** an iteration where the consumer slept through a push whose notify had
  returned more than 1 ms before the 2 ms backstop.

Go has no fence to remove, so the bug run swaps two statements, re-checking before announcing:

| spread | announce, then re-check: lost | re-check, then announce: lost |
|---|---|---|
| 64 | 0 | 3 |
| 256 | 0 | 5 |
| 1024 | 0 | 3 |
| 4096 | 0 | 1 |

That is 0 lost in 80,000 iterations in the real order, and 12 with the statements swapped (25 in the
other run). The race detector sees nothing in either. A reviewer reordering two lines for readability
would introduce it, with no atomic in sight to warn them.

One measurement problem turned up here:
- **The symptom:** an early version counted false losses. A select with several cases ready picks one
  at random. A consumer that reached its select late, more than 2 ms after arming the timer, could find
  its token and the expired timer both waiting and take the timer.
- **The fix:** the sleep now takes the token as well when the timer wins. The wake did arrive, and left
  in the channel it would end the next sleep early. Since then the real order has lost nothing in any
  run.

## 4. Preemption you did not schedule (G4)

Since Go 1.14 the runtime can stop a goroutine at nearly any instruction. That is asynchronous
preemption, driven by `sysmon` once a goroutine has run for `forcePreemptNS`, 10 ms
(`runtime/proc.go:6390`). So a producer can be descheduled inside its window by the runtime as well
as by the OS. A preempted goroutine goes to the global run queue, and a P looks there only when its
own queue is empty, or on every 61st schedule.

**Every P spinning.** In this experiment:
- **The producer** reserves a slot and calls `runtime.Gosched` inside its window (a `Variant` hook),
  which puts it on the global queue as a preemption would.
- **The consumers:** twelve wait for that slot on four Ps, so a P that looks for work finds a spinner
  queued before it finds the producer. The producer starts them itself, because a `go` statement
  queues the new goroutine on the creating P. A first version started them from elsewhere, and idle Ps
  stole them; the producer's P then found nothing queued and picked the producer straight back up.
  That version measured nothing.
- **The measure:** how long until the producer publishes, median and worst of 5. Spinners give up
  after 1 s.

| how consumers wait | async preemption on | worst | off | worst |
|---|---|---|---|---|
| a tight spin, no calls in the loop | 20.6 ms | 95.5 ms | **1000 ms** | 1000 ms |
| `Pop`, spin only (`Variant{Spin: true}`) | 20.7 ms | 21.1 ms | 13.4 ms | 18.5 ms |
| `Pop`, spin with `Gosched` | 10.1 µs | 30.1 µs | 9.06 µs | 12.5 µs |
| `Pop`, spin, then park (the ring's) | **12.3 µs** | 15.6 µs | 19.1 µs | 138 µs |

The other run of the pair gave the same shape: 20.6 ms and 1000 ms for the tight spin, 16.5 µs for
the ring's own `Pop`.

**What the table says about each way of waiting.**
- **A loop with no calls can only be stopped asynchronously.** With asynchronous preemption off it
  is never stopped: the producer ran again only when the spinners gave up. That is what
  `asyncpreemptoff` breaks, and it is also what a stop-the-world collection would wait on.
- **`Pop`'s spin calls functions.** Each call's prologue checks the preemption flag that `sysmon`
  sets, so it is stopped cooperatively whichever way the setting is. It still holds every P for tens
  of milliseconds, which is priority inversion with no priorities in the program.
- **Yielding on every spin hands the P back.** It costs the spinner a trip through the scheduler on
  every iteration.
- **Parking frees the P within microseconds.** Whether the 128-PAUSE spin before the park earns its
  keep is measured in §6; at this brief's rate it does not, measurably.

**`runtime.LockOSThread` does not help.** It ties a goroutine to an OS thread. It gives that thread
neither a P nor a core, and it prevents neither kind of preemption. I did not measure it.

**GOMAXPROCS against the brief's 48 goroutines on 32 logical CPUs.** The ring's `Push` at 5 M/s,
median of 3:

| | push p50 | **p99** | p99.9 | behind p99 | consumers parked on a hole, per run |
|---|---|---|---|---|---|
| GOMAXPROCS 16 | 165 ns | 952 ns | 1.80 µs | 129 µs | 1,709 |
| GOMAXPROCS 32 | 211 ns | 1.06 µs | 1.90 µs | 105 µs | 1,432 |
| GOMAXPROCS 48 | 185 ns | 899 ns | 1.59 µs | 102 µs | 1,899 |

I expected 48 to bring back the oversubscription that C++ measured, 48 threads on 32 CPUs, and to be
the worst. It was no worse than 32, in both runs of the pair. My explanation, which this table does
not test, is that fewer than 32 of the 48 are runnable most of the time. Producers here push about a
hundred events and nap, and consumers park. The C++ harness's bursts of 5,800 events (§6) keep all 48
runnable for much longer.

Step 6's arithmetic is unchanged: 65,536 slots is 13 ms at 5 M/s, against `sysmon`'s 10 ms slice. The
"parked on a hole" column counts consumers that went to sleep with a position below the tail reserved
and unpublished: one to two thousand times a second, in every arm.

**A panic inside the window.** `TryPushFunc` lets a caller build the value in place, and a deferred
function pays the reservation with a tombstone if the build panics.
- **Why it cannot be forgotten:** a `defer` runs on a panic and on a normal return alike, so there is
  no path on which the debt is forgotten. The C++ `catch` has to be written for exactly the path
  nobody tests.
- **Tested:** `TestPanicInWindowPaysTombstone`. The panic reaches the caller, and the events behind
  the tombstone are delivered.
- **Measured:** a push and pop cost 36.1 ns with the deferred fill, against 33.5 ns with `TryPush`
  (`BenchmarkPushFuncPop`, `BenchmarkPushPop`).
- **A fix from review:** the deferred tombstone also wakes consumers. Events published behind it
  become reachable at that moment.

## 5. Close is a panic on the send side (G5)

A send on a closed channel panics, `panic("send on closed channel")`, and
`TestSendOnClosedChannelPanics` recovers it to show it. With 32 senders, the data channel therefore
cannot be the shutdown signal. A close racing a send turns step 8's stranded slot into a crashed
process. The protocol, in the benchmark's channel adapter:
- **`Close` closes a separate `done` channel.** Producers that see it stop.
- **The driver counts producers out with a `sync.WaitGroup`.** Only once the last one has returned
  from its last push does it close the data channel.
- **Receivers drain what the buffer holds, then see it closed.** That is the consumers' termination
  condition, and no event accepted before it is lost.

**A select whose cases are both ready picks one at random**, so `select { ch <- e; <-done }` can
accept an event after shutdown began. That is harmless if consumers drain after the producers are
counted out, and fatal if they stop at the signal. Measured: 32 × 16 at 1 M/s, closed 200 ms in, five
rounds per arm:

| | refused as closed | accepted after `Close` returned | drain after the close, worst |
|---|---|---|---|
| ring, `Push` (ticket) | 160 of 160 | 0 | 11.2 µs |
| ring, `PushContext` (CAS) | 160 of 160 | 0 | 4.94 µs |
| ring, `TryPush` | 160 of 160 | 0 | 57.4 µs |
| channel, non-blocking first, then select | 160 of 160 | 0 | 6.52 ms |
| channel, select over send and done | 160 of 160 | **165** | 731 µs |
| channel, reject | 160 of 160 | 0 | 152 µs |

Every producer was refused once, and every accepted event was consumed exactly once. Only the
textbook select let pushes in after the close. The non-blocking-first send checks an `atomic.Bool`
the close sets before it ever reaches a select.

**What a producer pays per push to learn whether shutdown has begun** (`BenchmarkShutdownCheck`):

| check, while it has not | alone | per goroutine, every P at once |
|---|---|---|
| an `atomic.Bool` load | 1.4 ns | 2.4 ns |
| `select { case <-done: default: }` | 4.8 ns | 9.2 ns |
| `ctx.Err() != nil` | 2.6 ns | 4.8 ns |
| `select { case <-ctx.Done(): default: }` | 6.2 ns | 14.1 ns |
| the same, with `ctx.Done()` read once outside the loop | 5.1 ns | 9.4 ns |

All of them are cheap. `cancelCtx.Err` is an atomic load in this release; its comment says "~5x
faster than a mutex". I expected the blocking `select { ch <- e; <-done }` to be the expensive
spelling, because `selectgo` locks every channel in a select, so all 32 producers share the `done`
channel's lock on every push. Instead it was the fastest of the three channel sends in §1: 18.6 µs at
p99, against 515 µs for the non-blocking-first send. I have not measured why.

**The ring's termination, and the hang review found.** The ring keeps C++'s design: a CAS reservation
cannot happen after a close, because the closed bit is in the word it swaps. A ticket can, because an
`Add` cannot be refused. In C++ a ticket landing after the close paid its slot with a tombstone, and
the port first did the same. That strands a producer:
1. More producers than there are slots pass the ticket pre-check.
2. The ring closes, and its consumers drain and leave.
3. All of them then take tickets.
4. The one whose ticket maps onto a slot an earlier one tombstoned waits for a release that no
   consumer will ever make.

It needs more producers in flight than slots, so at 65,536 slots it cannot happen; on a two-slot ring,
`TestTicketAfterCloseDoesNotWaitForASlot` reproduced it every time.
- **The fix:** `Close` records where the tail stood, from the old value its `Or` returns. Consumers
  drain exactly to that position, and `Pop` returns `ErrClosed` there. A ticket past it is refused
  without touching its slot, because nobody is owed it.
- **The tombstone remains** for the one debt that is owed and cannot be paid with a value: a fill that
  panicked.
- **The C++ queue still has the strand.** Its rings in this kata are all larger than its producer
  count, so no run of it can hit it.

## 6. Measure the tail you promised (G6)

**`testing.B` for costs, a harness for the tail.** `RunParallel` runs GOMAXPROCS goroutines, all
executing one body. That is right for the per-operation tables above, and wrong for 32 producers and
16 consumers doing different things at a paced rate. So the driver
([`internal/bench/driver.go`](internal/bench/driver.go)) is the C++ driver ported:
- paced producers with staggered starts;
- the push time and the "behind schedule" time, the coordinated-omission view, recorded per push;
- a hashed exactly-once tally;
- a per-consumer order check.

Each producer and consumer goroutine takes its own handle, as the API asks.

**The instrument comes first.** Phase 0 prints it before anything depends on it.
- **`time.Now` is unusable here.** Its smallest observable step was **436 µs**, and 403,183 calls
  across 5 ms returned **5 distinct readings**. On Windows its monotonic reading is
  `KUSER_SHARED_DATA.InterruptTime` (`runtime/time_windows_amd64.s`), which advances at the clock
  interrupt.
- **So the driver reads the timestamp counter** through four lines of assembly. It runs at 2.419 GHz,
  is invariant, and costs 6.15 ns per read; a second 200 ms window agrees with its calibration to
  0.0006 %. Cross-core skew was +5 ns from CPU 0 to 2 and −21 ns from CPU 0 to 31.
- **The sleep floors matter for every later table.** `time.Sleep(1µs)` returned after 595 µs at the
  median, and a 50 µs timer in a select after 515 µs.

**The arms, at the brief's rate.** 32 × 16 at 5 M/s, block on full, 1 µs per event, median of 3:

| | achieved | push p50 | **p99** | p99.9 | behind p99 | e2e p99 |
|---|---|---|---|---|---|---|
| channel, non-blocking first | 1.97 M/s | 89 ns | 488 µs | 1.46 ms | 1.55 s | 36.4 ms |
| **ring, `Push` (ticket)** | **5.00 M/s** | 165 ns | **899 ns** | 1.64 µs | 91.4 µs | 1.41 ms |
| ring, `PushContext` (CAS) | 4.99 M/s | 1.16 µs | 18.6 µs | 98.2 µs | 11.7 ms | 6.50 ms |
| ring, `Push`, consumers park at once | 5.00 M/s | 198 ns | 1.11 µs | 2.22 µs | 105 µs | 1.03 ms |

The ring allocated nothing and the collector did not run, in this table or in any other paced one.
Each producer woke every 625 µs and pushed 98 events back to back.

**The decision, as written before the run.** The ring is kept only if its worst p99 of the three
repetitions beats the channel's best. It did: 952 ns against 488 µs.
- **Against the brief, `Push`'s p99 at 32 × 16 was 899 ns, and 1.11 µs in the other run of the pair:
  met.**
- **The CAS reservation missed in both,** at 18.6 µs and 15.2 µs. Saturated, it retried 4.3 times per
  push (3.2 in the other run); the C++ track counted 3.4–3.7.

**The spin before parking does not earn its keep here.**
- **The measurement:** consumers that park as soon as the ring is empty (`Variant{NoSpin: true}`)
  measured 1.11 µs against the spinning ring's 899 ns. In the other run both measured 1.11 µs.
- **Under the C++ harness's bursts (below):** 2.54 µs against 2.22 µs, and 2.43 µs against 2.33 µs.
- **The verdict:** that is within the run-to-run spread, so the spin — 128 PAUSEs, carried over from
  the C++ design — is not measurably buying anything at this rate. I left the default as it is rather
  than change it after the measuring runs. Dropping it would also remove the ring's one use of
  assembly on the consumer path.

**That verdict holds at this machine's normal clock.** During one earlier full run, of the first
version of this code, the laptop's power management held CPU 0 at about a third of its usual clock. A
PAUSE read 180 ns where it reads 35–49 ns in the final pair, and in that stretch the same
32 × 16 spec measured 9.31 µs. Every phase prints the PAUSE thermometer for this reason.

**The C++ track's other two rows,** same spec:

| | channel p99 | `Push`: achieved | push p50 | **p99** | p99.9 |
|---|---|---|---|---|---|
| 16 × 8 at 5 M/s | 257 µs (at 1.56 M/s) | 5.00 M/s | 145 ns | 582 ns | 1.00 µs |
| 32 × 16 at 1 M/s | 122 µs | 1.00 M/s | 95 ns | 1.06 µs | 2.22 µs |

**The pacer is part of the experiment.** Both harnesses ask each producer to nap 200 µs between
bursts and then push everything that came due.
- **Go:** the runtime sleeps on a high-resolution waitable timer, so each producer woke every 625 µs
  and pushed 98 events.
- **C++:** in the C++ track's own run, each producer woke every 37.2 ms and pushed 5,814 events back
  to back (cpp §10's run), because `sleep_for` on Windows returns tens of milliseconds late.

So the two tracks' tables do not offer the queue the same traffic, and the same arms were rerun here
with a 37 ms nap:

| 37 ms between bursts | achieved | push p50 | **p99** | p99.9 | behind p99 |
|---|---|---|---|---|---|
| channel, non-blocking first | 1.83 M/s | 95 ns | 515 µs | 1.63 ms | 1.72 s |
| ring, `Push` (ticket) | 4.82 M/s | 152 ns | **2.22 µs** | 3.60 µs | 12.6 ms |
| ring, `PushContext` (CAS) | 4.82 M/s | 185 ns | 8.47 µs | 25.4 µs | 50.3 ms |
| ring, `Push`, park at once | 4.83 M/s | 138 ns | 2.54 µs | 4.66 µs | 12.1 ms |

Each producer now woke every 36.8 ms and pushed 5,754 events, the C++ harness's traffic.
- **The ring's p99 more than doubled,** to 2.22 µs (2.33 µs in the other run), and crossed the line.
- **No ratio with the C++ figures:** the C++ ticket's 2.96 µs and 3.60 µs came from other runs.
- **What the measurement shows:** most of the distance between "C++ misses at 32 × 16" and "Go meets
  it" is how the two harnesses' producers sleep. The brief's 5 M/s says nothing about burst shape,
  and the tail depends on it.

**The saturated ceiling,** 32 × 16, 250,000 events per producer, nothing done per event:

| | throughput | retries per push |
|---|---|---|
| channel, bare send | 6.43 M/s | — |
| channel, non-blocking first | 6.01 M/s | — |
| ring, `Push`, `Ring[LineEvent]` | 7.54 M/s | 0 |
| ring, `Push`, `Ring[Event]`, packed | 4.19 M/s | 0 |
| ring, `PushContext` | 4.83 M/s | 4.32 |

**Asynchronous preemption, on and off.** It is read when the runtime starts, so each run is a child
process, the pairs launched back to back:

| | p99 on | p99 off |
|---|---|---|
| ring, `Push` | 793 ns | 846 ns |
| channel | 515 µs | 515 µs |

Asynchronous preemption made no measurable difference to either queue at the brief's rate. What it
does matter for is §4's spinners.

**The race detector changes timing and scheduling.** So a race build reports no latency at all, and
divides the correctness phases' event counts by ten. It runs phases 2, 3 and 5 (§7).

### Measurement problems, and what they cost

- **`time.Now`.** Its smallest step is about half a millisecond, so every latency here would have read
  zero or a whole number of milliseconds. Fixed with the counter, printed in phase 0.
- **One heap allocation per event, from escape analysis.**
  - **The cause:** the first version's consumer passed `&ev` through an interface method, and the
    compiler cannot keep such a variable on the stack.
  - **The effect:** 1.000 allocations per event, and 12 to 37 collections per one-second run.
  - **What fixed it:** hoisting the declaration out of the loop, and then the API, which returns the
    value instead. The allocations-per-event column exists because of this.
- **A benchmark whose state filled up.**
  - **The symptom:** the never-resting token's channel had room for 2²⁶ sends. At `RunParallel`'s
    speeds `b.N` passed that, after which every send failed at the full check, and the row measured
    the resting case: 0.19 ns/op.
  - **The fix:** the channel is now sized from `b.N`. Its zero-size elements make that free.
  - **A related trap:** a `b.Loop` benchmark does not know `b.N` in advance, so that one uses the
    classic loop.
- **The thermometer read whichever core it landed on:** 26 to 55 ns from one phase to the next. It now
  pins itself to CPU 0.
- **The first hole experiment measured nothing** (§4).
- **The runtime's contention totals are estimates** that can exceed goroutines × seconds (§1).
- **A select picks among ready cases at random,** which first made a late wake look like a lost one
  (§3).
- **The arrival process differs between the two harnesses** by a factor of 60 in burst size (above).
- **The clock moves under sustained load** (above). The final pair's PAUSE readings stayed between 35
  and 49 ns.

## 7. Verification

**Tests**, under `go test` and `go test -race`, in [`internal/turnstile/`](internal/turnstile/) and
[`internal/bench/`](internal/bench/):
- lap wraps on two- and four-slot rings, for `TryPush`, `Push` and `PushContext`;
- never overwriting with no consumer;
- close against reserve for every push;
- a ticket taken after the close not waiting for a slot;
- `PushContext` and `PopContext` honouring their deadlines;
- a 65th consumer refused when its handle is made;
- a panic inside the window paying its slot;
- the collector probe;
- a send on a closed channel panicking;
- the shutdown protocol with 32 senders;
- the package's `Example`, whose output is checked.

All pass, and pass under the race detector.

**The bug × tool table.** Each deliberate bug is one line of the protocol made wrong, through
`NewVariant`:
- **The stress:** 8 × 8 on a 64-slot ring, 25,000 events each, with a 30 s hang limit.
- **The race detector:** the same stress in the race build.
- **The engineered race:** §3's race, 4 × 20,000 iterations.

| bug | stress, x86-64 | race detector | engineered race |
|---|---|---|---|
| none | clean | 0 reports | 0 lost |
| publish before writing the payload | **486 events torn** | **3 reports** | — |
| reserve a slot still holding the previous lap | **hung** | **3 reports**, then hung | — |
| re-check, then announce | clean | 0 reports | **18 lost** |

The race detector is the instrument for a payload published too early, and it reported the overwrite
as well. For the swapped sleep it is blind, and so is the stress: its consumers never sleep long
enough for a lost wakeup to be more than a delay. Only the engineered race sees that bug. The
race-detector build ran phases 2, 3 and 5 with 0 reports.

**Independent review.** A reviewer was given the first version's code, not my conclusions. It found
no lost wakeup and no exactly-once violation. It did find these:
- **The ticket-close strand** (§5), now reproduced by a test and fixed.
- **The panic tombstone did not wake consumers** (§4). Fixed.
- **A wake claimed for a sleeper whose re-check then found work was dropped.** For a producer blocked
  on full, woken only once per 64 releases, that could mean sleeping through free slots until its
  50 ms backstop. A cancelled sleep now passes such a wake on.
- **Two API traps**, now impossible to write with handles
  ([The shape of the Go code](#the-shape-of-the-go-code)). A 128-producer limit in the order check
  also failed runs spuriously; the driver now refuses such a run up front.
- **A harness gap.** The driver never asserted that every producer finished, so a producer stuck in a
  run without an early close went unnoticed. It now does. The reviewer also noted that the accounting
  check `offered == accepted + rejected + refused` is the harness checking itself and cannot catch a
  queue bug; it is kept, and labelled as such in the code.

## 8. Predictions against results

Written before the first measuring phase ran:

| prediction | result |
|---|---|
| the channel misses the 2 µs p99 at 32 × 16 by 10–50 µs | missed by much more: 488–515 µs, and it could not sustain the rate at all |
| saturated, the channel is below 5 M/s at 32 × 16 | wrong: 6.0–7.0 M/s; saturated is not where it fails |
| the textbook `select` is the slowest send | wrong: lowest p99 of the three (18.6 µs), and the only one that accepts after close |
| the ticket ring's p99 at 32 × 16 is 1.5–3 µs | 899 ns and 1.11 µs; 2.22–2.33 µs under the C++ harness's bursts |
| the CAS ring misses at 10–30 µs | 15.2–18.6 µs |
| the wake decision with nobody asleep costs ~2 ns | 1.0 ns alone, 4.6 ns each with every P at it |
| a wake costs the waker 1–5 µs | 1.5–1.9 µs at the median |
| at 20 k/s a sleeper count and a token on every push perform alike | beside the point: the shared token, not the count, was the problem |
| a spinning hole lasts 10–20 ms with async preemption, forever without | 20.6 ms with, until the 1 s give-up without |
| `Pop` spinning, async preemption off: 10–20 ms | 13.4–15.1 ms |
| GOMAXPROCS 48 brings back oversubscription and is worst | wrong: 48 and 32 were equal in both runs |
| padding is worth 20–50 % | worth 1.3–1.8× |
| asynchronous preemption off changes little for the ring | right |

## 9. When the channel is the right call

When the rate is lower than this brief's, or the budget is milliseconds rather than microseconds. At
1 M/s the channel kept up, but its p99 was still 122 µs. It is also right when the people maintaining
the code are not the ones who wrote it:
- **One ring needs** the sequence protocol, a bitmap waiter whose correctness rests on statement
  order, and a close position.
- **The channel needs** a WaitGroup.

Batching is the lever the C++ track found for its mutex, and a `chan []Event` is the Go equivalent. I
did not measure it. That is the first thing to try before writing a ring, and the next experiment
here.

### Known gaps

- **One unexplained hang.**
  - **What happened:** once, in the first version and before the last change to its sleep, phase 3's
    low-rate section hung until the watchdog ended the run (exit 3). No goroutine stacks were
    captured.
  - **Since then:** phase 3's low-rate section has run 34 more times without one, two of them on this
    API. The watchdog now writes every goroutine's stack before exiting, so a recurrence will say
    where.
  - **Status:** I could not construct the interleaving from the code, and do not claim the later
    changes fixed it.
- **The spin before parking is kept, though it does not measurably pay** (§6).
- **No model checker.** Go has no Relacy or GenMC equivalent in common use, so the lap wrap is covered
  by stress on two- and four-slot rings and by the race detector, not exhaustively.
- **AArch64 not run.** There, Go's sequentially consistent atomics are `LDAR`/`STLR` pairs.
- **A per-producer channel arm** — the C++ track's §13 experiment — is not built in Go.
- **The C++ queue keeps the ticket-close strand** (§5).

---

## Appendix: Glossary

**Async preemption** — the runtime stopping a goroutine at an arbitrary instruction, by signal (or
`SuspendThread` on Windows), once it has run 10 ms. `GODEBUG=asyncpreemptoff=1` turns it off.

**Coordinated omission** — measuring only how long the operations that ran took, rather than how late
each event became. The "behind schedule" column is the correction.

**EcoQoS** — Windows' power throttling of background processes: lower clocks, efficiency cores. The
binary and the test binaries opt out at start.

**Escape analysis** — the compiler deciding whether a variable can live on the stack. A variable
whose address goes through an interface method usually cannot.

**G, M, P** — goroutine, OS thread, processor. A P is the right to run Go code; there are GOMAXPROCS
of them. An M needs a P to run a G.

**`hchan`** — the runtime's channel: a ring buffer, two wait queues, and one lock.

**Runtime mutex, spin bit** — the lock inside `hchan` and the scheduler, rewritten in Go 1.24 so that
one waiting M spins while the rest sleep in the OS.

**Store buffering** — two threads each store to one location and then load the other's. If both
loads miss, a wakeup is lost. Sequentially consistent atomics forbid that outcome.

**`sudog`** — the runtime's record of a goroutine parked on a channel, cached per P.

**`sysmon`** — the runtime's monitor thread. It retakes Ps from long-running goroutines, and is what
makes a 10 ms preemption happen.

**TSC** — the timestamp counter, read with `RDTSC`. It is invariant on this CPU, so one calibration
holds.
