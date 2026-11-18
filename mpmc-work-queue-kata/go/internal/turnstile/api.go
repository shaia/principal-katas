// Package turnstile is a bounded multi-producer, multi-consumer queue: the
// main kata's per-slot sequenced ring, in Go.
//
// A Ring is shared; the goroutines using it each take a handle. A producing
// goroutine calls Producer once and pushes through it, a consuming goroutine
// calls Consumer once and pops through it, and anyone may Close the ring.
// Handles carry what a goroutine needs to sleep — its timer, its place among
// the sleepers — and its statistics, so none of that is passed on each call.
package turnstile

import "errors"

var (
	// ErrFull is TryPush's refusal: the slot at the tail still holds an event
	// from the previous lap.
	ErrFull = errors.New("turnstile: full")

	// ErrClosed is returned by pushes once the ring has closed, and by pops once
	// it has closed and every event accepted before the close has been taken.
	ErrClosed = errors.New("turnstile: closed")
)

// Stats counts what a handle's operations cost beyond the fast path.
type Stats struct {
	Retries   uint64 // a lost compare-and-swap, or a stale index read again
	Parks     uint64
	Wakes     uint64 // wake tokens handed to a sleeper
	Holes     uint64 // parks while a position below the tail was reserved and unpublished
	Backstops uint64 // parks ended by the backstop timer rather than a wake
}

// Add accumulates o into s.
func (s *Stats) Add(o Stats) {
	s.Retries += o.Retries
	s.Parks += o.Parks
	s.Wakes += o.Wakes
	s.Holes += o.Holes
	s.Backstops += o.Backstops
}
