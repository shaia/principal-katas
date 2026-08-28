"""Platform constants and the one global that dominates every measurement here.

The C++ equivalent of this module is cache-line size and a spin-wait hint. In
Python neither exists in any observable way, and what replaces them is the
interpreter's thread switch interval.
"""

from __future__ import annotations

import sys
import time

# ---------------------------------------------------------------------------
# The single most impactful line in this package
# ---------------------------------------------------------------------------
#
# CPython releases the GIL every `sys.getswitchinterval()` seconds, default 5 ms.
# Five milliseconds is an enormous amount of time at these rates: a producer
# holding the GIL fills its entire 4096-slot ring in roughly the first 0.5 ms and
# then spends the remaining 4.5 ms dropping, while the consumer — which needs the
# GIL to drain — does not get scheduled at all.
#
# Measured on the development machine, 4 producers:
#
#     interval    accepted   sustained
#     5 ms          2.0%      0.53 M/s     <- the default
#     1 ms          3.1%      0.72
#     0.5 ms       38.3%      5.25
#     0.1 ms       43.6%      5.99
#     0.01 ms      49.6%      6.10
#
# 97% of events lost to a scheduler setting, and 11.5x throughput from one line.
# That dwarfs every data-structure choice in this package.
#
# This is the honest Python replacement for the C++ answer's section 3. There is
# no false sharing to avoid and no cache line to pad; the convoy effect of a
# coarse GIL handoff is the equivalent phenomenon, and it is far larger.
SWITCH_INTERVAL = 0.0005

sys.setswitchinterval(SWITCH_INTERVAL)

# ---------------------------------------------------------------------------
# Timing
# ---------------------------------------------------------------------------

now_ns = time.perf_counter_ns

# Measured on Windows: time.sleep() requests of 1, 10, 50 and 100 us all return
# in ~510-560 us. There is no way to ask for a shorter wait. Anything that needs
# sub-millisecond timing must spin on perf_counter_ns, which costs ~40-120 ns per
# poll (against ~25 ns for the C++ steady_clock read).
#
# The C++ writeup records the same class of bug with sleep_for rounding to the
# Windows timer tick. Python's version is worse, because its clock read is more
# expensive too.
SLEEP_FLOOR_NS = 500_000


def yield_gil() -> None:
    """The only correct idle primitive in a GIL interpreter.

    A busy-spin here would be worse than useless: it holds the GIL and starves
    the very producers the consumer is waiting for. `time.sleep(0)` releases and
    reacquires the GIL in ~193 ns, letting a producer run. There is no Python
    equivalent of `_mm_pause`, and the C++ idle ladder's first rung simply does
    not exist here — it is deleted, not translated.
    """
    time.sleep(0)


def spin_until_ns(deadline_ns: int) -> None:
    """Busy-wait until perf_counter_ns reaches deadline_ns.

    Used only by the pacer and the slow sink, where the delay must actually be
    the delay requested. Holds the GIL for its duration, which is precisely why
    it must never appear on the consumer's idle path.
    """
    while now_ns() < deadline_ns:
        pass


def gil_enabled() -> bool:
    """True on a standard build, False on a free-threaded (PEP 703) one.

    Every atomicity argument in this package is conditional on this being True.
    On a free-threaded build the flag list in active_set.py and the index stores
    in spsc_ring.py would need real atomics, which Python does not offer at all.
    """
    return getattr(sys, "_is_gil_enabled", lambda: True)()
