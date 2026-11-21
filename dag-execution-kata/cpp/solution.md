# The C++ answer — counted is not published

## 1. The missing edge

The counter's modification order makes precisely one predecessor observe `1`. That proves uniqueness of scheduling, not publication of every input. A release/acquire queue publishes the final predecessor's preceding effects; it does not independently acquire other predecessors' ordinary writes. Without that edge, ordinary result reads can race with those writes, giving undefined behavior.

The implementation uses `fetch_sub(acq_rel)`. Each RMW reads the immediately preceding modification, acquiring its release and transitively its earlier predecessor writes. The final decrement therefore aggregates all results. Its queue publication transfers them to the executing worker; an inline continuation is sequenced after the acquire instead. See C++20 [atomics.order](https://timsong-cpp.github.io/cppwp/n4861/atomics.order). A release decrement plus a final acquire fence can also work through release sequences, but is not the default here; see [atomics.fences](https://timsong-cpp.github.io/cppwp/n4861/atomics.fences).

Graph initialization precedes submission publication. Each predecessor writes its own output and terminal state before decrementing children, never modifies that output afterwards, and notifies each edge once. Output/state fields are ordinary objects: the scheduler supplies their synchronization. Duplicate edges, excessive fan-in, invalid IDs, and cycles are rejected before execution. `Builder::freeze` returns an immutable graph with contiguous successor storage and fixed eight-element predecessor arrays.

## 2. Ownership and terminal accounting

`Executor::submit` accepts an entire graph run or returns an empty handle for capacity/closed rejection. Graph descriptions are shared, while tasks, pending counters, outcomes, and outputs belong to a run. The graph is reusable across concurrent runs. Node callbacks are function pointers with borrowed `void*` contexts; the caller must keep context storage alive and synchronize any shared mutable context. The scheduler owns the uint64 output of each node. It is intentionally a teaching executor, not a generic dynamically typed result framework.

A run remains owned by the executor after its caller drops the handle. A worker holds a temporary shared run reference while executing and publishing children. The final terminal-accounting RMW acquires the earlier accounting operations, removes the executor's owning reference, releases admission capacity, and publishes `done`. `wait` acquires that publication before inspecting outcomes. The graph arena outlives every successful queue reservation; losing speculative thieves never dereference candidates. Deque arrays outlive all workers and indices never wrap. Callback contexts have their separate caller-owned lifetime.

Exceptions become `failed`; a node with any unsuccessful predecessor becomes `skipped` without calling its function. Otherwise a cancellation observed at dispatch makes it `canceled`. All outcomes notify children. Running callbacks get a stop token; cancellation cannot forcibly interrupt them, and they may successfully return after cancellation. Failed independent branches do not cancel unrelated work. Shutdown closes admission, drains accepted runs, and joins all workers. Calling wait from this executor's callback throws; destroying/shutting down the executor inside its own callback is a contract violation. Callbacks must eventually return.

Capacity counts admitted unfinished node instances, with a default limit of one million. Run allocation/initialization happens on the submission path, so rejected submissions can still incur temporary allocation. A retained completed handle still owns its results: the active-task cap is not a cap on memory deliberately retained by callers. Per-task shared-reference operations remain an overhead worth measuring; this implementation does not hide them behind a zero-cost claim.

## 3. Queue proof and scheduling policy

The global mutex FIFO is a baseline, not the proposed scalable architecture. Sharded mutex deques pop locally from the back and steal from the front. The Chase–Lev arm uses fixed 4,096-entry rings; external submissions and full-ring overflow use intrusive per-worker locked inboxes. No callback completion allocates a queue entry. Admission and run retirement use a control mutex, separate from ready queues. The whole executor is not lock-free.

Algorithm source: Nhat Minh Le et al., **Correct and Efficient Work-Stealing for Weak Memory Models**, PPoPP 2013, [Figure 1 and §5](https://fzn.fr/readings/ppopp13.pdf). The paper, not a third-party reference repository, is the source; there is no reference-code revision dependency.

| Source | Adaptation | Check |
|---|---|---|
| Fig. 1 push/take | One owner, release-store publication, atomic pointer slots | LIFO, empty take, repeated slot reuse |
| Fig. 1 steal | Acquire observations, SC fences/CAS, dereference after success | Thief/owner exactly-once race |
| Fig. 1 resize | Fixed ring; locked intrusive overflow | 15,000-way ready fan-out |
| §5 SC version | Separate all-SC reference deque | Same oracle and concurrency checks |
| Mathematical indices | Signed 64-bit, initially 1, checked upper bound; future pushes spill | Empty and near-limit tests |

The release store replaces the paper's release fence followed by a relaxed store. The array never grows, removing array reclamation. These are intentional changes, not claims that the paper proves this entire executor. SC fences in take/steal remain necessary for arbitration; publication of task outputs is a separate proof from uniqueness of reservation. Tests and TSan do not replace either proof.

Work-first execution retains one newly ready successor (highest remaining-path rank), iteratively, with a 32-task maximum chain. Other successors are queued. Priority mode marks nodes lying on a weighted longest path, with eight urgent dispatches before ordinary service when available. These are heuristics, not a p99 guarantee or deadline scheduler. Cost hints default to one and need not match observed task times.

Idle workers scan victims from a worker-local randomized start, then register idle and recheck before parking. Publishers increment a target worker's epoch under its parking mutex and notify it. Registration/observation uses SC atomics; the epoch prevents a notify between recheck and wait from being lost. A 1 ms timed wait is the liveness backstop for racing scans, stale idle observations, or selecting a different sleeper. This bounds search attempts per scan, not their elapsed execution time on a descheduled worker. Publication currently pays a parking-mutex acquisition even when no worker is idle, deliberately leaving a visible optimization opportunity.

On multi-node Windows hosts, workers attempt group-zero affinity and prefer same-node victims before remote victims. The CSV records whether all affinity calls succeeded. Single-node hosts and unknown/Linux topology use randomized victims without pinning. This is not a NUMA performance result; processor groups beyond zero and a Linux NUMA backend remain unsupported.

## 4. What to measure

Any execution obeys `T >= max(W/P,L)`. The source example has 10 ms total useful work and a 2 ms span: 64 workers cannot beat 2 ms without changing the work or dependencies. This is a lower bound, not an estimate including queueing or synchronization.

The harness uses seed 33 and a deterministic integer kernel calibrated toward the reported `work_ns` target. That column is a calibration target, not a verified invariant for every node or CPU. Graph creation and oracle construction are outside the event timer; run allocation, initialization, admission, queueing, and completion are inside. Kernel output is compared against a serial oracle. Five shapes exercise chains, root fan-out, eight-way joins, irregular fan-in, and a chain alongside independent tasks. In mixed mode one in four events is foreground; other runs use the normal lane. The CSV reports each class's p99 separately.

Arrivals follow an independent intended schedule (default 2,000 events/s). Latency is intended arrival to worker-recorded terminal timestamp, so a late producer wakeup remains visible. Rejections are counted separately. Samples retain completed runs until reporting; this intentionally costs memory. Percentiles use sorted observations. Throughput is accepted events divided by elapsed arrival-and-validation time; validation overhead can depress it. Queue time and busy time are worker-local accumulated nanoseconds, not OS CPU utilization. Counters include warmup; latency samples do not. Metadata bytes are a calculated subtotal of graph/run/task and worker object storage, excluding allocator overhead, control maps, stacks, vector excess capacity, and library state; this is neither RSS nor peak live memory.

`--full` uses one-second warmups, five repetitions, at least 10,000 samples, and alternates arm order. The quick mode uses three repetitions. It is useful to compare traces and distributions rather than one mean. CPU power, affinity, pacing accuracy, timer granularity, competing load, and instrumentation all affect the result. In particular, sleep-until pacing on Windows can delay arrivals by a timer tick: the resulting latency is a workload finding, not proof of scheduler contention.

## 5. Verification and recorded evidence

The README contains exact build/run commands. Correctness covers all four arms, seeded random DAGs, chains, eight-way joins, fan-out exceeding deque capacity, concurrent submissions, exception propagation, capacity rejection, cancellation, close rejection, and eventual release after dropping the run handle. The separate million-node case runs with 64 workers. Debug and Release checks passed on Windows with standalone Clang 21.1.7; sanitizer status and benchmark provenance are recorded alongside the raw results.

The supplied 16/64-worker table is hypothetical. The full performance matrix, ARM behavior, multi-node NUMA behavior, and Linux TSan are not established by the native correctness checks. A clean result from `--bug relaxed-deps` would not establish safety: its ordinary reads are deliberately racy, and x86 may hide that defect.

Measured evidence: [raw results and provenance](results/README.md). The native smoke run missed the 5 ms p99 target for all three scheduler arms.
