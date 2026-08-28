"""Benchmark harness: assertions, percentiles, and the open-loop pacer."""

from __future__ import annotations

import threading
from dataclasses import dataclass

from weir.platform import now_ns

_lock = threading.Lock()
_failures = 0


def check(cond: bool, what: str) -> None:
    """Records a failure and prints it. The process exit code is derived from
    failures(), so a violated invariant fails the run rather than only the eye of
    whoever is reading the output."""
    global _failures
    if cond:
        return
    with _lock:
        _failures += 1
    print(f"  FAIL: {what}")


def failures() -> int:
    return _failures


@dataclass
class Percentiles:
    p50: float = 0.0
    p99: float = 0.0
    p999: float = 0.0
    max: float = 0.0


def percentiles(v: list[int]) -> Percentiles:
    """Sorts in place, returns microseconds.

    Percentiles, never means: the entire claim here is about tails, and a mean
    averages away exactly the events under discussion.
    """
    if not v:
        return Percentiles()
    v.sort()
    n = len(v)

    def at(q: float) -> float:
        return v[min(n - 1, int(q * (n - 1)))] / 1000

    return Percentiles(at(0.50), at(0.99), at(0.999), v[-1] / 1000)


def median_of(reps: list, get) -> float:
    """Median across repetitions per statistic, rather than picking one 'median
    run'. Choosing a whole run by its p99.9 lets one noisy percentile drag
    unrelated columns along with it."""
    if not reps:
        return 0.0
    vals = sorted(get(r) for r in reps)
    return vals[len(vals) // 2]


def pace(rate_per_sec: int, total: int, emit) -> None:
    """Open-loop load generator. ``emit(seq, intended_ns)`` is called on schedule.

    THE SCHEDULE IS FIXED FROM A SINGLE ORIGIN. The intended time is
    ``origin + n*period``, never ``now + period``: a late send must not push the
    schedule back, because that is precisely coordinated omission — the classic
    latency-benchmark lie, in which a stalled system stops receiving load so the
    stall contributes few or no samples and the recorded latency looks fine. It
    always lies in the flattering direction.

    IT SPINS RATHER THAN SLEEPING, and it must. Measured on Windows,
    ``time.sleep`` requests of 1, 10, 50 and 100 us all return in ~510-560 us.
    There is no way to ask for a shorter wait, so at the 10 us periods this
    benchmark uses, sleeping would quantize every send to 50 periods late.

    A pure spin holds the GIL, though, which is a cost the C++ version does not
    have: there, a spinning pacer costs a core; here it costs the *interpreter*,
    stalling the very consumer whose latency is being measured. That caps the
    number of paced producers at about four, and the cap is set by the GIL rather
    than by core count.
    """
    period_ns = 1_000_000_000 // rate_per_sec
    origin = now_ns()
    for s in range(total):
        intended = origin + s * period_ns
        while now_ns() < intended:
            pass
        emit(s, intended)
