package turnstile

import (
	"math/bits"
	"sync/atomic"
	"time"
)

// Waiter is how one side of the queue sleeps.
//
// Go has no wait-on-address, so a sleep is a channel receive, and a channel
// operation takes the channel's lock: a runtime mutex whose waiters, once it is
// contended, sleep in the OS. So the design question is which channels a push
// and a sleep touch, and how many goroutines share each one's lock.
//
// The default gives every sleeper a one-token channel of its own, and keeps a
// bitmap of who is asleep. A producer with nobody asleep reads the bitmap and
// is done — one atomic load of a line nobody writes while the queue is busy. A
// producer that finds a sleeper clears that sleeper's bit, which claims the
// wake, and sends the token on that sleeper's channel; the token waits there if
// the sleeper has not reached its receive yet. Each channel's lock is shared by
// one sleeper and whichever producer claimed it, and a sleep selects only on its
// own token and its own timer.
//
// The alternative, Shared, is the obvious design: one token channel for every
// sleeper, a sleeper count in front of it, and a closed channel for the
// broadcast. It is also correct, and phase 3 measures what its two shared locks
// cost once sixteen consumers sleep and wake on them.
//
// Neither can lose a wakeup. The sleeper announces itself and then re-checks
// the queue; the waker publishes and then reads the announcements. Every
// sync/atomic operation is sequentially consistent, so those four accesses fall
// into one total order, and in any such order either the re-check sees the
// publication or the waker sees the sleeper: the store-buffering outcome is not
// one the Go memory model allows. Nothing here can be annotated wrongly — but
// the statements can still be put in the wrong order, and phase 3 runs that bug.
type Waiter struct {
	_       [64]byte
	asleep  atomic.Uint64 // per sleeper: bit i is set while sleeper i is announced
	count   atomic.Int32  // shared: how many are announced
	_       [64]byte
	shared  bool
	n       atomic.Int32 // sleeper indices handed out
	closing atomic.Bool
	tokens  [MaxSleepers]chan struct{} // per sleeper: sleeper i's token
	token   chan struct{}              // shared: the one token
	closed  chan struct{}              // shared: closed by Close, the broadcast
}

// MaxSleepers bounds the goroutines that can sleep on one side of one queue:
// one bit each in the bitmap. The brief has 16 consumers and 32 producers.
const MaxSleepers = 64

// NewWaiter is a Waiter on its own, for measuring the protocol apart from a
// queue.
func NewWaiter(shared bool) *Waiter {
	w := new(Waiter)
	w.init(shared)
	return w
}

func (w *Waiter) init(shared bool) {
	w.shared = shared
	if shared {
		w.token = make(chan struct{}, 1)
		w.closed = make(chan struct{})
	}
}

// slot is the calling goroutine's index, assigned the first time it sleeps here
// and kept in its OpStats. An OpStats belongs to one goroutine, which sleeps on
// one Waiter.
func (w *Waiter) slot(st *OpStats) int {
	if st.waiter != w {
		i := int(w.n.Add(1) - 1)
		if i >= MaxSleepers {
			panic("turnstile: more than 64 goroutines sleeping on one side of a queue")
		}
		// Written before the goroutine's bit is ever set, and read only by a
		// waker that has seen the bit, so the bitmap's atomics order it.
		w.tokens[i] = make(chan struct{}, 1)
		st.waiter, st.slot = w, i
	}
	return st.slot
}

// Prepare announces a sleeper. Re-check the condition after it, not before.
func (w *Waiter) Prepare(st *OpStats) {
	if w.shared {
		w.count.Add(1)
		return
	}
	w.asleep.Or(1 << w.slot(st))
}

// Cancel withdraws the announcement when the re-check found work, or the
// caller gives up. If a Notify claimed this sleeper in the meantime, that wake
// was meant for a goroutine that is no longer going to sleep: Cancel drops the
// token and passes the wake to one that is. Without that, a producer blocked
// on full — woken only once per batch of releases — could sleep through free
// slots until its backstop.
func (w *Waiter) Cancel(st *OpStats) {
	if w.shared {
		w.count.Add(-1) // a token sent meanwhile stays in the channel for the next sleeper
		return
	}
	bit := uint64(1) << w.own(st)
	if w.asleep.And(^bit)&bit == 0 {
		select {
		case <-w.tokens[st.slot]:
		default: // the claimer has not sent it yet; it will make one spurious wake
		}
		w.Notify()
	}
}

// own is the caller's slot, which Prepare assigned. An OpStats that did not
// Prepare here is another goroutine's or another Waiter's, and sharing a slot
// would let one goroutine's Cancel erase another's announcement.
func (w *Waiter) own(st *OpStats) int {
	if st.waiter != w {
		panic("turnstile: Cancel or Wait from an OpStats that did not Prepare on this Waiter")
	}
	return st.slot
}

// Wait sleeps until a wake, Close, or d; it reports whether it was woken.
func (w *Waiter) Wait(d time.Duration, st *OpStats) bool {
	if w.shared {
		woken := st.sleep(d, w.token, w.closed)
		w.count.Add(-1)
		return woken
	}
	woken := st.sleep(d, w.tokens[w.own(st)], nil)
	// A timer that fired first leaves the bit set; clear it. A waker that claims
	// it in the meantime leaves a token in the channel, and the next sleep
	// returns at once and re-checks: a spurious wake, never a lost one.
	w.asleep.And(^(uint64(1) << st.slot))
	return woken
}

// Notify wakes one sleeper if there is one, and reports whether it did.
func (w *Waiter) Notify() bool {
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
		bit := uint64(1) << bits.TrailingZeros64(m)
		if w.asleep.And(^bit)&bit != 0 { // cleared it ourselves: the wake is ours to send
			select {
			case w.tokens[bits.TrailingZeros64(bit)] <- struct{}{}:
			default: // a stale token is already there; it will do
			}
			return true
		}
	}
}

// Close wakes every sleeper. Shared, the closed channel wakes later ones too;
// per sleeper, a goroutine announcing itself after Close must find the queue
// closed in its re-check, which the Ring's own sleeps all do.
func (w *Waiter) Close() {
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

func (w *Waiter) Sleepers() int {
	if w.shared {
		return int(w.count.Load())
	}
	return bits.OnesCount64(w.asleep.Load())
}
