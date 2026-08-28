"""The asyncio pipeline: the right answer when producers are I/O-bound.

Everything the threaded design has to defend against — preemption between two
statements, lost wakeups, atomicity of an index update — simply cannot occur on
a single-threaded event loop. What remains is bounded buffers, a terminating
overload policy, batching, and accounting, which is the part of the design that
was never about concurrency.
"""

from __future__ import annotations

import asyncio

import pytest

from weir import EVENT_STRUCT, RING_CAPACITY
from weir.aio import AsyncEventPipeline


def _decode(blobs: list[bytes]):
    return [e for b in blobs for e in EVENT_STRUCT.iter_unpack(b)]


@pytest.mark.asyncio_compat
def test_async_pipeline_preserves_order_and_accounting():
    async def run():
        out: list[bytes] = []
        async with AsyncEventPipeline(out.append) as pipe:
            producers = [pipe.producer(f"p{i}") for i in range(8)]

            async def emit(p, n):
                for s in range(n):
                    p.push(s)
                    if s % 256 == 0:
                        await pipe.drain_pressure(p)
                        await asyncio.sleep(0)

            await asyncio.gather(*(emit(p, 5_000) for p in producers))
            await asyncio.sleep(0.05)

        events = _decode(out)
        totals = pipe.totals()

        assert totals.pushed + totals.dropped == 8 * 5_000, "every push accounted for"
        assert len(events) == totals.pushed, "drain lost nothing"
        assert totals.high_water <= RING_CAPACITY, "memory stayed bounded"

        # Per-producer ordering. seq is monotonic within each producer id.
        last: dict[int, int] = {}
        for pid, seq, _stamp, _pl in events:
            assert seq > last.get(pid, -1), "per-producer order preserved"
            last[pid] = seq

    asyncio.run(run())


@pytest.mark.asyncio_compat
def test_async_backpressure_prefers_waiting_over_dropping():
    """The thing neither the C++ nor the threaded Python answer can offer.

    A coroutine can be told to wait without blocking anything else, so overload
    becomes a scheduling decision rather than data loss. With drain_pressure in
    the loop there should be no drops at all.
    """
    async def run():
        out: list[bytes] = []
        async with AsyncEventPipeline(out.append, capacity=512) as pipe:
            p = pipe.producer("chatty")
            for s in range(20_000):
                while not p.push(s):
                    await pipe.drain_pressure(p, threshold=0.25)
                await pipe.drain_pressure(p)
            await asyncio.sleep(0.05)

        assert pipe.totals().dropped == 0, (
            "with backpressure available, overload should cost latency, not events")
        assert len(_decode(out)) == 20_000

    asyncio.run(run())


@pytest.mark.asyncio_compat
def test_async_drop_policy_still_terminates_without_backpressure():
    """Without awaiting, the bound must still hold and drops must be counted."""
    async def run():
        out: list[bytes] = []
        async with AsyncEventPipeline(out.append, capacity=256) as pipe:
            p = pipe.producer("firehose")
            for s in range(10_000):
                p.push(s)          # never awaits: the consumer never runs
            await asyncio.sleep(0.05)

        t = pipe.totals()
        assert t.dropped > 0, "a bounded buffer with no drain must drop"
        assert t.pushed + t.dropped == 10_000, "and count every one"
        assert t.high_water <= 256, "and stay bounded"

    asyncio.run(run())
