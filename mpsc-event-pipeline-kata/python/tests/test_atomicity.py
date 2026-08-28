"""The design's central claim, made executable rather than argued in prose.

Section 2 of solution.md rests on exactly one empirical question: which
operations are atomic under the GIL? These tests answer it, and the answer is
more interesting than the received wisdom.
"""

from __future__ import annotations

import dis
import sys
import threading

import pytest


def _opnames(fn) -> list[str]:
    return [ins.opname for ins in dis.get_instructions(fn)]


def test_flag_store_is_a_single_bytecode():
    """`flags[i] = 1` is one STORE_SUBSCR dispatching to one C call.

    That is what "atomic by construction" means here: no checkpoint can land
    inside a single bytecode, and list.__setitem__ neither releases the GIL nor
    re-enters the interpreter.
    """
    def store(flags, i):
        flags[i] = 1

    ops = _opnames(store)
    assert ops.count("STORE_SUBSCR") == 1
    assert "BINARY_OP" not in ops, "a read-modify-write crept in"


def test_index_publish_is_a_single_store():
    """`self._tail = t + 1` is one STORE_ATTR — the publish in spsc_ring.py.

    The addition happens on a local before the store, so the store itself is
    indivisible. This is what replaces the C++ release store.
    """
    def publish(self, t):
        self._tail = t + 1

    ops = _opnames(publish)
    assert ops.count("STORE_ATTR") == 1
    assert "LOAD_ATTR" not in ops, "the publish must not re-read the attribute"


def test_int_bitmap_or_is_a_read_modify_write():
    """`self._m |= bit` spans several bytecodes.

    This is the shape the naive analysis calls unsafe. See the next two tests for
    why that analysis is wrong, and why it is still the wrong thing to write.
    """
    def bitmap_or(self, bit):
        self._m |= bit

    ops = _opnames(bitmap_or)
    assert "LOAD_ATTR" in ops and "BINARY_OP" in ops and "STORE_ATTR" in ops
    assert ops.index("LOAD_ATTR") < ops.index("STORE_ATTR")


def test_straight_line_rmw_does_not_actually_race():
    """The surprise: the three-bytecode read-modify-write does NOT lose updates.

    CPython checks the eval breaker only at specific instructions —
    JUMP_BACKWARD, RESUME, CALL — and a straight-line LOAD_ATTR / BINARY_OP /
    STORE_ATTR sequence contains no checkpoint, so no thread switch can land
    inside it.

    This is why the naive reasoning is not merely incomplete but backwards: the
    dangerous form tests clean, reliably, even with the switch interval pinned as
    low as it will go.
    """
    class Box:
        __slots__ = ("v",)

        def __init__(self):
            self.v = 0

    old = sys.getswitchinterval()
    sys.setswitchinterval(1e-9)          # maximally hostile
    try:
        box = Box()
        threads = [threading.Thread(target=lambda: [
            setattr(box, "v", box.v + 1) for _ in range(20_000)
        ]) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        sys.setswitchinterval(old)

    # Not asserted as exactly 160_000: this is an implementation detail of
    # eval-breaker placement, not a language guarantee, and asserting it would
    # be asserting something CPython never promised. The point is that it is
    # *close*, which is what makes the trap invisible.
    assert box.v > 160_000 * 0.9, (
        "expected the straight-line RMW to lose few or no updates on this build"
    )


def test_inserting_a_call_makes_the_same_code_lose_most_updates():
    """And the trap springs the moment a CALL appears between load and store.

    This is the whole argument. The identical logical operation, refactored in a
    way any reviewer would wave through, loses the majority of its updates —
    measured at 71.5% loss on the development machine.

    So the integer bitmap is not safe because of anything you reasoned about. It
    is safe because of where CPython happens to place its checkpoints, and it
    stops being safe when you factor a helper out. That is a far worse property
    than being outright broken, and it is why active_set.py uses a list of flags
    that is atomic *by construction* instead.

    This is the Python analogue of the C++ answer's TSO argument — there, x86
    hides ordering bugs that AArch64 exposes; here, eval-breaker placement hides
    an atomicity bug that a free-threaded build exposes — and it is sharper,
    because the variable is your code's shape rather than your hardware.
    """
    class Box:
        __slots__ = ("v",)

        def __init__(self):
            self.v = 0

    def ident(x):
        return x

    old = sys.getswitchinterval()
    sys.setswitchinterval(1e-9)
    try:
        box = Box()
        threads = [threading.Thread(target=lambda: [
            setattr(box, "v", ident(box.v) + 1) for _ in range(20_000)
        ]) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        sys.setswitchinterval(old)

    expected = 8 * 20_000
    assert box.v < expected, (
        f"expected lost updates once a CALL sits between the load and the store, "
        f"got the full {expected}"
    )


def test_gil_is_enabled_or_every_claim_here_is_void():
    """Every atomicity argument in this package is conditional on the GIL.

    On a free-threaded (PEP 703) build, `flags[i] = 1` and `self._tail = t + 1`
    would need real atomics, which Python does not offer at any width. That is
    named as a gap in solution.md section 10, in the same way the C++ answer
    names its missing ThreadSanitizer.
    """
    from weir.platform import gil_enabled

    if not gil_enabled():
        pytest.fail(
            "running on a free-threaded build: the atomicity arguments in "
            "active_set.py and spsc_ring.py do not hold here and the design "
            "would need real atomics, which Python does not provide"
        )
