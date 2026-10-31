package turnstile

import "time"

// Variant describes a deliberately different ring: the arms the benchmark
// measures New's against, and the seams its experiments need. Code that wants
// a queue calls New; a Variant exists to be compared with it or to break it.
//
// A ring built by New carries a nil *Variant, and each place a variant could
// differ checks that pointer first, so New's ring pays one predictable branch
// per push and per pop for the existence of this type.
type Variant struct {
	// SharedToken: consumers share one token channel and a count of sleepers,
	// instead of a channel each and a bitmap.
	SharedToken bool
	// NoSpin: a consumer that finds the ring empty parks at once, rather than
	// spinning first in case a producer is mid-window.
	NoSpin bool
	// Spin: consumers never park. With Yield they call runtime.Gosched between
	// spins.
	Spin, Yield bool
	// Retain: a consumed value stays in its slot rather than being zeroed.
	Retain bool
	// Backstop bounds every park; 50 ms when zero.
	Backstop time.Duration
	Hooks    Hooks
	Bug      Bug
}

// Hooks are called where the ring has no reason to wait, so a hook that stalls
// reproduces a goroutine descheduled exactly there.
type Hooks struct {
	Reserved func(pos uint64) // producer: after reserving, before publishing
	Parking  func()           // consumer: about to announce itself as a sleeper
}

// Bug selects one line of the protocol made wrong, for the bug × tool table.
type Bug uint8

const (
	BugNone         Bug = iota
	BugPublishFirst     // the publication store before the payload write
	BugNoFullCheck      // reserve a slot that still holds the previous lap
	BugLateAnnounce     // a consumer re-checks, then announces itself as a sleeper
)
