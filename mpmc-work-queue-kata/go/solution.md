# A bounded MPMC work queue in Go — the runtime already wrote one, and it does not meet the brief

Answer to [../question.md](../question.md) and the [Go track](question.md), with a working
implementation and benchmarks alongside it. The C++ answer is [../cpp/solution.md](../cpp/solution.md);
the Python track is not answered yet.

Every number below came from running [`bin/turnstile.exe`](cmd/turnstile/), whose exit code is the
verdict, on the same i9-13980HX laptop as the C++ track (8 P-cores, 16 E-cores, 32 logical CPUs) under
Windows 11 with Go 1.25.5. Unless a section says otherwise, figures are from one full run, the second
of two back to back; where the first run of the pair disagrees in a way that matters, both are given.
Arms compared in a table ran interleaved in that run. Nothing here is compared with a C++ figure as a
ratio: those came from other runs, with another harness, and §6 shows the harness alone moves the
answer. Terms of art are expanded in the [Glossary](#appendix-glossary).

---

## The answer, up front

- **The channel answers every correctness step and misses the brief by a factor of 230.** One lock
  makes reservation, copy and publication a single critical section, so nothing in steps 2–8 can go
  wrong. But at 32 × 16 with consumers spending 1 µs per event it moved **1.76 M/s against 5 M/s
  offered**, and its p99 send was **461 µs**. The lock is the reason. One receive, timed by itself
  with events always waiting, has a p99 of 105–244 µs, with one producer, eight or thirty-two. The
  runtime's own contention profile puts 60 % of the waiting in receives and 38 % in sends. (§1)
- **The ring ports intact, gets simpler, and meets the p99.** There are no orderings to choose, the
  payload is a plain field, and consumed slots are zeroed for the collector. At 32 × 16 and 5 M/s its
  p99 push was **1.11 µs** (899 ns in the other run of the pair), with no allocation and no
  collection in the measured window. The rule written before the run held by a factor of 400: the
  ring's worst repetition had to beat the channel's best. (§2, §6)
- **One token channel shared by sixteen sleepers recreates the channel's problem.** At 200 k/s its
  p99 push was **91.4 µs**. A token channel per sleeper, with a bitmap saying who is asleep, brought
  that to **1.53 µs**. The runtime's profile shows 6.79 s of lock waiting for the first and none for
  the second. (§3)
- **A producer descheduled inside its window, with every P held by a spinning consumer, waits for
  the runtime's forced preemption.** That was 31.1 ms at the median for a spin with no calls in it,
  and **forever** — until the spinners gave up at 1 s — with asynchronous preemption off. Consumers
  that spin briefly and then park let it back in **6.53 µs**. (§4)
- **Shutdown is a separate signal, a count of producers, and the data channel closed last.** The
  textbook `select` over the send and the signal accepted 101 events after `Close` returned; every
  other arm accepted none. Review found a hang in the ported ticket close, which the C++ queue shares;
  the Go ring now records where the tail stood at the close and drains to there. (§5)
- **The comparison with the C++ track is a comparison of pacers.** Both harnesses ask producers to
  nap 200 µs between bursts. Go's timer returns in about 0.6 ms; on this machine C++'s `sleep_for`
  returned after 37 ms, so each C++ producer pushed about 5,800 events back to back. Given that same
  arrival process, the Go ring's p99 was **2.01 µs**, twice its own and near the C++ ticket's. (§6)

### Where each step is answered

| Step | Section |
|---|---|
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
  internal/turnstile/  the queue: Ring (the per-slot sequence), Waiter (the sleep), ChanQueue
                       (a channel behind the same interface), Event; and their tests
  internal/tsc/        the timestamp counter, in assembly, and its calibration
  internal/platform/   the EcoQoS opt-out, thread pinning, process CPU time (Windows)
  internal/bench/      the driver, seven phases (0 is the instrument, 1–6 are G1–G6), --bug
  cmd/turnstile/       -> bin/turnstile.exe
```

Run it, from `go/`:

```sh
go vet ./... && go test ./...
go build -o bin/turnstile.exe ./cmd/turnstile
bin/turnstile.exe                       # exit code is the verdict; about three and a half minutes
bin/turnstile.exe -phase 6              # the tail, and the decision
bin/turnstile.exe -bug late-announce    # one row of the bug x tool table (§7)
```

The race detector needs cgo, and cgo needs gcc: clang targeting MSVC rejects the `-mthreads` that cgo
passes. With [WinLibs](https://winlibs.com/) on `PATH`:

```sh
CGO_ENABLED=1 CC=gcc go test -race ./...
CGO_ENABLED=1 CC=gcc go build -race -o bin/turnstile_race.exe ./cmd/turnstile
bin/turnstile_race.exe -phase 2,3,5     # the correctness phases; a race build reports no latency
TURNSTILE_SHOW_RACE=1 CGO_ENABLED=1 CC=gcc go test -race -run RaceDetectorSeesPublishFirst ./internal/turnstile/
```

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
- **A refusal never takes the lock.** A non-blocking send to a full channel returns at the
  lock-free full check (`chan.go:213`), in 4 ns. That stays at 12 ns with 32 goroutines doing it at
  once, because they share a line they only read.

| one goroutine, nothing contended | per call |
|---|---|
| send, then receive — room, nobody parked | 34 ns |
| `select { send; <-done }`, then receive | 69 ns |
| non-blocking send to a full channel | 4 ns |
| non-blocking receive from an empty channel | 7 ns |
| the ring: `TryPush`, then `TryPop` | 39 ns |

**Saturated, the channel is not slow.** Time per event, no work per event, median of 3:

| senders \ receivers | 1 | 4 | 16 |
|---|---|---|---|
| 1 | 162 ns | 277 ns | 377 ns |
| 8 | 260 ns | 169 ns | 307 ns |
| 32 | 358 ns | 285 ns | 180 ns |

At 32 × 16 that is 5.57 M/s, against 8.78 M/s for the ring in the same phase. In §6's saturated table
the channel's bare send reached 7.27 M/s. In the other run of the pair it reached 7.56 M/s, ahead of
the ring's 7.15. The raw ceiling is not the problem. Saturated, every receive also hands a parked
sender's event into the buffer, under one lock acquisition.

**At the brief's workload it falls over.** At 32 × 16 at 5 M/s, block on full, consumers spending
1 µs per event, median of 3:

| send | achieved | push p50 | **p99** | p99.9 | behind schedule, p99 | e2e p99 |
|---|---|---|---|---|---|---|
| bare `ch <- e` | 1.51 M/s | 264 ns | 569 µs | 1.03 ms | 2.32 s | 50.3 ms |
| `select { ch <- e; <-done }` | 2.05 M/s | 99 ns | 23.7 µs | 5.85 ms | 1.50 s | 26.9 ms |
| non-blocking first, then that select | 1.75 M/s | 76 ns | **461 µs** | 1.35 ms | 1.39 s | 43.3 ms |

None of them keeps up. "Behind schedule" is the coordinated-omission view: from when an event could
first have been pushed to when it was accepted. At the 99th percentile, events were accepted more than
a second after they were due, in a one-second run.

**The cause is the receive.** One receive was timed by itself, with 16 consumers and a buffer kept
full by producers that reject and yield, so every receive finds an event waiting:

| producers | consumer work | throughput | receive p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1 | none | 5.09 M/s | 46 ns | 84.7 µs | 217 µs |
| 1 | 1 µs | 2.55 M/s | 158 ns | 135 µs | 271 µs |
| 8 | none | 9.81 M/s | 36 ns | 3.81 µs | 230 µs |
| 8 | 1 µs | 2.21 M/s | 178 ns | 244 µs | 488 µs |
| 32 | none | 1.73 M/s | 51 ns | 45.7 µs | 569 µs |
| 32 | 1 µs | 1.79 M/s | 198 ns | 105 µs | 569 µs |

With one producer, sixteen consumers alone give a receive whose p99 is above 100 µs. That is fifty
times the brief's whole budget, spent getting an event that was already there.

The runtime's lock-contention profile of the brief's run is consistent with that. It needs
`GODEBUG=runtimecontentionstacks=1` to name the waiter, so it runs as a child process.
- **The estimate:** 144 s of waiting for runtime locks across a 2.25 s run. 60 % of it was in channel
  receives, 38 % in sends, and 1 % elsewhere.
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

The number that decides whether to go further is the p99 at 32 × 16: 461 µs against 2 µs.

## 2. Port the slot protocol (G2)

[`internal/turnstile/ring.go`](internal/turnstile/ring.go) is the C++ `turnstile::Queue` in Go, with
no `unsafe`.

**What survives:**
- **The lap-stamped sequence and the four tests on it.** Each slot holds `seq == p` when free,
  `p + 1` when published and `p + N` when released.
- **The closed bit in the tail word.**
- **The CAS reservation for reject and the ticket for block,** sharing that word.
- **The signed difference,** written `int64(seq - pos)`. Go defines unsigned wraparound, so the
  subtraction is correct across a wrap with no cast-order subtlety. The C++ track needs "subtract
  first, convert second" to stay out of undefined behaviour.

**What disappears is every ordering, and the step-5 fence with them.** The Go memory model
(`go_mem.html`) says:

> If the effect of an atomic operation *A* is observed by atomic operation *B*, then *A* is
> synchronized before *B*. All the atomic operations executed in a program behave as though executed
> in some sequentially consistent order.

- **The payload can be a plain field.** A plain write sequenced before the sequence store is visible
  to a plain read sequenced after a load that observes it.
- **Both handoffs through a slot are ordered without annotation:** to the consumer, and back to the
  next lap's producer.
- **The store-buffering outcome that loses a wakeup cannot be written.** It needs two loads that each
  miss the other side's store, and no single total order of four operations produces that.
- **The cost:** every publication compiles to `XCHGQ` (checked in the disassembly), where the C++
  release store is a plain `MOV`.

**Generics, and the line a slot sits on.** `Ring[T, Pad any]` puts a padding type first in each slot.
Go cannot size an array from `T`, so the caller says how much to pad. Padding goes first because a
zero-sized field at the end of a struct gets a byte of padding of its own.
- **The layout check fails the build, not a benchmark.** `var _ [64 - unsafe.Sizeof(slot[Event,
  LinePad]{})]byte` and its mirror stop compilation if an `Event` slot stops being exactly one line.
- **The padding's cost, saturated at 32 × 16:** 8.54 M/s padded against 3.82 M/s packed (7.15 against
  3.81 in the other run).

**What a consumed slot must look like.** A consumer that leaves a pointer in its slot keeps the
referent alive until the next lap overwrites it. That is 13 ms at 5 M/s, and never on an idle queue.
The test pushed 1,000 events, each pointing at 1 KiB, consumed them, and ran a GC:

| | referents still alive |
|---|---|
| ring, consumed slot zeroed | 0 of 1000 |
| ring, consumed slot left as it was | 1000 of 1000 |
| channel | 0 of 1000 — `chanrecv` clears the slot it read (`chan.go:621`) |

**The invariants, on every arm.** Phase 2 runs each arm and checks it under the C++ track's labels:
- every accepted event consumed exactly once;
- none read torn;
- per consumer, each producer's events in order;
- with no consumer, exactly 65,536 accepted.

The arms are both reservations, both policies, both at once, and consumers that never sleep, at
32 × 16 on 65,536 slots; four-slot rings at 8 × 8, where a lap wraps every four events; and the channel
three ways. Every one passed, in both runs.

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
- `select` with a timer, which is how a park gets its backstop and a blocking push its deadline.

**With nobody asleep — which at 5 M/s is nearly every push — the wake decision must not touch a
lock:**

| a push's wake decision, nobody asleep | alone | 32 at once |
|---|---|---|
| a bitmap of sleepers (the ring's) | 3 ns | 7 ns |
| a sleeper count, in front of one shared token | 2 ns | 6 ns |
| a token on every push, the token resting in the channel | 4 ns | 8 ns |
| a token on every push, the token never resting | 16 ns | 685 ns |
| `sync.Cond`: Lock, Signal, Unlock | 12 ns | 1.81 µs |

The resting state of a coalescing token decides the cost. A token left in a full channel makes each
send fail at the lock-free full check. A token that is always taken makes every send succeed, and
take the lock: 685 ns at 32 producers.

**With someone asleep, a wake costs about 2 µs whatever carries it.** Over 2,000 trials, the waker's
own time and the time until the sleeper ran:

| | waker p50 | p99 | sleeper runs, p50 | p99 |
|---|---|---|---|---|
| a token, claimed from the bitmap | 2.01 µs | 7.20 µs | 2.33 µs | 8.04 µs |
| a send to a receiver parked on it | 1.69 µs | 2.86 µs | 1.90 µs | 3.60 µs |
| `sync.Cond.Signal` | 1.64 µs | 4.23 µs | 1.80 µs | 4.87 µs |

It is the same order as the C++ futex wake, and the same conclusion follows: the expensive case has to
be rare. A woken consumer that finds more behind its event wakes the next sleeper itself, so a burst
costs the producers one wake.

**Sixteen sleepers: one token channel, or sixteen.**
- **The obvious design** is one token channel with a sleeper count in front of it, plus a closed
  channel for the broadcast. It is correct.
- **What it costs:** every sleep's `select` locks both shared channels, and every waking push locks
  one of them. At low rates nearly every push finds a sleeper, so those locks are as contended as the
  channel's own.

The fix gives each sleeper a token channel of its own:
- **Announcing:** a sleeper sets its bit in an `atomic.Uint64` and re-checks the queue before
  sleeping on its own channel.
- **Waking:** a producer reads the bitmap, clears one sleeper's bit with `And` (whoever clears it owns
  the wake), and sends to that sleeper's channel.
- **The result:** each channel's lock is shared by two goroutines, and a sleep selects only on its own
  token and its own timer.

The comparison ran at 32 × 16, 1 µs per event, with the queue empty most of the time; median of 3:

| rate | design | push p50 | **p99** | p99.9 | e2e p99 | CPUs |
|---|---|---|---|---|---|---|
| 20 k/s | ring, a token per sleeper | 291 ns | **2.12 µs** | 11.0 µs | 5.29 µs | 0.4 |
| 20 k/s | ring, one token, shared | 238 ns | 8.04 µs | 30.5 µs | 8.47 µs | 0.5 |
| 20 k/s | ring, consumers never sleep | 251 ns | 635 ns | 3.28 µs | 952 ns | 15.8 |
| 20 k/s | channel, its own parking | 251 ns | 2.86 µs | 14.4 µs | 6.35 µs | 0.2 |
| 200 k/s | ring, a token per sleeper | 178 ns | **1.53 µs** | 6.56 µs | 17.8 µs | 1.4 |
| 200 k/s | ring, one token, shared | 238 ns | 91.4 µs | 183 µs | 9.74 µs | 1.8 |
| 200 k/s | ring, consumers never sleep | 185 ns | 582 ns | 1.90 µs | 4.44 µs | 15.8 |
| 200 k/s | channel, its own parking | 264 ns | 26.2 µs | 64.3 µs | 77.9 µs | 0.8 |

At 200 k/s, 57 % of pushes woke someone. The runtime's contention profile of the two ring designs at
200 k/s, each run as a child:
- **one shared token:** 6.79 s of estimated waiting, 80 % of it inside sleepers' selects and 20 %
  inside wakes' sends;
- **a token per sleeper:** none to two decimal places.

The other run of the pair gave 94.8 µs against 1.80 µs at 200 k/s.

Spinning consumers buy the lowest tail for 15.8 CPUs. That is the bill the brief's "consumers should
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
| 64 | 0 | 1 |
| 256 | 0 | 7 |
| 1024 | 0 | 5 |
| 4096 | 0 | 3 |

That is 0 lost in 80,000 iterations in the real order, and 16 with the statements swapped (20 in the
other run). The race detector sees nothing in either. A reviewer reordering two lines for readability
would introduce it, with no atomic in sight to warn them.

One measurement problem turned up here:
- **The symptom:** an early version counted false losses. A select with several cases ready picks one
  at random. A consumer that reached its select late, more than 2 ms after arming the timer, could find
  its token and the expired timer both waiting and take the timer.
- **The fix:** `OpStats.sleep` now takes the token as well when the timer wins. The wake did arrive,
  and left in the channel it would end the next sleep early. In 15 repetitions of the phase since, the
  real order lost nothing.

**A blocking push with a deadline** is a select over the send, the shutdown channel and a timer the
goroutine reuses. `sync.Cond` cannot do that; `select` can.

## 4. Preemption you did not schedule (G4)

Since Go 1.14 the runtime can stop a goroutine at nearly any instruction. That is asynchronous
preemption, driven by `sysmon` once a goroutine has run for `forcePreemptNS`, 10 ms
(`runtime/proc.go:6390`). So a producer can be descheduled inside its window by the runtime as well
as by the OS. A preempted goroutine goes to the global run queue, and a P looks there only when its
own queue is empty, or on every 61st schedule.

**Every P spinning.** In this experiment:
- **The producer** reserves a slot and calls `runtime.Gosched` inside its window, which puts it on the
  global queue as a preemption would.
- **The consumers:** twelve wait for that slot on four Ps, so a P that looks for work finds a spinner
  queued before it finds the producer. The producer starts them itself, because a `go` statement
  queues the new goroutine on the creating P. A first version started them from elsewhere, and idle Ps
  stole them; the producer's P then found nothing queued and picked the producer straight back up.
  That version measured nothing.
- **The measure:** how long until the producer publishes, median and worst of 5. Spinners give up
  after 1 s.

| how consumers wait | async preemption on | worst | off | worst |
|---|---|---|---|---|
| a tight spin, no calls in the loop | 31.1 ms | 83.5 ms | **1000 ms** | 1000 ms |
| `Ring.Pop`, spin only | 62.1 ms | 62.7 ms | 17.3 ms | 56.9 ms |
| `Ring.Pop`, spin with `Gosched` | 9.33 µs | 16.6 µs | 11.2 µs | 22.9 µs |
| `Ring.Pop`, spin, then park (the ring's) | **6.53 µs** | 31.9 µs | 12.7 µs | 25.6 µs |

**What the table says about each way of waiting.**
- **A loop with no calls can only be stopped asynchronously.** With asynchronous preemption off it
  is never stopped: the producer ran again only when the spinners gave up. That is what
  `asyncpreemptoff` breaks, and it is also what a stop-the-world collection would wait on.
- **`Ring.Pop`'s spin calls functions.** Each call's prologue checks the preemption flag that
  `sysmon` sets, so it is stopped cooperatively whichever way the setting is. It still holds every P
  for tens of milliseconds, which is priority inversion with no priorities in the program.
- **Yielding on every spin hands the P back.** It costs the spinner a trip through the scheduler on
  every iteration.
- **Parking frees the P within microseconds,** which is why consumers spin 128 PAUSEs and then park.

**`runtime.LockOSThread` does not help.** It ties a goroutine to an OS thread. It gives that thread
neither a P nor a core, and it prevents neither kind of preemption. I did not measure it.

**GOMAXPROCS against the brief's 48 goroutines on 32 logical CPUs.** The ticket ring at 5 M/s,
median of 3:

| | push p50 | **p99** | p99.9 | behind p99 | consumers parked on a hole, per run |
|---|---|---|---|---|---|
| GOMAXPROCS 16 | 138 ns | 899 ns | 1.59 µs | 129 µs | 1,595 |
| GOMAXPROCS 32 | 191 ns | 1.06 µs | 2.01 µs | 115 µs | 1,052 |
| GOMAXPROCS 48 | 178 ns | 952 ns | 1.90 µs | 105 µs | 1,479 |

I expected 48 to bring back the oversubscription that C++ measured, 48 threads on 32 CPUs, and to be
the worst. It was no worse than 32, in both runs of the pair. My explanation, which this table does
not test, is that fewer than 32 of the 48 are runnable most of the time. Producers here push about a
hundred events and nap, and consumers park. The C++ harness's bursts of 5,800 events (§6) keep all 48
runnable for much longer. Step 6's arithmetic is unchanged:
65,536 slots is 13 ms at 5 M/s, against `sysmon`'s 10 ms slice. The "parked on a hole" column counts
consumers that went to sleep with a position below the tail reserved and unpublished: about a
thousand times a second, in every arm.

**A panic inside the window.** `TryPushFunc` lets a caller build the value in place, and a deferred
function pays the reservation with a tombstone if the build panics.
- **Why it cannot be forgotten:** a `defer` runs on a panic and on a normal return alike, so there is
  no path on which the debt is forgotten. The C++ `catch` has to be written for exactly the path
  nobody tests.
- **Measured:** a fill that panics leaves a tombstone, the panic reaches the caller, and the next
  event is delivered. With the open-coded defer, a push and pop cost 30 ns either way.
- **A fix from review:** the deferred tombstone now also wakes consumers. Events published behind it
  become reachable at that moment.

## 5. Close is a panic on the send side (G5)

A send on a closed channel panics, `panic("send on closed channel")`; phase 5 recovers it to show it.
With 32 senders, the data channel therefore cannot be the shutdown signal. A close racing a send
turns step 8's stranded slot into a crashed process. The protocol:
- **`Close` closes a separate `done` channel.** Producers that see it stop.
- **The driver counts producers out with a `sync.WaitGroup`.** Only once the last one has returned
  from its last push does it close the data channel (`ProducersDone`).
- **Receivers drain what the buffer holds, then see it closed.** That is the consumers' termination
  condition, and no event accepted before it is lost.

**A select whose cases are both ready picks one at random**, so `select { ch <- e; <-done }` can
accept an event after shutdown began. That is harmless if consumers drain after the producers are
counted out, and fatal if they stop at the signal. Measured: 32 × 16 at 1 M/s, closed 200 ms in, five
rounds per arm:

| | refused as closed | accepted after `Close` returned | drain after the close, worst |
|---|---|---|---|
| ring, CAS, block | 160 of 160 | 0 | 2.84 µs |
| ring, ticket, block | 160 of 160 | 0 | 2.32 µs |
| ring, reject | 160 of 160 | 0 | 5.40 µs |
| channel, non-blocking first, then select | 160 of 160 | 0 | 58.7 µs |
| channel, select over send and done | 160 of 160 | **101** | 715 µs |
| channel, reject | 160 of 160 | 0 | 109 µs |

Every producer was refused once, and every accepted event was consumed exactly once. Only the
textbook select let pushes in after the close. The non-blocking-first send checks an `atomic.Bool`
the close sets before it ever reaches a select.

**What a producer pays per push to learn whether shutdown has begun:**

| check, while it has not | alone | 32 at once |
|---|---|---|
| an `atomic.Bool` load | 1 ns | 6 ns |
| `select { case <-done: default: }` | 8 ns | 12 ns |
| `ctx.Err() != nil` | 4 ns | 7 ns |
| `select { case <-ctx.Done(): default: }` | 7 ns | 14 ns |

All four checks are cheap. `cancelCtx.Err` is an atomic load in this release; its comment says "~5x
faster than a mutex". I expected the blocking `select { ch <- e; <-done }` to be the expensive
spelling, because `selectgo` locks every channel in a select, so all 32 producers share the `done`
channel's lock on every push. Instead it was the fastest of the three channel sends in §1: 23.7 µs at
p99, against 461 µs for the non-blocking-first send. I have not measured why.

**The ring's termination, and the hang review found.** The ring keeps C++'s design: a CAS reservation
cannot happen after a close, because the closed bit is in the word it swaps. A ticket can, because an
`Add` cannot be refused. In C++ a ticket landing after the close paid its slot with a tombstone, and
the port did the same. That strands a producer:
1. More producers than there are slots pass the ticket pre-check.
2. The queue closes, and its consumers drain and leave.
3. All of them then take tickets.
4. The one whose ticket maps onto a slot an earlier one tombstoned waits for a release that no
   consumer will ever make.

It needs more producers in flight than slots, so at 65,536 slots it cannot happen; on a two-slot ring,
the test `TestTicketAfterCloseDoesNotWaitForASlot` reproduced it every time. The Go ring now has
`Close` record where the tail stood, from the old value its `Or` returns. Consumers drain exactly to
that position, and a ticket past it is refused without touching its slot, because nobody is owed it.
The tombstone remains for the one debt that is owed and cannot be paid with an event: a fill that
panicked. The C++ queue still has the strand. Its rings in this kata are all larger than its producer
count, so no run of it can hit it.

## 6. Measure the tail you promised (G6)

**The harness, not `testing.B`.** `RunParallel` runs GOMAXPROCS goroutines, all executing one body.
It can express "many goroutines doing the same thing", not 32 producers and 16 consumers doing
different things at a paced rate. So the driver ([`internal/bench/driver.go`](internal/bench/driver.go))
is the C++ driver ported:
- paced producers with staggered starts;
- the push time and the "behind schedule" time, the coordinated-omission view, recorded per push;
- a hashed exactly-once tally;
- a per-consumer order check.

**The instrument comes first.** Phase 0 prints it before anything depends on it.
- **`time.Now` is unusable here.** Its smallest observable step was **502 µs**, and 398,177 calls
  across 5 ms returned **4 distinct readings**. On Windows its monotonic reading is
  `KUSER_SHARED_DATA.InterruptTime` (`runtime/time_windows_amd64.s`), which advances at the clock
  interrupt.
- **So the driver reads the timestamp counter** through four lines of assembly. It runs at 2.419 GHz,
  is invariant, and costs 6.41 ns per read; a second 200 ms window agrees with its calibration to
  0.0021 %. Cross-core skew was +1 ns from CPU 0 to 2 and +21 ns from CPU 0 to 31.
- **The sleep floors matter for every later table.** `time.Sleep(1µs)` returned after 571 µs at the
  median, and a 50 µs timer in a select after 620 µs.

**The arms, at the brief's rate.** 32 × 16 at 5 M/s, block on full, 1 µs per event, median of 3:

| | achieved | push p50 | **p99** | p99.9 | behind p99 | e2e p99 |
|---|---|---|---|---|---|---|
| channel, non-blocking first | 1.76 M/s | 76 ns | 461 µs | 1.41 ms | 1.38 s | 45.1 ms |
| **ring, ticket** | **5.00 M/s** | 205 ns | **1.11 µs** | 2.12 µs | 102 µs | 1.03 ms |
| ring, CAS | 4.29 M/s | 1.48 µs | 71.1 µs | 339 µs | 555 ms | 11.3 ms |

The ring allocated nothing and the collector did not run, in this table or in any other paced one.
Each producer woke every 619 µs and pushed 97 events back to back.

**The decision, as written before the run.** The ring is kept only if its worst p99 of the three
repetitions beats the channel's best. It did: 1.16 µs against 461 µs. **Against the brief, the ticket
ring's p99 at 32 × 16 was 1.11 µs, and 899 ns in the other run of the pair: met.** The CAS reservation
missed in both (71.1 µs and 17.8 µs). Saturated, it retried 3.99 times per push; the C++ track counted
3.4–3.7.

**That verdict holds at this machine's normal clock.** During one earlier full run the laptop's power
management held CPU 0 at about a third of its usual clock; a PAUSE read 180 ns where it reads 36–52 ns
here. In that stretch phase 4's GOMAXPROCS 32 row, which runs this spec, measured 9.31 µs. That build
differed from this one only off the hot path. Every phase prints the PAUSE thermometer for this reason.

**The C++ track's other two rows,** same spec:

| | channel p99 | ticket ring: achieved | push p50 | **p99** | p99.9 |
|---|---|---|---|---|---|
| 16 × 8 at 5 M/s | 210 µs (at 1.53 M/s) | 5.00 M/s | 132 ns | 555 ns | 899 ns |
| 32 × 16 at 1 M/s | 122 µs | 1.00 M/s | 72 ns | 820 ns | 2.33 µs |

**The pacer is part of the experiment.** Both harnesses ask each producer to nap 200 µs between
bursts and then push everything that came due.
- **Go:** the runtime sleeps on a high-resolution waitable timer, so each producer woke every 619 µs
  and pushed 97 events.
- **C++:** in the C++ track's own run, each producer woke every 37.2 ms and pushed 5,814 events back
  to back (cpp §10's run), because `sleep_for` on Windows returns tens of milliseconds late.

So the two tracks' tables do not offer the queue the same traffic, and the same arms were rerun here
with a 37 ms nap:

| 37 ms between bursts | achieved | push p50 | **p99** | p99.9 | behind p99 |
|---|---|---|---|---|---|
| channel, non-blocking first | 1.84 M/s | 69 ns | 461 µs | 1.35 ms | 1.33 s |
| ring, ticket | 4.83 M/s | 119 ns | **2.01 µs** | 3.17 µs | 9.10 ms |
| ring, CAS | 4.28 M/s | 1.64 µs | 57.6 µs | 312 µs | 499 ms |

Each producer now woke every 36.6 ms and pushed 5,714 events, the C++ harness's traffic. The ticket
ring's p99 roughly doubled, to 2.01 µs (2.43 µs in the other run), and crossed the line. The C++
ticket's 2.96 µs and 3.60 µs came from other runs and are not compared as a ratio. What this
measurement shows is that most of the distance between "C++ misses at 32 × 16" and "Go meets it" is
how the two harnesses' producers sleep. The brief's 5 M/s says nothing about burst shape, and the
tail depends on it.

**The saturated ceiling,** 32 × 16, 250,000 events per producer, nothing done per event:

| | throughput | retries per push |
|---|---|---|
| channel, bare send | 7.27 M/s | — |
| channel, non-blocking first | 6.45 M/s | — |
| ring, ticket, a slot per line | 8.54 M/s | 0 |
| ring, ticket, slots packed | 3.82 M/s | 0 |
| ring, CAS | 5.08 M/s | 3.99 |

**Asynchronous preemption, on and off.** It is read when the runtime starts, so each run is a child
process, the pairs launched back to back:

| | p99 on | p99 off |
|---|---|---|
| ring, ticket | 1.16 µs | 1.11 µs |
| channel | 461 µs | 461 µs |

Asynchronous preemption made no measurable difference to either queue at the brief's rate. What it
does matter for is §4's spinners.

**The race detector changes timing and scheduling.** So a race build reports no latency at all, and
divides the correctness phases' event counts by ten. It runs phases 2, 3 and 5 (§7).

### Measurement problems, and what they cost

- **`time.Now`.** Its smallest step is 500 µs, so every latency here would have read zero or a whole
  number of milliseconds. Fixed with the counter, printed in phase 0.
- **One heap allocation per event, from escape analysis.**
  - **The cause:** the driver's consumer declared `var ev Event` inside its loop and passed `&ev`
    through an interface method. The compiler cannot keep such a variable on the stack.
  - **The effect:** 1.000 allocations per event, and 12 to 37 collections per one-second run, until
    the declaration moved out of the loop. The allocations-per-event column exists because of this.
- **The thermometer read whichever core it landed on:** 26 to 55 ns from one phase to the next. It now
  pins itself to CPU 0.
- **The first hole experiment measured nothing** (§4).
- **The runtime's contention totals are estimates** that can exceed goroutines × seconds (§1).
- **A select picks among ready cases at random,** which first made a late wake look like a lost one
  (§3).
- **The arrival process differs between the two harnesses** by a factor of 60 in burst size (above).
- **The clock moves under sustained load** (above). The final pair's PAUSE readings ran from 35 to
  97 ns. One earlier development run with the clock pulled down measured the ticket ring's p99 at
  3.17 µs.

## 7. Verification

**Tests, under `go test` and `go test -race`** ([`internal/turnstile/`](internal/turnstile/)):
- lap wraps on two- and four-slot rings, for every reservation and policy;
- never overwriting with no consumer;
- close against reserve for every arm;
- a ticket taken after the close not waiting for a slot;
- a panic inside the window paying its slot;
- the collector probe;
- a send on a closed channel panicking;
- the shutdown protocol with 32 senders.

All pass, and pass under the race detector.

**The bug × tool table.** Each deliberate bug is one line of the protocol made wrong:
- **The stress:** 8 × 8 on a 64-slot ring, 25,000 events each, with a 30 s hang limit.
- **The race detector:** the same stress in the race build.
- **The engineered race:** §3's race, 4 × 20,000 iterations.

| bug | stress, x86-64 | race detector | engineered race |
|---|---|---|---|
| none | clean | 0 reports | 0 lost |
| publish before writing the payload | **658 events torn** | **3 reports** | — |
| reserve a slot still holding the previous lap | **hung** | **3 reports**, then hung | — |
| re-check, then announce | clean | 0 reports | **20 lost** |

The race detector is the instrument for a payload published too early, and it reported the overwrite
as well. For the swapped sleep it is blind, and so
is the stress: its consumers never sleep long enough for a lost wakeup to be more than a delay. Only
the engineered race sees that bug. The race-detector build ran phases 2, 3 and 5 with 0 reports.

**Independent review.** A reviewer was given the code, not my conclusions. It found no lost wakeup
and no exactly-once violation. It did find these:
- **The ticket-close strand** (§5), now reproduced by a test and fixed.
- **The panic tombstone did not wake consumers** (§4). Fixed.
- **A wake claimed for a sleeper whose re-check then found work was dropped.** For a producer blocked
  on full, woken only once per 64 releases, that could mean sleeping through free slots until its
  50 ms backstop. `Cancel` now passes such a wake on.
- **Two API traps.** An `OpStats` used on two Waiters re-assigned its sleeper slot; it now panics
  instead. And a 128-producer limit in the order check failed runs spuriously; the driver now refuses
  such a run up front.
- **A harness gap.** The driver never asserted that every producer finished, so a producer stuck in a
  run without an early close went unnoticed. It now does. The reviewer also noted that the accounting
  check `offered == accepted + rejected + refused` is the harness checking itself and cannot catch a
  queue bug; it is kept, and labelled as such in the code.

## 8. Predictions against results

Written before the first measuring phase ran:

| prediction | result |
|---|---|
| the channel misses the 2 µs p99 at 32 × 16 by 10–50 µs | missed by much more: 461 µs, and it could not sustain the rate at all |
| saturated, the channel is below 5 M/s at 32 × 16 | wrong: 5.57–7.56 M/s; saturated is not where it fails |
| the textbook `select` is the slowest send | wrong: lowest p99 of the three (23.7 µs), and the only one that accepts after close |
| the ticket ring's p99 at 32 × 16 is 1.5–3 µs | 1.11 µs and 899 ns; 2.01–2.43 µs under the C++ harness's bursts |
| the CAS ring misses at 10–30 µs | 17.8–71.1 µs |
| the wake decision with nobody asleep costs ~2 ns | 3 ns, 7 ns with 32 at once |
| a wake costs the waker 1–5 µs | 1.6–2.0 µs at the median |
| at 20 k/s a sleeper count and a token on every push perform alike | true, and beside the point: the shared token, not the count, was the problem |
| a spinning hole lasts 10–20 ms with async preemption, forever without | 31.1 ms with, until the 1 s give-up without |
| `Ring.Pop` spinning, async preemption off: 10–20 ms | 17.3 ms |
| GOMAXPROCS 48 brings back oversubscription and is worst | wrong: 48 and 32 were equal in both runs |
| padding is worth 20–50 % | worth more: 2.2× |
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
  - **What happened:** once, before the last changes to the sleep, phase 3's low-rate section hung
    until the watchdog ended the run (exit 3). No goroutine stacks were captured.
  - **Since then:** phase 3 has run 32 more times without one — 20 on its own, 8 after phases 0–2,
    and 4 within full runs — 27 of them after the change to the sleep. The watchdog now writes every
    goroutine's stack before exiting, so a recurrence will say where.
  - **Status:** I could not construct the interleaving from the code, and do not claim the later
    changes fixed it.
- **No model checker.** Go has no Relacy or GenMC equivalent in common use, so the lap wrap is covered
  by stress on two- and four-slot rings and by the race detector, not exhaustively.
- **AArch64 not run.** There, Go's sequentially consistent atomics are `LDAR`/`STLR` pairs.
- **A per-producer channel arm** — the C++ track's §13 experiment — is not built in Go.
- **`MaxSleepers` is 64 per side of a queue.** An `OpStats` belongs to one goroutine, sleeping on one
  Waiter.
- **The C++ queue keeps the ticket-close strand** (§5).

---

## Appendix: Glossary

**Async preemption** — the runtime stopping a goroutine at an arbitrary instruction, by signal (or
`SuspendThread` on Windows), once it has run 10 ms. `GODEBUG=asyncpreemptoff=1` turns it off.

**Coordinated omission** — measuring only how long the operations that ran took, rather than how late
each event became. The "behind schedule" column is the correction.

**EcoQoS** — Windows' power throttling of background processes: lower clocks, efficiency cores. The
binary opts out at start.

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
