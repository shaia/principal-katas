package turnstile

import (
	"math/bits"
	"sync/atomic"
	"time"
)

// waiter is how one side of the ring sleeps: consumers on empty, producers on
// full.
//
// Go has no wait-on-address, so a sleep is a channel receive, and a channel
// operation takes the channel's lock: a runtime mutex whose waiters, once it is
// contended, sleep in the OS. So the design question is which channels a push
// and a sleep touch, and how many goroutines share each one's lock.
//
// By default every sleeper has a one-token channel of its own, and a bitmap
// says who is asleep. A producer with nobody asleep reads the bitmap and is
// done — one atomic load of a line nobody writes while the ring is busy. A
// producer that finds a sleeper clears that sleeper's bit, which claims the
// wake, and sends the token on that sleeper's channel; the token waits there if
// the sleeper has not reached its receive yet. Each channel's lock is shared by
// one sleeper and whichever producer claimed it, and a sleep selects only on its
// own token, its own timer and its caller's context.
//
// Variant.SharedToken is the obvious design instead: one token channel for
// every sleeper, a count in front of it, and a closed channel for the
// broadcast. It is also correct, and phase 3 measures what its two shared locks
// cost once sixteen consumers sleep and wake on them.
//
// Neither can lose a wakeup. The sleeper announces itself and then re-checks
// the ring; the waker publishes and then reads the announcements. Every
// sync/atomic operation is sequentially consistent, so those four accesses fall
// into one total order, and in any such order either the re-check sees the
// publication or the waker sees the sleeper: the store-buffering outcome is not
// one the Go memory model allows. Nothing here can be annotated wrongly — but
// the statements can still be put in the wrong order, and phase 3 runs that bug.
type waiter struct {
	_       [64]byte
	asleep  atomic.Uint64 // per sleeper: bit i is set while sleeper i is announced
	count   atomic.Int32  // shared: how many are announced
	_       [64]byte
	shared  bool
	n       atomic.Int32 // sleeper indices handed out
	closing atomic.Bool
	tokens  [maxSleepers]chan struct{} // per sleeper: sleeper i's token
	token   chan struct{}              // shared: the one token
	closed  chan struct{}              // shared: closed by close, the broadcast
}

// maxSleepers bounds the handles on one side of one ring: one bit each in the
// bitmap. The brief has 32 producers and 16 consumers.
const maxSleepers = 64

func (w *waiter) init(shared bool) {
	w.shared = shared
	if shared {
		w.token = make(chan struct{}, 1)
		w.closed = make(chan struct{})
	}
}

// sleeper is one handle's way of sleeping on one waiter: its index there, and
// the timer it reuses for every park, so that sleeping allocates once per
// handle rather than once per park.
type sleeper struct {
	w     *waiter
	slot  int
	timer *time.Timer
}

func (w *waiter) sleeper(side string) sleeper {
	if w.shared {
		return sleeper{w: w}
	}
	i := int(w.n.Add(1) - 1)
	if i >= maxSleepers {
		panic("turnstile: more than 64 " + side + " on one ring")
	}
	// Written before this sleeper's bit is ever set, and read only by a waker
	// that has seen the bit, so the bitmap's atomics order it.
	w.tokens[i] = make(chan struct{}, 1)
	return sleeper{w: w, slot: i}
}

// prepare announces the sleeper. Re-check the condition after it, not before.
func (s *sleeper) prepare() {
	if s.w.shared {
		s.w.count.Add(1)
		return
	}
	s.w.asleep.Or(1 << s.slot)
}

// cancel withdraws the announcement when the re-check found work, or the
// caller gives up. If a notify claimed this sleeper in the meantime, that wake
// was meant for a goroutine that is no longer going to sleep: cancel drops the
// token and passes the wake to one that is. Without that, a producer blocked on
// full — woken only once per batch of releases — could sleep through free slots
// until its backstop.
func (s *sleeper) cancel() {
	w := s.w
	if w.shared {
		w.count.Add(-1) // a token sent meanwhile stays in the channel for the next sleeper
		return
	}
	bit := uint64(1) << s.slot
	if w.asleep.And(^bit)&bit == 0 {
		select {
		case <-w.tokens[s.slot]:
		default: // the claimer has not sent it yet; it will make one spurious wake
		}
		w.notify()
	}
}

// wait sleeps until a wake, the ring's close, done, or d. It reports whether a
// wake or the close ended it; the caller checks its context for the rest.
func (s *sleeper) wait(d time.Duration, done <-chan struct{}) bool {
	w := s.w
	var token, closed <-chan struct{}
	if w.shared {
		token, closed = w.token, w.closed
	} else {
		token = w.tokens[s.slot]
	}
	woken := s.sleep(d, token, closed, done)
	if w.shared {
		w.count.Add(-1)
	} else {
		// A timer or a context that fired first leaves the bit set; clear it. A
		// waker that claims it in the meantime leaves a token in the channel, and
		// the next sleep returns at once and re-checks: a spurious wake, never a
		// lost one.
		w.asleep.And(^(uint64(1) << s.slot))
	}
	return woken
}

// sleep blocks until token or closed is ready, or done, or d passes.
//
// A select with several cases ready picks one at random, so a sleeper that
// reaches its select late — descheduled after arming the timer — can find both
// its token and the expired timer waiting and take the timer. The token is then
// taken too: the wake did arrive, and left in the channel it would only end the
// next sleep early for nothing.
func (s *sleeper) sleep(d time.Duration, token, closed, done <-chan struct{}) bool {
	if s.timer == nil {
		s.timer = time.NewTimer(d)
	} else {
		s.timer.Reset(d)
	}
	select {
	case <-token:
	case <-closed:
	case <-done:
		s.timer.Stop()
		return false
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

// notify wakes one sleeper if there is one, and reports whether it did.
func (w *waiter) notify() bool {
	if w.shared {
		if w.count.Load() == 0 {
			return false
		}
		select {
		case w.token <- struct{}{}: // full means a wake is already in flight
			return true
		default:
			return false
		}
	}
	for {
		m := w.asleep.Load()
		if m == 0 {
			return false
		}
		i := bits.TrailingZeros64(m)
		bit := uint64(1) << i
		if w.asleep.And(^bit)&bit != 0 { // cleared it ourselves: the wake is ours to send
			select {
			case w.tokens[i] <- struct{}{}:
			default: // a stale token is already there; it will do
			}
			return true
		}
	}
}

// close wakes every sleeper. Shared, the closed channel wakes later ones too;
// per sleeper, a goroutine announcing itself after close must find the ring
// closed in its re-check, which every sleep in this package does.
func (w *waiter) close() {
	if !w.closing.CompareAndSwap(false, true) {
		return
	}
	if w.shared {
		close(w.closed)
		return
	}
	for m := w.asleep.Swap(0); m != 0; m &= m - 1 {
		select {
		case w.tokens[bits.TrailingZeros64(m)] <- struct{}{}:
		default:
		}
	}
}

func (w *waiter) sleepers() int {
	if w.shared {
		return int(w.count.Load())
	}
	return bits.OnesCount64(w.asleep.Load())
}
