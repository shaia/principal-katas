package weir

import "unsafe"

// Event is the unit of transfer: 32 bytes, two per cache line, stored by value
// inside the ring. A push is a 32-byte copy into preallocated storage — no
// allocation, no indirection, no shared ownership on the hot path.
//
// StampNs is an int64 of nanoseconds, NOT a time.Time, and that is load-bearing
// rather than stylistic. time.Time embeds `loc *Location`. One pointer field
// here would make Event a pointer-carrying type, which would make every
// [4096]Event array a *scannable* allocation, which would put all 8 MiB of ring
// storage on the garbage collector's mark path. Measured on the development box
// at an identical 8.4 MiB heap: pointer-free rings cost 3.63 ms for 20 forced
// GCs with no observable stop-the-world pause; one pointer per event costs
// 9.84 ms and 26.8 us of average STW — which would land directly in the p99.9
// this whole design exists to protect.
//
// The same reasoning forbids string, []byte, error, any, and a map or channel
// field. TestEventIsPointerFree is the guard that survives a future edit by
// someone who has not read this comment.
//
// The layout is byte-identical to the C++ struct and to Python's
// struct.Struct("<IIq16s"), so a batch produced by any of the three answers
// decodes in the other two.
type Event struct {
	ProducerID uint32   // offset  0
	Seq        uint32   // offset  4  per-producer, strictly increasing
	StampNs    int64    // offset  8  *intended* send time — see the load generator
	Payload    [16]byte // offset 16
}

// EventSize is 32. Not a literal: if the struct ever changes, this follows and
// the assertions below fail rather than the wire format silently drifting.
const EventSize = int(unsafe.Sizeof(Event{}))

// Compile-time layout assertions.
//
// Go has no static_assert. A zero-length array whose length is a constant
// expression is the substitute, and it needs both directions: the first of each
// pair fails if the value is too small (the uintptr subtraction underflows to an
// astronomically large array length), the second fails if it is too large (a
// negative array length). Together they pin the value exactly.
//
// Pinning the offsets as well as the size also pins that there is no internal
// padding (4+4+8+16 = 32), which is what makes the reinterpretation in
// EventsAsBytes fully defined rather than partly reading uninitialised bytes.
var (
	_ [unsafe.Sizeof(Event{}) - 32]byte
	_ [32 - unsafe.Sizeof(Event{})]byte
	_ [unsafe.Offsetof(Event{}.Seq) - 4]byte
	_ [4 - unsafe.Offsetof(Event{}.Seq)]byte
	_ [unsafe.Offsetof(Event{}.StampNs) - 8]byte
	_ [8 - unsafe.Offsetof(Event{}.StampNs)]byte
	_ [unsafe.Offsetof(Event{}.Payload) - 16]byte
	_ [16 - unsafe.Offsetof(Event{}.Payload)]byte
)

// EventsAsBytes reinterprets a slice of events as the bytes behind them, with no
// copy, so the consumer can hand a staged batch to a Sink the way the C++ hands
// over a std::span.
//
// This is one of only two uses of unsafe in the package (the other is the TSC
// clock), and it is defensible for three specific reasons, each asserted
// elsewhere rather than assumed here:
//
//  1. Event is pointer-free, so the reinterpretation cannot hide a pointer from
//     the garbage collector — see TestEventIsPointerFree.
//  2. Event has no internal padding, so every byte of the result is a byte some
//     field wrote — see the offset assertions above.
//  3. The result aliases ev and is consumed synchronously. The Sink contract
//     says an implementation must not retain the slice past the call.
//
// The alternative, an encoding/binary loop, would add ~32 bytes of encoding work
// per event and change what the throughput number measures — which is exactly
// the mistake the C++ writeup already caught once, when a per-byte FNV hash in
// the benchmark sink made every "throughput" figure a measurement of the
// checksum.
func EventsAsBytes(ev []Event) []byte {
	if len(ev) == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(unsafe.SliceData(ev))), len(ev)*EventSize)
}
