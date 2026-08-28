package weir

import "time"

// FullPolicy is what a producer does when its ring is full.
//
// Both terminate. "Producers must not block indefinitely" is a hard
// requirement, so the only question is how long we try, never whether we
// eventually give up.
//
// Overwrite-oldest is deliberately absent: it cannot be done safely in a plain
// SPSC ring, because the producer would overwrite a slot the consumer may be
// mid-read. It needs sequence-numbered slots and reader validation, which is a
// different data structure. Note for the Python answer, where this is a live
// trap: collections.deque(maxlen=N) implements exactly this policy silently.
type FullPolicy uint8

const (
	DropNewest   FullPolicy = iota // fail immediately, count it
	SpinThenDrop                   // bounded spin first, for absorbable bursts
)

func (f FullPolicy) String() string {
	if f == SpinThenDrop {
		return "spin-then-drop"
	}
	return "drop-newest"
}

// ScanPolicy is how the consumer finds rings with work in them. FullScan is the
// naive O(N); Bitmap is the follow-up's answer. Both are kept so the benchmark
// can A/B them in one binary with everything else held identical.
type ScanPolicy uint8

const (
	FullScan ScanPolicy = iota
	Bitmap
)

func (s ScanPolicy) String() string {
	if s == Bitmap {
		return "bitmap"
	}
	return "full"
}

// StopMode: Drain is lossless but bounded by a deadline; Abort discards and
// returns fast.
type StopMode uint8

const (
	Drain StopMode = iota
	Abort
)

func (m StopMode) String() string {
	if m == Abort {
		return "abort"
	}
	return "drain"
}

const (
	CacheLine    = 64
	MaxProducers = 64   // one bitmap word
	RingCapacity = 4096 // per producer; bounds memory at N x this
	ringMask     = RingCapacity - 1
	DrainBatch   = 512  // per ring, per pass — the fairness cap
	StageEvents  = 2048 // staging buffer, in events
)

// Stats is everything the pipeline exposes about itself.
//
// Each mechanism in the design can fail silently, so each one is counted. This
// is not instrumentation for its own sake: the C++ version found its worst
// design flaw — eager bitmap clearing producing 582,954 writes to the shared
// line where the fix produces 8 — purely because BitmapWrites existed. No test
// failed. The mechanism was quietly doing the opposite of its job.
type Stats struct {
	Pushed, Dropped, Consumed       uint64
	RingsProbed, RingsEmpty, Passes uint64
	BitmapWrites, Notifies, Parks   uint64
	HighWater                       uint64
	GCCycles                        uint64 // GC cycles during the measured window
}

// Config is the tuning surface.
//
// The idle ladder has two budgets where the C++ has one, because
// runtime.Gosched() is not the equivalent of std::this_thread::yield(). It is a
// *goroutine* yield onto the run queue — no syscall, tens of nanoseconds — so
// it is far cheaper than the rung it replaces, and the spin budget in front of
// it should be correspondingly smaller.
type Config struct {
	Full            FullPolicy
	Scan            ScanPolicy
	SpinBeforeYield uint32        // consumer idle spins before the first Gosched
	YieldBeforePark uint32        // Gosched rounds before parking
	PushSpin        uint32        // SpinThenDrop budget
	ParkTimeout     time.Duration // missed-wakeup backstop
}

func DefaultConfig() Config {
	return Config{
		Full:            DropNewest,
		Scan:            Bitmap,
		SpinBeforeYield: 2000,
		YieldBeforePark: 2000,
		PushSpin:        200,
		ParkTimeout:     200 * time.Microsecond,
	}
}
