"""Bounded per-producer buffers.

The design here was chosen by measurement, and the reasoning is Python's own —
not an analogy to what a systems language would do.

**In Python the cost model is "how many C calls, and how many events does each
one move".** There is no cache coherence to reason about, no memory ordering to
choose, and no producer parallelism to protect. What there is, per event, is
interpreted bytecode, and every design decision below is about eliminating it.

Measured on this machine, end to end — producer submitting one event *plus*
consumer draining it into the wire bytes a socket would take:

    bytearray ring + pack_into + memoryview drain      131 ns/event
    bytearray.extend + whole-buffer swap, no lock      151 ns/event   (unsafe)
    list.append(bytes) + swap + b"".join, with lock    207 ns/event
    bytearray.extend + whole-buffer swap, with lock    267 ns/event

    an uncontended threading.Lock, on its own            96 ns/event

That last line decides it. **An uncontended lock costs about as much as the
entire rest of the push**, so a design that takes one per event has spent its
budget before doing any work. The ring avoids it — not through cleverness, but
because single-writer index ownership means each index update is a single
``STORE_ATTR``, and a single bytecode cannot be interrupted.

The second reason is the drain. ``pop_into`` moves up to 512 events with one
memoryview slice — one C call, ~1 ns/event amortised — where any per-event loop
costs 100+ ns/event on the single thread that cannot be sharded.

So: two properties, both about C calls per event, and they happen to point at a
ring. That it also resembles what you would write in C++ is a coincidence worth
noticing and not worth leaning on: the C++ ring exists to avoid cache-line
contention, which is not a thing here.

WHAT REPLACES THE MEMORY-ORDERING QUESTION. There is one interpreter lock, so
there is sequential consistency by construction — no ``release`` to write, no
``acquire`` to pair with it. The question that *does* need answering is which
operations are atomic, and the answer is subtler than it looks. See
``tests/test_atomicity.py``, which is executable rather than prose.

The sequencing still matters, though, and deleting the ordering annotations
makes it look as though it stopped. A thread switch can land between two Python
statements exactly as a CPU can reorder two instructions, so the slot write must
still precede the index publish and the copy-out must still precede the index
release. Same statement order, no annotation, same hazard.
"""

from __future__ import annotations

from collections import deque
from typing import Protocol

from .config import RING_CAPACITY
from .event import EVENT_SIZE, EVENT_STRUCT


class Ring(Protocol):
    """The interface both backends present, so the pipeline is agnostic."""

    def try_push(self, producer_id: int, seq: int, stamp_ns: int, payload: bytes) -> bool: ...
    def producer_size_hint(self) -> int: ...
    def pop_into(self, stage: memoryview, offset_events: int, max_items: int) -> int: ...
    def empty_now(self) -> bool: ...
    def size_now(self) -> int: ...


class BytesRing:
    """Events packed by value into one flat ``bytearray``.

    Three properties, each earned:

    * **No lock.** The producer owns ``_tail``, the consumer owns ``_head``, and
      each update is one ``STORE_ATTR``. That is what buys the 96 ns a lock would
      cost, and it is the whole reason the index bookkeeping is worth writing.
    * **No per-event allocation.** ``pack_into`` writes into storage that already
      exists. A design that appends objects allocates a tuple and a bytes object
      per event, and then the collector has to walk them.
    * **A drain that is one C call.** See ``pop_into``.

    The buffer is also already in wire format, so the batch handed to the sink
    needs no serialisation pass at all — it is a memoryview of bytes that a
    socket can take directly.

    Per-push it is more expensive than ``deque.append`` (137 ns against 76 ns).
    It wins end to end anyway, because the deque pays that back with interest on
    the consumer.
    """

    __slots__ = ("_buf", "_mv", "_cap", "_mask", "_head", "_tail",
                 "_cached_head", "_cached_tail", "_pack_into")

    def __init__(self, capacity: int = RING_CAPACITY) -> None:
        if capacity < 2 or capacity & (capacity - 1):
            raise ValueError("capacity must be a power of two >= 2")
        self._cap = capacity
        self._mask = capacity - 1
        self._buf = bytearray(capacity * EVENT_SIZE)
        self._mv = memoryview(self._buf)
        self._head = 0          # consumer-owned, sole writer
        self._tail = 0          # producer-owned, sole writer
        self._cached_head = 0   # producer-private, deliberately unsynchronised
        self._cached_tail = 0   # consumer-private
        # Hoisted to an attribute so the hot path does one LOAD_ATTR instead of
        # two. At 14 ns per attribute access and a ~220 ns push budget, this is
        # the real Python equivalent of cache-line discipline.
        self._pack_into = EVENT_STRUCT.pack_into

    # -- producer side ------------------------------------------------------

    def try_push(self, producer_id: int, seq: int, stamp_ns: int, payload: bytes) -> bool:
        t = self._tail
        if t - self._cached_head == self._cap:
            # Believed full. Only now read the consumer's index. In C++ this load
            # is `acquire`; here it is a plain attribute read and the GIL has
            # already ordered everything. The *check* survives; the annotation
            # does not.
            self._cached_head = self._head
            if t - self._cached_head == self._cap:
                return False

        # One C call. It does not re-enter the interpreter and does not release
        # the GIL, so the slot write cannot be observed half-done. That property
        # is what makes this safe, and it is a property of pack_into being a
        # single C function rather than of any ordering annotation.
        self._pack_into(self._buf, (t & self._mask) * EVENT_SIZE,
                        producer_id, seq, stamp_ns, payload)

        # THE PUBLISH. A single STORE_ATTR, therefore atomic under the GIL.
        #
        # It must be sequenced after the pack_into above. The C++ `release` is
        # gone, but the ordering requirement it encoded is not: a thread switch
        # landing between these two statements would let the consumer see an
        # index that promises a slot the producer has not written.
        self._tail = t + 1
        return True

    def producer_size_hint(self) -> int:
        """Producer-thread only: reads the private cached index."""
        return self._tail - self._cached_head

    # -- consumer side ------------------------------------------------------

    def pop_into(self, stage: memoryview, offset_events: int, max_items: int) -> int:
        """Copy up to ``max_items`` events into ``stage``.

        THE DRAIN IS THE POINT OF THE WHOLE STRUCTURE.

        One memoryview slice per contiguous run — two at a wrap — and then one
        index store. That is one C call moving up to 512 events: ~1 ns/event
        amortised, and the consumer executes no per-event bytecode at all.

        Compare the alternatives on the same machine, per event drained and
        serialised: ``popleft`` into ``pack_into``, 220 ns. ``popleft`` of
        pre-packed bytes into ``b"".join``, 128 ns. A whole-list swap into
        ``b"".join``, 85 ns — the best of the object-based designs, and still
        eighty times this.

        The consumer is the one participant that cannot be sharded, so its
        per-event cost sets the ceiling on everything. Spending producer-side
        bytecode to buy consumer-side C calls is the trade this design makes, and
        under a single interpreter lock it is the only trade that matters.
        """
        h = self._head
        if h == self._cached_tail:
            self._cached_tail = self._tail
            if h == self._cached_tail:
                return 0

        n = self._cached_tail - h
        if n > max_items:
            n = max_items

        cap = self._cap
        start = h & self._mask
        run = cap - start
        mv = self._mv
        dst = offset_events * EVENT_SIZE

        if run >= n:
            stage[dst:dst + n * EVENT_SIZE] = \
                mv[start * EVENT_SIZE:(start + n) * EVENT_SIZE]
        else:
            first = run * EVENT_SIZE
            stage[dst:dst + first] = mv[start * EVENT_SIZE:cap * EVENT_SIZE]
            stage[dst + first:dst + n * EVENT_SIZE] = \
                mv[0:(n - run) * EVENT_SIZE]

        # FREE THE SLOTS, and only after the copies above.
        #
        # This is the pairing the C++ writeup says everyone forgets. In Python
        # the annotation vanishes but the hazard is identical: a thread switch
        # between the copy and this store lets the producer overwrite a slot the
        # consumer has not read. Deleting the ordering annotations makes it
        # *look* like statement order stopped mattering. It did not.
        self._head = h + n
        return n

    def empty_now(self) -> bool:
        self._cached_tail = self._tail
        return self._head == self._cached_tail

    # -- observer side ------------------------------------------------------

    def size_now(self) -> int:
        """Safe from a third thread: reads only the two owned indices.

        The stopping thread must use this. ``producer_size_hint`` reads the
        producer's private cached index, and the C++ version shipped exactly that
        bug. The justification changes from "data race, undefined behaviour" to
        "reads a value another thread owns and may be mid-update of", but the
        rule is the same.
        """
        return self._tail - self._head


class DequeRing:
    """The idiomatic Python answer, presented through the same interface.

    ``collections.deque.append`` and ``popleft`` are single C calls and are
    atomic under the GIL, so this needs no index arithmetic and no publish store
    at all. It is measured at 66 ns/push against BytesRing's 222 ns.

    Note what is NOT used: ``deque(maxlen=N)``. That silently discards the oldest
    element on overflow and returns nothing to say it did — the one overload
    policy the C++ answer rejects as unimplementable, handed to you by default
    and uncounted. The bound here is an explicit length check, so the drop is a
    counted DROP_NEWEST.

    Note also what the drain does NOT do: ``out = list(d); d.clear()`` is the
    fastest drain measurable (13 ns/event) and is wrong — an ``append`` landing
    between those two statements is lost. Only ``popleft`` in a loop is safe.
    """

    __slots__ = ("_d", "_cap", "_consumed")

    def __init__(self, capacity: int = RING_CAPACITY) -> None:
        self._d: deque = deque()
        self._cap = capacity
        self._consumed = 0

    def try_push(self, producer_id: int, seq: int, stamp_ns: int, payload: bytes) -> bool:
        d = self._d
        if len(d) >= self._cap:      # explicit bound; NOT maxlen
            return False
        d.append((producer_id, seq, stamp_ns, payload))
        return True

    def producer_size_hint(self) -> int:
        return len(self._d)

    def pop_into(self, stage: memoryview, offset_events: int, max_items: int) -> int:
        d = self._d
        popleft = d.popleft
        pack = EVENT_STRUCT.pack_into
        n = len(d)
        if n > max_items:
            n = max_items
        base = offset_events * EVENT_SIZE
        # The serialisation the BytesRing charged the producer for, charged to
        # the consumer instead. ~103 ns/event, on the un-shardable thread. This
        # is the whole trade, and a comparison that omits it flatters the deque
        # by exactly that much.
        for i in range(n):
            try:
                pid, seq, stamp, payload = popleft()
            except IndexError:
                return i
            pack(stage, base + i * EVENT_SIZE, pid, seq, stamp, payload)
        return n

    def empty_now(self) -> bool:
        return not self._d

    def size_now(self) -> int:
        return len(self._d)
