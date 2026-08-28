"""Policies, sizing constants, and the readouts."""

from __future__ import annotations

import enum
from dataclasses import dataclass, field


class FullPolicy(enum.Enum):
    """What a producer does when its ring is full.

    Both terminate. "Producers must not block indefinitely" is a hard
    requirement, so the only question is how long we try, never whether we
    eventually give up.

    Overwrite-oldest is deliberately absent, and in Python that omission needs
    stating loudly rather than quietly: ``collections.deque(maxlen=N)``
    implements exactly that policy, silently, discarding the *oldest* element on
    overflow and returning nothing to say it did. In C++ overwrite-oldest is
    rejected because it cannot be done safely in a plain SPSC ring. In Python it
    is safe and it is the default, which makes it a trap rather than a
    temptation — you have to know not to reach for it.
    """

    DROP_NEWEST = "drop-newest"
    SPIN_THEN_DROP = "spin-then-drop"


class ScanPolicy(enum.Enum):
    """How the consumer finds rings with work in them."""

    FULL_SCAN = "full"
    BITMAP = "bitmap"


class StopMode(enum.Enum):
    DRAIN = "drain"
    ABORT = "abort"


MAX_PRODUCERS = 64
RING_CAPACITY = 4096
RING_MASK = RING_CAPACITY - 1
DRAIN_BATCH = 512
STAGE_EVENTS = 2048


@dataclass
class PipelineStats:
    """Everything the pipeline exposes about itself.

    Each mechanism here can fail silently, so each one is counted. That is not
    instrumentation for its own sake: the C++ version found its worst design
    flaw — eager bitmap clearing producing 582,954 writes to the shared line
    where the fix produces 8 — purely because this counter existed. No test
    failed. The mechanism was quietly doing the opposite of its job.
    """

    pushed: int = 0
    dropped: int = 0
    consumed: int = 0
    rings_probed: int = 0
    rings_empty: int = 0
    passes: int = 0
    flag_writes: int = 0
    notifies: int = 0
    parks: int = 0
    high_water: int = 0


@dataclass
class PipelineConfig:
    full: FullPolicy = FullPolicy.DROP_NEWEST
    scan: ScanPolicy = ScanPolicy.BITMAP

    #: Idle passes on ``time.sleep(0)`` before descending to the poll rung.
    #: There is no busy-spin rung: it would hold the GIL and starve producers.
    yields_before_poll: int = 200

    #: Poll rounds at ~560 us before parking on the condition variable.
    #:
    #: This rung has no C++ counterpart and exists because of a measured
    #: pathology: ``threading.Condition.wait(timeout)`` on Windows sleeps
    #: ~15.5 ms *regardless of the timeout requested* — 200 us, 1 ms and 5 ms all
    #: measured 15.45-15.50 ms, the scheduler tick. ``threading`` timed waits go
    #: through ``WaitForSingleObjectEx`` and never got the high-resolution timer
    #: ``time.sleep`` gained in 3.11. So the C++ design's 200 us missed-wakeup
    #: backstop would become a 15.5 ms backstop, off by 77x. This rung keeps a
    #: missed wakeup costing ~560 us instead.
    polls_before_park: int = 20

    #: SPIN_THEN_DROP budget, in ``time.sleep(0)`` yields rather than pauses.
    push_spin: int = 4

    #: The deep-sleep backstop. See above: on Windows this is effectively
    #: ~15.5 ms whatever is asked for.
    park_timeout_s: float = 0.0002

    stage_events: int = field(default=STAGE_EVENTS)
