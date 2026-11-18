package turnstile

import "unsafe"

// Event is what crosses the queue: 32 bytes, with no pointers in it, carrying
// enough to check every delivery the moment it arrives.
type Event struct {
	Producer uint32
	Seq      uint32 // per producer: exactly-once, and per-producer order per consumer
	Stamp    uint64 // counter reading when the producer began the push: end-to-end latency
	Payload  uint64
	Check    uint64 // digest of the other four fields
}

// LineEvent is an Event padded so that its slot in a Ring — the 8-byte
// sequence, then the value — is one 64-byte cache line exactly, and adjacent
// slots, written by different producers and read by different consumers, never
// share a line. Ring[Event] packs them instead; the benchmark runs both.
type LineEvent struct {
	Event
	_ [24]byte
}

// The build fails here, not a benchmark, if a LineEvent slot stops being one
// line.
var (
	_ [64 - unsafe.Sizeof(slot[LineEvent]{})]byte
	_ [unsafe.Sizeof(slot[LineEvent]{}) - 64]byte
)

// mix is splitmix64's finalizer: cheap, and every input bit reaches every
// output bit.
func mix(x uint64) uint64 {
	x ^= x >> 30
	x *= 0xbf58476d1ce4e5b9
	x ^= x >> 27
	x *= 0x94d049bb133111eb
	x ^= x >> 31
	return x
}

func digest(producer, seq uint32, stamp, payload uint64) uint64 {
	return mix(mix(mix((uint64(producer)<<32|uint64(seq))+0x9e3779b97f4a7c15)^stamp) ^ payload)
}

func MakeEvent(producer, seq uint32, stamp uint64) Event {
	payload := mix(uint64(producer)<<32 | uint64(seq))
	return Event{producer, seq, stamp, payload, digest(producer, seq, stamp, payload)}
}

// Intact fails for a slot read before it was written or while it was being
// written, and for the zero Event: the digest of four zeros is not zero.
func Intact(e *Event) bool {
	return e.Payload == mix(uint64(e.Producer)<<32|uint64(e.Seq)) &&
		e.Check == digest(e.Producer, e.Seq, e.Stamp, e.Payload)
}

// Tally checks exactly-once without a record per event. Every accepted event
// adds two independent hashes of its identity to the producer's sums, every
// consumed one to the consumer's. Equal counts and equal sums mean the same
// multiset, except with probability around 2^-128 — and a loss plus a duplicate,
// which leave the count unchanged, still move both sums.
type Tally struct{ N, A, B uint64 }

func (t *Tally) Add(producer, seq uint32) {
	id := uint64(producer)<<32 | uint64(seq)
	t.N++
	t.A += mix(id ^ 0x2545f4914f6cdd1d)
	t.B += mix(id*0x9e3779b97f4a7c15 + 1)
}

func (t *Tally) Merge(o Tally) { t.N += o.N; t.A += o.A; t.B += o.B }
