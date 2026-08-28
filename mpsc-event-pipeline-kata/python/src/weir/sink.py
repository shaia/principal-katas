"""The output side.

The consumer hands each batch to a Sink as one contiguous buffer; in production
that Sink writes a socket.

    def write(self, batch: memoryview) -> None:
        # A socket write is partial by nature, so the remainder has to be
        # carried, and the socket must be non-blocking or the consumer stalls
        # every ring behind one slow peer:
        #
        #   self._pending += bytes(batch)
        #   while self._pending:
        #       try:
        #           n = self._sock.send(self._pending)
        #       except BlockingIOError:
        #           break                       # retry next pass
        #       self._pending = self._pending[n:]
        #
        # When the peer stops draining, _pending stops shrinking, the consumer
        # falls behind, the rings fill, and the drop policy fires. That is the
        # whole backpressure chain, and every link in it is bounded. A graceful
        # drain then ends with sock.shutdown(SHUT_WR) so the peer sees a clean
        # EOF rather than a reset.
"""

from __future__ import annotations

from typing import Protocol

from .platform import now_ns, spin_until_ns


class Sink(Protocol):
    def write(self, batch: memoryview) -> None: ...


class CountingSink:
    """Counts bytes so accounting is verifiable, and touches the batch so the
    work cannot be elided.

    The sampling stride matters. A per-byte hash over a 64 KiB batch would make
    the *sink* the bottleneck and every measurement would be reporting the
    checksum instead of the queue — the C++ version shipped exactly that bug and
    caught it by disbelieving its own throughput numbers.
    """

    __slots__ = ("bytes", "batches", "checksum")

    def __init__(self) -> None:
        self.bytes = 0
        self.batches = 0
        self.checksum = 0

    def write(self, batch: memoryview) -> None:
        n = batch.nbytes
        self.bytes += n
        self.batches += 1
        # Touch a bounded sample, O(1)-ish per batch.
        if n:
            self.checksum ^= batch[0] ^ batch[n - 1] ^ n


class CapturingSink:
    """Keeps the raw bytes so a run can be verified AFTER it finishes.

    THIS EXISTS BECAUSE PER-EVENT VERIFICATION ON THE CONSUMER DESTROYS THE RUN.

    Measured, 16 producers x 200k with an inline ordering check in the consumer
    callback:

        with per-event hook:  173,931 accepted, 3,026,069 dropped (94.6% loss)
        without:            3,200,000 accepted,         0 dropped

    The C++ ``std::function`` hook is affordable; its Python equivalent is not,
    by a factor of eighteen. So verification moves off the hot path entirely:
    capture the batch bytes (``bytes(mv)``, ~1 ns/event) and check ordering and
    accounting afterwards with ``struct.Struct.iter_unpack`` (159 ns/event,
    untimed).

    This is the single most important structural difference between the Python
    benchmark and the C++ one, and it is not a stylistic choice.
    """

    __slots__ = ("chunks", "bytes", "batches")

    def __init__(self) -> None:
        self.chunks: list[bytes] = []
        self.bytes = 0
        self.batches = 0

    def write(self, batch: memoryview) -> None:
        self.chunks.append(bytes(batch))
        self.bytes += batch.nbytes
        self.batches += 1

    def payload(self) -> bytes:
        return b"".join(self.chunks)


class SlowSink:
    """Deliberately slow, to drive the rings into overflow and exercise the drop
    policy. Stands in for a socket whose peer has stopped reading.

    It spins rather than sleeping, for the reason platform.py documents: a
    ``time.sleep`` of 400 us on Windows actually takes ~510 us at best and is not
    controllable below that. A sink whose delay is not the delay you asked for is
    not a controlled variable.
    """

    __slots__ = ("delay_ns", "bytes", "batches")

    def __init__(self, delay_ns: int) -> None:
        self.delay_ns = delay_ns
        self.bytes = 0
        self.batches = 0

    def write(self, batch: memoryview) -> None:
        self.bytes += batch.nbytes
        self.batches += 1
        spin_until_ns(now_ns() + self.delay_ns)
