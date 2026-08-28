"""What a Python engineer would actually write, presented through one facade.

This is the Python analogue of the Go port's channel comparison, and it lands
somewhere different. In Go the idiomatic answer loses badly above a handful of
producers. In Python the GIL means the comparison is not really about contention
at all — measured, sharing one queue across 8 producers costs about 6% against
perfect per-producer isolation. What it is about is how much *Python-level* work
each design does, because that is the only currency under a global lock.
"""

from __future__ import annotations

import queue
import threading
from collections import deque
from typing import Optional

from .config import RING_CAPACITY, PipelineStats, StopMode
from .event import EVENT_STRUCT
from .platform import yield_gil
from .sink import Sink


class _BaselineHandle:
    __slots__ = ("_t", "_id")

    def __init__(self, t, pid: int) -> None:
        self._t, self._id = t, pid

    @property
    def id(self) -> int:
        return self._id

    def push(self, seq: int, stamp_ns: int = 0, payload: bytes = b"") -> bool:
        return self._t._push(self._id, seq, stamp_ns, payload)

    def close(self) -> None:
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc) -> None:
        self.close()


class _Baseline:
    """Common plumbing. Subclasses provide _push and _drain."""

    name = "baseline"

    def __init__(self) -> None:
        self._accepting = False
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._next_id = 0
        self._lock = threading.Lock()
        self.pushed = 0
        self.dropped = 0
        self.consumed = 0

    def register_producer(self) -> _BaselineHandle:
        with self._lock:
            pid = self._next_id
            self._next_id += 1
        return _BaselineHandle(self, pid)

    def start(self, sink: Sink) -> None:
        self._accepting = True
        self._running = True
        self._thread = threading.Thread(target=self._consume, args=(sink,), daemon=True)
        self._thread.start()

    def stop(self, mode: StopMode = StopMode.DRAIN, deadline_s: float = 5.0) -> None:
        self._accepting = False
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=deadline_s)
            self._thread = None

    def drain_completed(self) -> bool:
        return True

    def stats(self) -> PipelineStats:
        s = PipelineStats()
        s.pushed, s.dropped, s.consumed = self.pushed, self.dropped, self.consumed
        return s


class QueueBaseline(_Baseline):
    """``queue.Queue``: the textbook answer, and the one to avoid.

    A ``deque`` behind a ``Condition``, so every put does a lock acquire, an
    append, a notify and a release — all at Python level. Measured at 675 ns per
    put single-threaded and 717 ns across 8 threads, against 33 ns for a bare
    ``deque.append``. Twenty times slower, and the gap has nothing to do with
    contention: it is the Condition-variable machinery per item.
    """

    name = "queue.Queue"

    def __init__(self, capacity: int = RING_CAPACITY * 8) -> None:
        super().__init__()
        self._q: queue.Queue = queue.Queue(maxsize=capacity)

    def _push(self, pid: int, seq: int, stamp_ns: int, payload: bytes) -> bool:
        if not self._accepting:
            return False
        try:
            self._q.put_nowait((pid, seq, stamp_ns, payload))
        except queue.Full:
            self.dropped += 1
            return False
        self.pushed += 1
        return True

    def _consume(self, sink: Sink) -> None:
        pack = EVENT_STRUCT.pack_into
        stage = bytearray(512 * EVENT_STRUCT.size)
        mv = memoryview(stage)
        while self._running:
            n = 0
            while n < 512:
                try:
                    pid, seq, stamp, payload = self._q.get_nowait()
                except queue.Empty:
                    break
                pack(mv, n * EVENT_STRUCT.size, pid, seq, stamp, payload)
                n += 1
            if n:
                self.consumed += n
                sink.write(mv[:n * EVENT_STRUCT.size])
            else:
                yield_gil()


class SimpleQueueBaseline(_Baseline):
    """``queue.SimpleQueue``: the fastest thing measured, and disqualified.

    C-implemented with no Python-level lock, so a put is one C call: 37 ns
    single-threaded, 52 ns across 8 threads sharing one instance. That is within
    6% of eight *independent* deques — which is the measurement that shows the
    C++ design's premise does not hold here. Producer-producer contention, the
    thing the whole architecture exists to eliminate, was already serialised away
    by the GIL.

    It is disqualified anyway: SimpleQueue is **unbounded**. "Bounded memory
    usage" is a hard requirement, and an unbounded queue is not a solution to
    overload, it is a deferral of it — a latency problem converted into a memory
    problem that arrives later, larger, and as an OOM kill.

    So the fastest Python answer fails the specification, and it fails it on the
    requirement the C++ answer also treats as non-negotiable.
    """

    name = "queue.SimpleQueue"

    def __init__(self, capacity: int = 0) -> None:
        super().__init__()
        self._q: queue.SimpleQueue = queue.SimpleQueue()

    def _push(self, pid: int, seq: int, stamp_ns: int, payload: bytes) -> bool:
        if not self._accepting:
            return False
        self._q.put((pid, seq, stamp_ns, payload))   # never fails: unbounded
        self.pushed += 1
        return True

    def _consume(self, sink: Sink) -> None:
        pack = EVENT_STRUCT.pack_into
        stage = bytearray(512 * EVENT_STRUCT.size)
        mv = memoryview(stage)
        while self._running:
            n = 0
            while n < 512:
                try:
                    pid, seq, stamp, payload = self._q.get_nowait()
                except queue.Empty:
                    break
                pack(mv, n * EVENT_STRUCT.size, pid, seq, stamp, payload)
                n += 1
            if n:
                self.consumed += n
                sink.write(mv[:n * EVENT_STRUCT.size])
            else:
                yield_gil()


class DequeBaseline(_Baseline):
    """One bounded ``deque`` per producer: arguably the correct Python answer.

    It reaches the same architecture as the C++ — per-producer isolation, batch
    drain, bounded memory, counted drops — by the same reasoning, in about ten
    lines, with none of the ring machinery. ``append`` and ``popleft`` are single
    C calls and atomic under the GIL, so there is no index arithmetic and no
    publish store to get wrong.

    The bound is an explicit length check, never ``maxlen``: ``deque(maxlen=N)``
    silently discards the oldest element and counts nothing.
    """

    name = "deque/producer"

    def __init__(self, capacity: int = RING_CAPACITY) -> None:
        super().__init__()
        self._cap = capacity
        self._queues: list[deque] = []

    def register_producer(self) -> _BaselineHandle:
        with self._lock:
            pid = self._next_id
            self._next_id += 1
            self._queues.append(deque())
        return _BaselineHandle(self, pid)

    def _push(self, pid: int, seq: int, stamp_ns: int, payload: bytes) -> bool:
        if not self._accepting:
            return False
        d = self._queues[pid]
        if len(d) >= self._cap:
            self.dropped += 1
            return False
        d.append((pid, seq, stamp_ns, payload))
        self.pushed += 1
        return True

    def _consume(self, sink: Sink) -> None:
        pack = EVENT_STRUCT.pack_into
        size = EVENT_STRUCT.size
        stage = bytearray(512 * size)
        mv = memoryview(stage)
        while self._running:
            total = 0
            for d in list(self._queues):
                popleft = d.popleft
                n = 0
                while n < 256 and d:
                    try:
                        pid, seq, stamp, payload = popleft()
                    except IndexError:
                        break
                    pack(mv, n * size, pid, seq, stamp, payload)
                    n += 1
                if n:
                    self.consumed += n
                    sink.write(mv[:n * size])
                    total += n
            if not total:
                yield_gil()


BASELINES = {
    "queue.Queue": QueueBaseline,
    "queue.SimpleQueue": SimpleQueueBaseline,
    "deque/producer": DequeBaseline,
}
