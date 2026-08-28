# mpsc-event-pipeline-kata — high-throughput in-process event pipeline

One systems-design question, answered three times in three languages — each from that language's own
cost model rather than translated from the others.

[**question.md**](question.md) — many producers push small events to a single consumer that batches
them into a socket. Millions of events/sec, low latency, minimal allocation, bounded memory,
producers must never block indefinitely, and **event order is preserved per producer, not globally**.
That last clause is what permits an architecture with no shared coordination point at all. The
follow-up: with 64 producers but only 4–8 active, stop polling 64 empty queues without reintroducing
a contended global synchronization point.

It is staged as a kata — eleven steps that build the argument in the order it has to be built, each
with probes, a hint, and a self-assessable gate, and the answer sketch folded at the bottom. Three
language tracks then ask what the shared question cannot:
[cpp/question.md](cpp/question.md) · [go/question.md](go/question.md) ·
[python/question.md](python/question.md).

## The three answers

| | thesis |
|---|---|
| [**cpp/solution.md**](cpp/solution.md) | One bounded SPSC ring per producer. No lock, no CAS, no `fetch_add` on the push path — the whole design exists to keep producers off a shared cache line. Reaches **362 M/s** sustained and *rises* with producer count. |
| [**go/solution.md**](go/solution.md) | **Start with one buffered channel per producer** — ordinary Go, ~100 lines, no `unsafe`. Escalate to a hand-rolled ring only when the *consumer* saturates, because Go has no bulk channel receive. |
| [**python/solution.md**](python/solution.md) | Rederived from Python's cost model: how much interpreted bytecode runs per event. **If your producers are coroutines, use [`weir.aio`](python/src/weir/aio.py) and most of the problem disappears.** Ceiling is 1.2–2.7 M/s for the whole interpreter. |

All three assert the same invariants under the same labels — per-producer ordering preserved,
`accepted + dropped == offered` exactly, a drain shutdown loses nothing, buffer depth never exceeds
capacity — and each exits non-zero if one breaks.

They also share a wire format. `Event` is 32 bytes in every language (`producer_id: u32`,
`seq: u32`, `stamp_ns: i64`, `payload: 16 B`, offsets `0/4/8/16`, little-endian), so a batch produced
by any of the three decodes in the other two.

## Why "weir"

A **weir** is a low barrier built across a river. Water flows over it continuously; the barrier holds
a controlled depth upstream, and whatever arrives faster than the crest can pass simply spills over
and is gone. Engineers build them to *measure* flow, because the depth over the crest tells you the
rate — and to make overflow predictable instead of catastrophic.

That is this design, precisely, and in four respects:

| the structure | the queue |
|---|---|
| holds a **bounded** pool upstream | each producer's ring is a fixed capacity; memory is `N × capacity`, not a hope |
| **spills** the excess rather than backing up | `DropNewest` — overload is an explicit policy that terminates, never an unbounded queue that defers the problem into an OOM kill |
| passes a **steady** flow downstream regardless of surges | the consumer drains in fixed batches into one sink call; the rings absorb the burst |
| exists to **gauge** the flow | every mechanism is counted — `pushed`, `dropped`, `high_water`, `flag_writes`, `parks` — because a design whose failure mode is silent loss must never be silent |

The last one is why the name earns its place rather than just sounding nice. The C++ answer's worst
defect — eager bitmap clearing doing 582,954 writes to a shared line where the fix does 8 — broke no
test and was found only because the counter existed. A weir that you cannot read the depth on is just
a wall.

It also names the thing the *specification* is actually about. "Producers must not block
indefinitely" plus "bounded memory usage" are one requirement seen from two sides, and a weir is the
structure that satisfies both at once: nothing upstream stalls, nothing grows without limit, and the
difference goes over the top where you can count it.

Short, unambiguous, and it reads as a qualifier in all three languages — `weir::EventPipeline`,
`weir.Event{}`, `from weir import EventPipeline`.

## Verify

```sh
# C++ — exit code is the verdict
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build && cpp/bin/solution

# Go — invariants in `go test`, measurements in the binary
cd go
go vet ./...                      # includes copylocks; `go test` does NOT run it
go test ./...
go build -o bin/weir.exe ./cmd/weir && ./bin/weir.exe

# Python  (PowerShell; on bash use `source .venv/Scripts/activate`)
cd python
uv venv --python 3.13
.\.venv\Scripts\Activate.ps1
uv pip install -e ".[dev]"
pytest -q                         # 26 invariant tests, ~10 s
python -m weir_bench              # measurement phases
```

<details>
<summary>Race and thread checking (needs two one-time installs)</summary>

```sh
# Go race detector — needs cgo, which needs a GCC-compatible compiler.
# clang targeting MSVC will not work: cgo passes -mthreads, which it rejects.
winget install BrechtSanders.WinLibs.POSIX.UCRT
cd go && CGO_ENABLED=1 CC=gcc go test -race ./...

# See what -race actually catches, as a control on the clean run above:
WEIR_SHOW_RACE=1 CGO_ENABLED=1 CC=gcc go test -race -run ContractViolation ./internal/weir/

# C++ ThreadSanitizer — unavailable on x86_64-pc-windows-msvc, available in WSL.
wsl -- bash -lc 'cd /mnt/c/.../mpsc-event-pipeline-kata/cpp &&
  cmake -S . -B build-tsan -DWEIR_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo &&
  cmake --build build-tsan && ./bin/solution_tsan'

# Longer runs
cd go     && ./bin/weir.exe                    # full sizes, a few minutes
cd python && pytest -q -m slow                  # 16 x 200k parity stress
```

</details>

## What each language changed

Not a demonstration that the design ports — it partly doesn't. Two of the three reach a different
conclusion than C++ does.

- **C++** gets to choose its memory ordering, and spends its longest section on why acquire/release
  is correct and `seq_cst` is not needed — except in one place, off the hot path, where StoreLoad is
  unavoidable and a misplaced fence was a real shipped bug.
- **Go** removes that choice: every `sync/atomic` operation is sequentially consistent, so the
  publish costs a locked `XCHGQ` where C++ pays a plain `MOV` — but the fence guarding the wakeup
  handshake becomes *unwritable*, and so does the bug it guards. It also adds a GC that must never
  scan the rings, removes RAII, and makes the rejected design idiomatic. The real finding is
  elsewhere, though: **sharding channels fixes contention completely** (31.5 ns/push flat against a
  shared channel's 1313) and then plateaus at ~10 M/s sustained, because Go offers no bulk channel
  receive and the consumer pays an `hchan.lock` per event.
- **Python** removes the premise. Under the GIL there are no parallel producers, so the contention
  the C++ design exists to eliminate was never there. What decides the structure instead is that an
  **uncontended `threading.Lock` costs 96 ns** — the entire push budget — so single-writer index
  ownership, not cache coherence, is what buys the lock-free path. And the follow-up **inverts**: the
  optimization C++ measures as not moving the tail is worth **3.8× on p50** here (278 → 73 µs),
  because a probe is interpreted bytecode and never an L1 hit.

### The cross-cutting result

Go and Python arrive at the same wall from opposite directions. Go's per-producer channel is fast to
send into and slow to drain; Python's per-producer `deque` is fast to append to and slow to drain.
In both, the idiomatic container's drain costs 30–100 ns of per-event work on the **one participant
that cannot be sharded**, and that sets the ceiling for everything.

> The rule both answers land on: **shard the producers, then measure the consumer.** If it keeps up,
> you are done. If it does not, no arrangement of the idiomatic container fixes it — the missing
> primitive is a bulk drain, and the hand-rolled ring exists to provide one.

## Notable results

- **`alignas` on the first member of a group aligns that member, not the group.** In the C++ pipeline
  that put `running_`, `accepting_` (acquire-load on *every push*), `parked_`, `retire_pending_`
  (an `exchange` — an RMW — on *every idle pass*) and `park_mu_` on one cache line, line 131. The
  consumer's idle loop takes the push-path line exclusive on every empty pass: a coherence miss per
  push, produced by the consumer doing nothing. No test can see it — the program is correct, just
  slower than it claims — and inspection cannot either. `clang -Xclang -fdump-record-layouts` can.
  ([cpp §3](cpp/solution.md#3-cache-behavior))
- **Go's `time.Now()` on Windows does not read QPC.** It reads `KUSER_SHARED_DATA.InterruptTime` and
  advances with the clock interrupt: 237,201 calls across 5 ms produced **six** distinct values, a
  307 µs smallest observable tick against a 4.4 ns read cost. Any microsecond-scale latency measured
  with it reports `p50 = 0.0 µs` — here, a Go port that looks *faster* than the C++. The measurement
  needs its own instrument: a calibrated RDTSC clock, 0.413 ns resolution, within 0.03 % over 200 ms,
  printed in phase 0 before any result depends on it. ([go §8a](go/solution.md#8a-measuring-at-all))
- **The SPSC single-producer contract is a comment in C++ and a checked property in Go.** Two
  goroutines pushing on one handle is reported: `WARNING: DATA RACE … spsc_ring.go:78`. That control
  is what makes the clean `-race` run on the real design non-vacuous. On the C++ side the equivalent
  is TSan, unavailable on `x86_64-pc-windows-msvc` and run under WSL: clean across 3.1 M events and 40
  waves of producer churn. ([go §10](go/solution.md#10-verification))

## Layout

``` text
mpsc-event-pipeline-kata/
  question.md          the kata — 11 steps, the rubric, the folded answer sketch
  cpp/                 question.md solution.md  src/ bench/ CMakeLists.txt  -> bin/solution
  go/                  question.md solution.md  internal/{weir,verify,phases} cmd/weir  -> bin/weir
  python/              question.md solution.md  src/{weir,weir_bench} tests/  -> python -m weir_bench
```
