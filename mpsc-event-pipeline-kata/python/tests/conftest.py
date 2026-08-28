from __future__ import annotations

import sys

import pytest

from weir.platform import SWITCH_INTERVAL


@pytest.fixture(scope="session", autouse=True)
def _switch_interval():
    """Pin the GIL switch interval for the whole session.

    Without this the correctness tests drop ~97% of their events, and while the
    accounting assertions would still be *true*, they would stop exercising the
    steady-state path they exist to cover. See platform.py for the measurements.
    """
    old = sys.getswitchinterval()
    sys.setswitchinterval(SWITCH_INTERVAL)
    yield
    sys.setswitchinterval(old)


@pytest.fixture(params=["bytes", "deque"])
def ring_factory(request):
    """Both backends, so every invariant is checked against both.

    This is the matrix the C++ main.cpp hand-unrolls, expressed as a fixture.
    """
    from weir.spsc_ring import BytesRing, DequeRing

    return BytesRing if request.param == "bytes" else DequeRing
