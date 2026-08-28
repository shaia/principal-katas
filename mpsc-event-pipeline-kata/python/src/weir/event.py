"""The unit of transfer.

Byte-identical to the C++ struct and the Go struct: 32 bytes, little-endian,
fields at offsets 0/4/8/16. A batch produced by any of the three answers decodes
in the other two, which is a property no single-language version of this kata can
have and which is worth a test.
"""

from __future__ import annotations

import struct

# <  little-endian, no padding
# I  producer_id : uint32   offset 0
# I  seq         : uint32   offset 4
# q  stamp_ns    : int64    offset 8   *intended* send time
# 16s payload    : 16 bytes offset 16
EVENT_STRUCT = struct.Struct("<IIq16s")
EVENT_SIZE = EVENT_STRUCT.size

# The C++ writes `static_assert(sizeof(Event) == 32)`. This is the Python
# spelling: it fails at import rather than at first use.
assert EVENT_SIZE == 32, f"Event must be 32 bytes, got {EVENT_SIZE}"

pack_event_into = EVENT_STRUCT.pack_into
unpack_event_from = EVENT_STRUCT.unpack_from

# iter_unpack over a bytes object is how the benchmark verifies a run *after* it
# finishes. See sink.py for why verification cannot happen on the hot path.
iter_events = EVENT_STRUCT.iter_unpack

EMPTY_PAYLOAD = b"\x00" * 16
