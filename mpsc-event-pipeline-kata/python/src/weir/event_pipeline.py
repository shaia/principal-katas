"""The threaded pipeline: per-producer buffers drained by one consumer thread.

Use this when producers are genuinely threads. If they are coroutines — which,
for anything emitting telemetry at volume in Python, they usually are — read
``weir.aio`` first: on a single event loop most of what follows stops being a
problem at all.

Three things here are subtle enough to name:

    producer lifetime   FREE -> ACTIVE -> RETIRING -> FREE. Only the consumer
                        publishes FREE, and only after draining the buffer. A
                        departing producer cannot release its own slot, because
                        it has no way to know whether the consumer is mid-batch
                        inside it.
    wakeup handshake    Each side publishes its own state before reading the
                        other's, so no interleaving can leave both believing the
                        other will act. Under one interpreter lock this needs no
                        barriers — only the statement order.
    shutdown            Two phases, two modes, always bounded by a deadline.
                        Stop accepting first; a queue still being filled is one
                        you can fail to drain indefinitely.
"""

from __future__ import annotations

import threading
from typing import Callable, Optional

from .active_set import ActiveSet
from .config import (DRAIN_BATCH, MAX_PRODUCERS, PipelineConfig, PipelineStats,
                     RING_CAPACITY, ScanPolicy, StopMode, FullPolicy)
from .event import EVENT_SIZE, EMPTY_PAYLOAD
from .platform import now_ns, yield_gil
from .sink import Sink
from .spsc_ring import BytesRing

FREE, ACTIVE, RETIRING = 0, 1, 2


class _Slot:
    """One producer's ring and its counters.

    Counters are per-slot with a single writer — the producer — so ``+= 1`` is
    safe here for the same reason it is unsafe on a shared counter. That is the
    C++ design's own answer to the same problem, and it ports directly.
    """

    __slots__ = ("ring", "state", "pushed", "dropped", "high_water", "flag_writes")

    def __init__(self, ring_factory) -> None:
        self.ring = ring_factory()
        self.state = FREE
        self.pushed = 0
        self.dropped = 0
        self.high_water = 0
        # Per-slot, single writer, for the same reason as pushed/dropped: a
        # shared counter incremented from every producer is exactly the
        # multi-bytecode read-modify-write that active_set.py exists to avoid.
        # Counting the mechanism's own cost must not itself be the bug.
        self.flag_writes = 0


class ProducerHandle:
    """A producer's ticket. Use it as a context manager::

        with pipeline.register_producer() as p:
            p.push(seq)

    There is deliberately **no ``__del__``**. It is not guaranteed to run, it
    runs at a time the collector chooses, and exceptions inside it are swallowed
    — so it cannot implement a handoff protocol whose entire value is happening
    at a known moment. ``__exit__`` and ``close()`` are the tools that actually
    run when you need them to.

    That does mean a caller who ignores both leaks one slot out of sixty-four,
    with no language mechanism to prevent it. Worth stating rather than papering
    over: it is the one place where the deterministic-cleanup story is genuinely
    weaker than a scope-bound language's.
    """

    __slots__ = ("_pipe", "_id", "_closed")

    def __init__(self, pipe: "EventPipeline", pid: int) -> None:
        self._pipe = pipe
        self._id = pid
        self._closed = False

    @property
    def id(self) -> int:
        return self._id

    def push(self, seq: int, stamp_ns: int = 0, payload: bytes = EMPTY_PAYLOAD) -> bool:
        return self._pipe._push(self._id, seq, stamp_ns, payload)

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self._pipe._retire(self._id)

    def __enter__(self) -> "ProducerHandle":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


class EventPipeline:
    def __init__(self, cfg: Optional[PipelineConfig] = None,
                 ring_factory=BytesRing) -> None:
        self.cfg = cfg or PipelineConfig()
        self._slots = [_Slot(ring_factory) for _ in range(MAX_PRODUCERS)]
        self._active = ActiveSet(MAX_PRODUCERS)

        # Plain bool attributes, not threading.Event: is_set() is 40 ns and
        # set() is 576 ns, against a ~220 ns push budget. _accepting is read on
        # every push, so a 40 ns check would be a 20% tax for nothing. Events are
        # used only where cost is irrelevant.
        self._accepting = False
        self._running = False

        self._retire_pending = False
        self._reg_lock = threading.Lock()
        self._stop_lock = threading.Lock()
        self._stopped = False
        self._drain_met = False
        self._thread: Optional[threading.Thread] = None

        # Consumer-local; no other thread touches these.
        self._stage = bytearray(self.cfg.stage_events * EVENT_SIZE)
        self._stage_mv = memoryview(self._stage)
        self._staged = 0
        self._consumed = 0
        self._rings_probed = 0
        self._rings_empty = 0
        self._passes = 0
        self._flag_writes = 0
        self._retired_pushed = 0
        self._retired_dropped = 0

        self.on_batch: Optional[Callable[[memoryview], None]] = None

    # -- lifecycle -----------------------------------------------------------

    def start(self, sink: Sink) -> None:
        self._running = True
        self._accepting = True
        # daemon=True so a forgotten stop() cannot hang interpreter exit. The
        # C++ destructor made stop optional; nothing here does.
        self._thread = threading.Thread(target=self._consume, args=(sink,),
                                        name="weir-consumer", daemon=True)
        self._thread.start()

    def stop(self, mode: StopMode = StopMode.DRAIN, deadline_s: float = 5.0) -> None:
        """Idempotent, and bounded by a deadline."""
        with self._stop_lock:
            if self._stopped or self._thread is None:
                return
            self._stopped = True

            # Producers see this and start failing pushes, so the rings can only
            # shrink from here. This must come first: draining a queue that is
            # still being filled is a race you can lose indefinitely.
            self._accepting = False

            if mode is StopMode.DRAIN:
                until = now_ns() + int(deadline_s * 1e9)
                while now_ns() < until and not self._all_quiesced():
                    self._active.wake()
                    yield_gil()
                self._drain_met = self._all_quiesced()

            self._running = False
            self._active.wake()
            self._thread.join(timeout=deadline_s)

    def drain_completed(self) -> bool:
        return self._drain_met

    def register_producer(self, wait_s: float = 1.0) -> Optional[ProducerHandle]:
        """Claim a slot, waiting briefly for the consumer to reclaim one.

        The wait is not incidental. Only the consumer may publish FREE, and it
        does so only on an idle pass — which is correct, because reclaiming is an
        O(64) scan that would otherwise run on every iteration and cancel out the
        bitmap. In C++ and Go the producers are fast enough relative to the
        consumer that idle passes are plentiful and a registry of 64 never runs
        dry.

        In Python they are not. A churn workload retires 8 producers and
        immediately registers 8 more, while the consumer — sharing one
        interpreter with all of them — has not had an idle pass in which to
        reclaim anything. Registration then fails on a registry that is entirely
        RETIRING and about to be free.

        So the caller waits, and wakes the consumer so the wait is short. The
        invariant is untouched: the consumer is still the only thread that
        publishes FREE. This is a scheduling accommodation, not a protocol
        change, and it is a genuinely Python-specific one.
        """
        deadline = now_ns() + int(wait_s * 1e9)
        while True:
            with self._reg_lock:
                for i, slot in enumerate(self._slots):
                    if slot.state == FREE:
                        slot.pushed = 0
                        slot.dropped = 0
                        slot.high_water = 0
                        slot.state = ACTIVE
                        self._refresh_registered()
                        return ProducerHandle(self, i)
            if now_ns() >= deadline or not self._running:
                return None  # registry genuinely full — caller checks
            self._active.wake()
            yield_gil()

    def _refresh_registered(self) -> None:
        self._active.set_registered(
            tuple(i for i, s in enumerate(self._slots) if s.state != FREE))

    def stats(self) -> PipelineStats:
        s = PipelineStats()
        s.pushed = self._retired_pushed
        s.dropped = self._retired_dropped
        for slot in self._slots:
            s.pushed += slot.pushed
            s.dropped += slot.dropped
            s.flag_writes += slot.flag_writes
            s.high_water = max(s.high_water, slot.high_water)
        s.consumed = self._consumed
        s.rings_probed = self._rings_probed
        s.rings_empty = self._rings_empty
        s.passes = self._passes
        s.flag_writes += self._flag_writes
        s.notifies = self._active.notifies
        s.parks = self._active.parks
        return s

    # -- producer hot path ---------------------------------------------------

    def _push(self, pid: int, seq: int, stamp_ns: int, payload: bytes) -> bool:
        if not self._accepting:
            return False
        slot = self._slots[pid]
        ring = slot.ring

        ok = ring.try_push(pid, seq, stamp_ns, payload)
        if not ok and self.cfg.full is FullPolicy.SPIN_THEN_DROP:
            for _ in range(self.cfg.push_spin):
                yield_gil()
                ok = ring.try_push(pid, seq, stamp_ns, payload)
                if ok:
                    break
        if not ok:
            slot.dropped += 1      # per-slot, single writer: safe
            return False           # loss is counted, never silent
        slot.pushed += 1

        depth = ring.producer_size_hint()
        if depth > slot.high_water:
            slot.high_water = depth

        if self.cfg.scan is ScanPolicy.BITMAP and self._active.signal(pid):
            # Counted per-slot, single writer. A shared counter here would be
            # the very read-modify-write hazard active_set.py exists to avoid —
            # instrumenting the mechanism must not itself be the bug.
            slot.flag_writes += 1
        return True

    def _retire(self, pid: int) -> None:
        self._slots[pid].state = RETIRING
        self._retire_pending = True
        if self.cfg.scan is ScanPolicy.BITMAP:
            self._active.signal(pid)
        self._active.wake()

    # -- consumer ------------------------------------------------------------

    def _consume(self, sink: Sink) -> None:
        idle = 0
        rotate = 0
        bitmap = self.cfg.scan is ScanPolicy.BITMAP

        while self._running:
            n = (self._drain_bitmap(sink, rotate) if bitmap
                 else self._drain_full(sink, rotate))
            self._passes += 1
            rotate = (rotate + 1) & (MAX_PRODUCERS - 1)

            if n:
                idle = 0
                continue

            if self._retire_pending:
                self._retire_pending = False
                self._reclaim_retired()

            idle += 1
            if idle < self.cfg.yields_before_poll:
                # THE SPIN RUNG IS DELETED, NOT TRANSLATED. A busy-spin here
                # would hold the GIL and starve the producers it is waiting for
                # — not merely useless, actively negative. time.sleep(0) is the
                # only correct yield: it releases and reacquires the GIL in
                # ~193 ns, letting a producer run.
                yield_gil()
                continue

            if bitmap:
                self._clear_empty_flags()

            # Defense in depth: one unconditional full scan immediately before
            # sleeping. Going to sleep is the only moment where being wrong
            # becomes unbounded, and it is also the moment an O(64) scan is
            # affordable.
            if self._drain_full(sink, rotate):
                idle = 0
                continue

            self._active.park(self.cfg.park_timeout_s, self._running)
            idle = 0

        # Final passes: whatever is published still goes out, so a DRAIN
        # shutdown loses nothing. Full scan regardless of policy — at shutdown,
        # correctness beats scan efficiency.
        for _ in range(2):
            r = 0
            while self._drain_full(sink, r):
                r = (r + 1) & (MAX_PRODUCERS - 1)
        self._flush(sink)
        self._reclaim_retired()

    def _drain_full(self, sink: Sink, rotate: int) -> int:
        total = 0
        slots = self._slots
        for k in range(MAX_PRODUCERS):
            i = (k + rotate) & (MAX_PRODUCERS - 1)
            if slots[i].state == FREE:
                continue
            self._rings_probed += 1
            n = self._drain_ring(sink, i)
            if not n:
                self._rings_empty += 1
            total += n
        if total:
            self._flush(sink)
        return total

    def _drain_bitmap(self, sink: Sink, rotate: int) -> int:
        ids = self._active.active_ids()
        if not ids:
            return 0
        # Fairness: rotate the starting point so low indices are not always
        # drained first. Under sustained overload a fixed order starves the tail
        # of the list.
        if rotate and len(ids) > 1:
            k = rotate % len(ids)
            ids = ids[k:] + ids[:k]
        total = 0
        for i in ids:
            self._rings_probed += 1
            n = self._drain_ring(sink, i)
            if not n:
                self._rings_empty += 1
            total += n
        if total:
            self._flush(sink)
        return total

    def _drain_ring(self, sink: Sink, i: int) -> int:
        got = 0
        ring = self._slots[i].ring
        cap = self.cfg.stage_events
        while got < DRAIN_BATCH:
            if self._staged == cap:
                self._flush(sink)
            room = min(cap - self._staged, DRAIN_BATCH - got)
            n = ring.pop_into(self._stage_mv, self._staged, room)
            if not n:
                break
            self._staged += n
            got += n
        self._consumed += got
        return got

    def _flush(self, sink: Sink) -> None:
        if not self._staged:
            return
        batch = self._stage_mv[:self._staged * EVENT_SIZE]
        sink.write(batch)
        if self.on_batch is not None:
            self.on_batch(batch)
        self._staged = 0

    def _clear_empty_flags(self) -> None:
        for i in self._active.active_ids():
            slot = self._slots[i]
            self._flag_writes += self._active.clear_if_empty(
                i, slot.ring, slot.state == RETIRING)

    def _reclaim_retired(self) -> None:
        """Only the consumer publishes FREE, and only after the ring is empty."""
        changed = False
        for i, slot in enumerate(self._slots):
            if slot.state != RETIRING or not slot.ring.empty_now():
                continue
            # Roll the departing producer's counters into lifetime totals before
            # the slot is reusable. register_producer resets the per-slot ones,
            # so without this a recycled slot erases its predecessor's history
            # and the accounting silently stops adding up. That was a real bug
            # in the C++ version.
            self._retired_pushed += slot.pushed
            self._retired_dropped += slot.dropped
            slot.pushed = 0
            slot.dropped = 0
            self._active.clear(i)
            slot.state = FREE
            changed = True
        if changed:
            self._refresh_registered()

    def _all_quiesced(self) -> bool:
        """Called from the stopping thread, which is neither producer nor
        consumer, so it must use size_now(). producer_size_hint() reads the
        producer's private cached index — the C++ version shipped exactly that
        bug."""
        return all(s.state == FREE or s.ring.size_now() == 0 for s in self._slots)

    # -- context manager -----------------------------------------------------

    def __enter__(self) -> "EventPipeline":
        return self

    def __exit__(self, *exc) -> None:
        self.stop(StopMode.ABORT)
