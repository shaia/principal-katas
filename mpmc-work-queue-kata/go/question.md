# The Go track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

Go is the track that starts with the answer already written. `make(chan Event, 65536)` is a bounded
MPMC queue with blocking sends, non-blocking sends that fail, parked receivers, and a close — so the
first job is to read it and find out which of the main kata's steps it has already answered, and
whether it meets the one requirement it may not. After that the track ports the slot protocol by
hand, and finds two things in this kata with no direct Go spelling: a sleep that compares a value
before blocking, and a preemption you never asked for.

[Kata 1's Go track](../../mpsc-event-pipeline-kata/go/question.md) already established that
`sync/atomic` gives you no orderings to choose, and what that costs on the publish path. This one does
not repeat that; it asks what the absence of choice does to a protocol with *two* handoffs per slot
and sixteen sleepers.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step G1 — The runtime already wrote this queue

Map steps 0–10 onto a buffered channel of capacity 65,536 before you write a single line of your own.

**Be prepared to say:**

- what `runtime.hchan` holds and what protects it — and therefore how a channel answers the central
  question, in one sentence about *where* reservation and publication happen
- what a send does when a receiver is already parked, and whether that event ever occupies a buffer
  slot at all. What does that do to your definition of "accepted"?
- block and reject: a plain send, and `select` with a `default` — and what the reject path costs next
  to a successful send
- the steps the channel answers by construction — list them — and the ones it cannot. Name the
  requirement in the brief that a channel cannot be configured to meet, if there is one, and say how
  you would find out
- the measurement: ns per operation at 1, 8 and 32 senders against 1, 4 and 16 receivers, and the p99
  of a send at 32 × 16 — and which of those numbers decides whether you go any further

<details>
<summary>Hint</summary>

A channel is a ring under one runtime mutex, with queues of parked goroutines on both sides. Under
that lock, reservation, copy and publication are one critical section — which makes it the main
kata's step-10 mutex, written by the runtime team. So the question is not whether it is correct. It
is whether one lock, contended 48 ways, meets a 2 µs p99, and you can only answer that by measuring
the tail rather than the mean.

Read `chansend` closely. A sender that finds a parked receiver copies straight into that receiver,
bypassing the buffer, which is faster — and means "in the queue" was never a state that event was in.
A non-blocking send to a full channel has a fast path of its own; find out what it does and does not
touch.

</details>

**Done when** you can say where the channel's reservation and publication happen, list the steps it
answers, and have the measured p99 at 32 senders × 16 receivers.

---

## Step G2 — Port the slot protocol: keep the states, drop the annotations

Write the per-slot sequenced ring from step 3 in Go, with no `unsafe`.

**Be prepared to say:**

- what survives the port: the lap-stamped sequence, the four tests, and the signed difference. Write
  the difference for `uint64` operands in Go, and say why Go's defined wraparound makes it simpler
  than the C++ version
- what disappears: every ordering choice, and with it the step-5 fence. Say why the lost-wakeup
  interleaving cannot occur when every atomic operation is sequentially consistent — and what you pay
  per push instead
- what may be a plain field: the payload. Quote the sentence of the memory model that makes a plain
  write before an atomic store visible to a plain read after an atomic load that observes it
- generics: `Ring[T any]` — what the collector must scan when `T` contains pointers, and what a
  consumer must write into a slot after taking its event so the ring does not keep garbage alive
- the race detector: what it reports if you publish before writing the payload, and why it can say
  nothing at all about memory ordering

<details>
<summary>Hint</summary>

The memory model's synchronized-before edge for atomics is exactly a release/acquire pair, except
that it is not optional: plain payload writes made before the sequence store are visible after a
sequence load that observes it. And since all atomic operations also fall into one total order, the
step-5 race needs an outcome — two loads that each miss the other side's store — that a single total
order of four operations cannot produce. That gift is paid for on every publish.

The collector question is the C++ track's moved-from residue in another form. A consumed slot that
still holds a pointer keeps its referent alive until the slot is overwritten — one full lap later,
which at 5 M/s is 13 ms, and under a light load could be never.

</details>

**Done when** the port passes `go vet` and a clean `-race` run, and you can say what a consumed slot
must look like for the collector to free what it pointed to.

---

## Step G3 — Build the sleep without a futex

Go exposes no wait-on-address. Your consumers still have to sleep without losing a wakeup, and your
producers still cannot afford a system call per push.

**Be prepared to say:**

- what Go *does* expose that can put a goroutine to sleep and wake it — and, for each, whether it can
  lose a wakeup
- a buffered `chan struct{}` of capacity one as a coalescing wake token: the producer's non-blocking
  send, the consumer's receive. Why the token cannot be lost, and how many consumers one token wakes
- sixteen consumers: one token channel, sixteen, or something else — and who wakes the second
  consumer during a burst
- `sync.Cond`: what it needs, and whether a lock on the *sleep* path (not the push path) is acceptable
- the cost of each on the push path when nobody is asleep, measured. It must not be the expensive case
- a blocking push with a deadline: which of these composes with `select` and a timer, and which does not

<details>
<summary>Hint</summary>

A channel with room for one token is a semaphore that saturates at one. A send that would block is
the "already signalled" case, so a non-blocking send coalesces naturally — and the token stays put
until a consumer takes it. That persistence is what prevents the lost wakeup, not any fence.

The question is what it costs when nobody is waiting. Read `chansend`'s fast path for a non-blocking
send to find out when it takes the channel's lock and when it does not. Then decide what the token's
resting state should be while consumers are busy — present or absent — because one of those makes
every push take a lock.

`sync.Cond` needs a `Locker` and has no timeout. `select` has both a timer case and a default.

</details>

**Done when** you can name your primitive, its measured no-sleeper push cost, its wake fan-out, and
how a blocking push times out.

---

## Step G4 — Preemption you did not schedule

In C++, the OS deschedules a thread. In Go, the runtime can also deschedule a goroutine — in the
middle of a function that never calls into it.

**Be prepared to say:**

- asynchronous preemption: since which release a goroutine can be stopped at nearly any instruction,
  and what that means for the window between reservation and publication
- consumers spinning on a hole occupy Ps. With every P running a spinning consumer, where is the
  producer that owns the hole, what eventually lets it run, and after how long?
- `runtime.Gosched` in a spin loop, and `runtime.LockOSThread`: what each does and does not do here
- `GOMAXPROCS` against 48 goroutines on your core count, and what oversubscription does to the main
  kata's step-6 arithmetic
- a panic between reserve and publish: what `defer` gives you, and what your deferred function must
  write into the slot
- `GODEBUG=asyncpreemptoff=1` as one arm of an A/B: what it isolates, and what it breaks

<details>
<summary>Hint</summary>

Spinning consumers are the trap. If every P is running a consumer that spins waiting for the hole to
fill, the goroutine that would fill it is runnable but has nowhere to run until the runtime forcibly
preempts a spinner — find the interval at which it does that, in `runtime/proc.go`. That is priority
inversion without any priorities in the program.

A `defer` that publishes a tombstone is Go's version of "every path pays the debt" — and unlike the
C++ `catch`, it runs on a panic and on a normal return alike, so the debt cannot be forgotten on the
path nobody tested.

</details>

**Done when** you can say how long a hole lasts with spinning consumers on a fully occupied set of Ps,
and what your deferred function writes.

---

## Step G5 — Close is a panic on the send side

Step 8's close-versus-reserve race, in a language where getting it wrong does not strand a slot — it
crashes the process.

**Be prepared to say:**

- what `close` does to senders and receivers, and why a send on a closed channel panicking turns step
  8's race into a crash rather than a lost event
- who closes: the rule, and the protocol that lets 32 producers stop without anyone closing a channel
  that someone else still sends on
- `select` over a `done` channel and a send: what happens when both are ready, whether an event can be
  accepted *after* shutdown began — and whether that is a bug
- the consumers' termination condition for your hand-rolled ring, with no `close` to lean on
- `context.Context` as the shutdown signal: what checking it costs per push

<details>
<summary>Hint</summary>

The Go idiom — only a sender closes, and with many senders nobody closes the data channel — exists
because `close` is the one channel operation that is not idempotent, and it turns a race into a
panic. With 32 senders, coordinate shutdown on a separate signal and count producers out with a
`sync.WaitGroup`; the data channel is closed, if at all, by whoever knows the last sender has
finished.

`select` chooses uniformly at random among ready cases, so "shutdown requested" and "event accepted"
can both be true of the same push. That is fine as long as consumers drain after the producers have
been counted out, and fatal if they stop at the signal.

</details>

**Done when** you can shut down 32 producers and 16 consumers with no panic and no stranded event, and
say which goroutine closes what.

---

## Step G6 — Measure the tail you promised

The brief makes a promise about p99 enqueue latency. Decide how you would know whether you kept it.

**Be prepared to say:**

- `testing.B` with `RunParallel` versus a harness with fixed goroutine counts: which of them measures
  the brief's 32 × 16
- recording p99 enqueue: the clock (Kata 1's Go track has a warning about it on Windows), the
  histogram, and where the time a blocked send spends goes
- what `-race` does to timing and scheduling, and therefore which runs may report a latency at all
- the arms: channel against ring; async preemption on and off; `GOMAXPROCS` at the core count and
  below 48 — and the result that would make you keep the channel
- the invariants under shared labels — exactly-once, no read before publish,
  `accepted + rejected == offered` — and the exit code that reports them

<details>
<summary>Hint</summary>

Decide the falsifying result first. The channel is the default. The ring has to beat it on the p99 the
brief names, at 32 × 16, on your hardware, by more than run-to-run noise, before its extra protocols
are worth carrying.

If it does not, the finding is that Go's runtime already solved this queue well enough — which is a
result, not a failure, and arguably the most useful one this track can produce.

</details>

**Done when** you have written down the arms, what is held fixed, and the p99 margin that would make
you delete the ring — before running anything.

---

<details>
<summary><b>The shape of the Go answer</b> — read after you have your own.</summary>

Start with the channel. It answers the central question by putting reservation, copy and publication
inside one critical section on `hchan.lock`, so the distinction is never observable; it cannot lose a
wakeup, because parking happens under the same lock; and select-with-default is the reject policy. If
the channel misses anything in the brief, it will be the 2 µs p99 under 48-way contention on that one
lock, and the only way to know is to measure the tail at 32 × 16.

If the measurement forces a ring, the slot protocol ports intact and gets simpler. Every atomic is
sequentially consistent, so there are no orderings to choose and no fence to forget, and the lost
wakeup cannot be written — but every publish pays for that. Payloads are plain fields; consumed slots
are zeroed so the collector can free their referents.

Sleep is a one-token channel behind a sleeper count, so a busy system never touches it.
Preemption arrives from the runtime as well as the OS, and spinning consumers can occupy the very Ps
the hole's owner needs, until the runtime forces a preemption after 10 ms — so consumers spin
briefly, then park, and a `defer` pays every reservation. Shutdown is a separate signal plus a
`WaitGroup`, never a `close` on a channel that has 32 senders.

</details>

Then read what the same questions look like in the other two:
[cpp/question.md](../cpp/question.md) · [python/question.md](../python/question.md).
