package turnstile

import "time"

// Push is how a push ended.
type Push uint8

const (
	PushOK      Push = iota
	PushFull         // reject policy: the slot at the tail still holds the previous lap
	PushTimeout      // block policy: still full at the deadline
	PushClosed       // refused because the queue had closed; nothing was accepted
)

// Pop is how a pop ended.
type Pop uint8

const (
	PopOK      Pop = iota
	PopEmpty       // non-blocking: the head slot holds nothing for this lap — empty, or a hole
	PopDrained     // closed, and every position ever reserved has been claimed
	PopTimeout
)

// OpStats is one goroutine's: what its operations cost beyond the fast path,
// and the timer its parks reuse, so that sleeping allocates once per goroutine
// rather than once per park.
type OpStats struct {
	Retries   uint64 // a lost compare-and-swap, or a stale index read again
	Parks     uint64
	Wakes     uint64 // wake tokens handed to a sleeper
	Holes     uint64 // parks while a position below the tail was reserved and unpublished
	Backstops uint64 // parks ended by the backstop timer rather than a wake

	timer  *time.Timer
	waiter *Waiter // the Waiter this goroutine sleeps on, and its index there
	slot   int
}

func (s *OpStats) Merge(o *OpStats) {
	s.Retries += o.Retries
	s.Parks += o.Parks
	s.Wakes += o.Wakes
	s.Holes += o.Holes
	s.Backstops += o.Backstops
}

// sleep blocks on the first of the three channels to become ready, or for d.
// It reports whether something other than the timer ended it.
//
// A select with several cases ready picks one at random, so a sleeper that
// reaches its select late — descheduled after arming the timer — can find both
// its token and the expired timer waiting and take the timer. The token is
// then taken too: the wake did arrive, and left in the channel it would only
// end the next sleep early for nothing.
func (s *OpStats) sleep(d time.Duration, token, closed <-chan struct{}) bool {
	if s.timer == nil {
		s.timer = time.NewTimer(d)
	} else {
		s.timer.Reset(d)
	}
	select {
	case <-token:
	case <-closed:
	case <-s.timer.C:
		select {
		case <-token:
			return true
		case <-closed:
			return true
		default:
			return false
		}
	}
	s.timer.Stop()
	return true
}
