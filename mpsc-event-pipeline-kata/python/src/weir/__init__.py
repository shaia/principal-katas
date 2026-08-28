"""kata — the Python answer to mpsc-event-pipeline-kata.

See ../solution.md. Importing this package sets sys.setswitchinterval; see
platform.py for why that is not a side effect to be squeamish about here.
"""

from .config import (DRAIN_BATCH, MAX_PRODUCERS, RING_CAPACITY, STAGE_EVENTS,
                     FullPolicy, PipelineConfig, PipelineStats, ScanPolicy,
                     StopMode)
from .event import EVENT_SIZE, EVENT_STRUCT, iter_events
from .aio import AsyncEventPipeline
from .event_pipeline import EventPipeline, ProducerHandle
from .sink import CapturingSink, CountingSink, SlowSink
from .spsc_ring import BytesRing, DequeRing

__all__ = [
    "AsyncEventPipeline", "BytesRing", "CapturingSink", "CountingSink", "DRAIN_BATCH", "DequeRing",
    "EVENT_SIZE", "EVENT_STRUCT", "EventPipeline", "FullPolicy",
    "MAX_PRODUCERS", "PipelineConfig", "PipelineStats", "ProducerHandle",
    "RING_CAPACITY", "STAGE_EVENTS", "ScanPolicy", "SlowSink", "StopMode",
    "iter_events",
]
