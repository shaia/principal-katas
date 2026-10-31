package turnstile

import (
	"runtime"
	"sync/atomic"
	"time"
	"unsafe"

	"turnstile/internal/tsc"
)

// Ring is the main kata's per-slot sequenced queue, ported from the C++ track's
// turnstile::Queue. For the slot that holds position p = lap·N + i:
//
//	seq == p        free for the producer that reserves p
//	seq == p + 1    published: the consumer that claims p may read it
//	seq == p + N    released: free for the producer of p + N, one lap on
//
// "Reserved but not yet published" is not a value of seq. It is seq == p while
// the tail has already passed p, visible only by reading both.
//
// What the port keeps: the lap-stamped sequence, the four tests on it, the
// closed bit in the tail word, the tombstone. What it drops: every ordering.
// Each sync/atomic operation is sequentially consistent, so the two handoffs
// through a slot — publication to the consumer, release to the next lap's
// producer — need no annotation, and the payload is a plain field: the Go
// memory model makes a plain write sequenced before an atomic store visible to a
// plain read sequenced after an atomic load that observes it. The cost is that
// every publication is an XCHG on amd64 rather than a plain store.
//
// A reservation is a debt, and every path out of a push pays it. A CAS
// reservation cannot happen after a close, because the closed bit lives in the
// word it operates on. A ticket can, because an Add cannot be refused. So Close
// records where the tail stood when the bit went in, consumers drain exactly to
// there, and a ticket that lands past it is refused without touching its slot:
// nobody is owed it. (The C++ track's queue paid such a ticket with a tombstone
// instead; with more tickets in flight than slots, and the consumers gone, the
// one that mapped onto a tombstoned slot waited for a release that never came.)
// The tombstone — seq set to p + 1 with the dead bit, and no value — remains
// for the one debt that is owed and cannot be paid with an event: a
// TryPushFunc whose fill panicked. Consumers claim it, skip it and release it.
type Ring[T, Pad any] struct {
	_                 [64]byte
	tail              atomic.Uint64 // (next position << 1) | closed; written by producers
	_                 [64]byte
	head              atomic.Uint64 // next position to claim; written by consumers
	_                 [64]byte
	closedAt          atomic.Uint64 // 1 + the tail's position when Close set the bit; 0 before
	slots             []slot[T, Pad]
	mask, fullWake    uint64
	opt               Options
	notEmpty, notFull Waiter
}

// slot puts the padding first: a zero-sized field at the end of a struct gets a
// byte of padding of its own, so an unpadded slot would not be the 40 bytes it
// looks like.
type slot[T, Pad any] struct {
	_   Pad
	seq atomic.Uint64
	val T
}

// LinePad pads an Event slot to one cache line.
type LinePad [24]byte

// The padded slot is exactly one line, and the build fails here, not a
// benchmark, if Event grows.
var (
	_ [64 - unsafe.Sizeof(slot[Event, LinePad]{})]byte
	_ [unsafe.Sizeof(slot[Event, LinePad]{}) - 64]byte
)

// Reserve is how a blocking Push claims a position. TryPush always uses CAS;
// the two share one tail word.
type Reserve uint8

const (
	// Look, then claim: compare-and-swap the tail after seeing the slot free. A
	// blocked push can give up at its deadline.
	ReserveCAS Reserve = iota
	// Claim, then look: one Add that never fails, and a position that cannot be
	// handed back. A blocked push waits for its slot however long that takes.
	ReserveTicket
)

// Bug selects one line of the protocol made wrong, for the bug × tool table.
type Bug uint8

const (
	BugNone         Bug = iota
	BugPublishFirst     // the publication store before the payload write
	BugNoFullCheck      // reserve a slot that still holds the previous lap
	BugLateAnnounce     // a consumer re-checks, then announces itself as a sleeper
)

// Hooks are test seams, each called where the real queue has no reason to wait,
// so a hook that stalls reproduces a goroutine descheduled exactly there.
type Hooks struct {
	Reserved func(pos uint64) // producer: after reserving, before publishing
	Claimed  func(pos uint64) // consumer: after taking, before releasing
	Parking  func()           // consumer: about to announce itself as a sleeper
}

type Options struct {
	Reserve Reserve
	// Consumers that never sleep: back to the spin until an event, the deadline
	// or the close. With Yield they call runtime.Gosched between spins.
	Spin, Yield bool
	// Consumers share one token channel and a sleeper count, instead of a
	// token channel each and a bitmap: phase 3's comparison.
	SharedToken bool
	// Leave a consumed value in its slot rather than zeroing it.
	Retain   bool
	Backstop time.Duration // bounds every park; 50 ms when zero
	Hooks    *Hooks
	Bug      Bug
}

const (
	closedBit = 1
	dead      = 1 << 63

	// Spin before parking, in PAUSE instructions; phase 0 prints what one costs.
	spins = 128
	// A ticket holder whose slot is still occupied yields this many times, then
	// sleeps between checks. It is waiting on a full queue, whose pace the
	// consumers already set, and nobody wakes it for its own slot.
	yields     = 64
	ticketPoll = 200 * time.Microsecond
)

func NewRing[T, Pad any](capacity int, opt Options) *Ring[T, Pad] {
	if capacity < 2 || capacity&(capacity-1) != 0 {
		panic("turnstile: capacity must be a power of two, at least 2")
	}
	if opt.Backstop == 0 {
		opt.Backstop = 50 * time.Millisecond
	}
	r := &Ring[T, Pad]{slots: make([]slot[T, Pad], capacity), mask: uint64(capacity - 1), opt: opt}
	// Consumers wake a producer blocked on full once per this many releases:
	// a blocked producer can use any free slot, and waking one per release made
	// every pop pay for a wake.
	r.fullWake = 64
	if capacity < 128 {
		r.fullWake = uint64(capacity / 2)
	}
	// Writing every sequence also faults in every page of the ring here, on the
	// constructing goroutine, so no producer takes a first-touch fault inside its
	// window. No slot's initial sequence is zero, so the design cannot skip it.
	for i := range r.slots {
		r.slots[i].seq.Store(uint64(i))
	}
	r.notEmpty.init(opt.SharedToken)
	r.notFull.init(false)
	return r
}

// ---------------------------------------------------------------- producers

// TryPush is the reject policy.
func (r *Ring[T, Pad]) TryPush(v T, st *OpStats) Push {
	s, pos, res := r.claim(st)
	if res != PushOK {
		return res
	}
	return r.publish(s, pos, v, st)
}

// Push is the block policy: with ReserveCAS until the deadline, with
// ReserveTicket until the slot frees. It returns closed once the queue closes.
func (r *Ring[T, Pad]) Push(v T, until time.Time, st *OpStats) Push {
	if r.opt.Reserve == ReserveTicket {
		// The pre-check spares a closed queue a ticket in the common case. The
		// race it leaves, a close between this load and the Add, is handled in
		// pushTicket.
		if r.tail.Load()&closedBit != 0 {
			return PushClosed
		}
		return r.pushTicket(v, st)
	}
	parked := false
	for {
		res := r.TryPush(v, st)
		if res == PushFull {
			r.notFull.Prepare(st)
			if res = r.TryPush(v, st); res != PushFull {
				r.notFull.Cancel(st)
			}
		}
		if res != PushFull {
			if parked && res == PushOK {
				r.chainFullWake(st)
			}
			return res
		}
		now := time.Now()
		if !now.Before(until) {
			r.notFull.Cancel(st)
			return PushTimeout
		}
		st.Parks++
		parked = true
		r.notFull.Wait(min(until.Sub(now), r.opt.Backstop), st)
	}
}

// pushTicket is a ticket push from the Add on, split out so that a test can
// stand in for producers that passed the pre-check before a close.
func (r *Ring[T, Pad]) pushTicket(v T, st *OpStats) Push {
	word := r.tail.Add(2) - 2
	if word&closedBit != 0 {
		return PushClosed // past the close position: owed to nobody
	}
	pos := word >> 1
	s := &r.slots[pos&r.mask]
	r.awaitTurn(s, pos, st)
	return r.publish(s, pos, v, st)
}

// TryPushFunc reserves a slot and lets fill build the value in place. Whatever
// fill does, the reservation is paid: if it panics, a deferred function
// publishes a tombstone, consumers skip it, and the panic carries on to the
// caller. A defer runs on a panic and on a normal return alike, so — unlike the
// C++ catch block — there is no path on which the debt is forgotten.
func (r *Ring[T, Pad]) TryPushFunc(fill func(*T), st *OpStats) Push {
	s, pos, res := r.claim(st)
	if res != PushOK {
		return res
	}
	published := false
	defer func() {
		if !published {
			var zero T
			s.val = zero
			s.seq.Store((pos + 1) | dead)
			// Events published behind this position became reachable just now,
			// and their consumers may be asleep waiting for this one.
			r.notEmpty.Notify()
		}
	}()
	fill(&s.val)
	s.seq.Store(pos + 1)
	published = true
	if r.notEmpty.Notify() {
		st.Wakes++
	}
	return PushOK
}

// claim reserves the tail position by compare-and-swap if its slot is free.
// The closed bit lives in the word it swaps, so a close and a reservation are
// one decision: the swap fails if the queue closed.
func (r *Ring[T, Pad]) claim(st *OpStats) (*slot[T, Pad], uint64, Push) {
	word := r.tail.Load()
	for {
		if word&closedBit != 0 {
			return nil, 0, PushClosed
		}
		pos := word >> 1
		s := &r.slots[pos&r.mask]
		// Signed, and defined: Go's unsigned subtraction wraps, so the
		// difference is right across a wrap of the 64-bit position.
		dif := int64((s.seq.Load() &^ dead) - pos)
		switch {
		case dif == 0 || (dif < 0 && r.opt.Bug == BugNoFullCheck):
			if r.tail.CompareAndSwap(word, word+2) {
				return s, pos, PushOK
			}
			st.Retries++
		case dif < 0:
			return nil, 0, PushFull // the slot still holds the previous lap
		default:
			st.Retries++ // another producer took pos; the tail read was stale
		}
		word = r.tail.Load()
	}
}

// publish is the window. Everything between the reservation and the store of
// pos + 1 is time during which every consumer behind this position waits.
func (r *Ring[T, Pad]) publish(s *slot[T, Pad], pos uint64, v T, st *OpStats) Push {
	if r.opt.Bug == BugPublishFirst {
		s.seq.Store(pos + 1)
		s.val = v
	} else {
		s.val = v
		if h := r.opt.Hooks; h != nil && h.Reserved != nil {
			h.Reserved(pos)
		}
		s.seq.Store(pos + 1)
	}
	if r.notEmpty.Notify() {
		st.Wakes++
	}
	return PushOK
}

// awaitTurn is a ticket holder waiting for the previous lap to leave its slot:
// spin, then yield, then poll on a timer.
func (r *Ring[T, Pad]) awaitTurn(s *slot[T, Pad], pos uint64, st *OpStats) {
	for i := 0; i < spins; i++ {
		if s.seq.Load() == pos {
			return
		}
		tsc.Pause()
	}
	for i := 0; i < yields; i++ {
		if s.seq.Load() == pos {
			return
		}
		runtime.Gosched()
	}
	for s.seq.Load() != pos {
		st.Parks++
		time.Sleep(ticketPoll)
	}
}

// chainFullWake: a producer that parked on full and has now got in may not be
// the only one waiting. If there is room behind it, it passes the wake on.
func (r *Ring[T, Pad]) chainFullWake(st *OpStats) {
	t := r.tail.Load() >> 1
	if r.slots[t&r.mask].seq.Load()&^dead == t && r.notFull.Notify() {
		st.Wakes++
	}
}

// ---------------------------------------------------------------- consumers

// TryPop never reads the tail: "empty" is decided by the head slot alone.
func (r *Ring[T, Pad]) TryPop(out *T, st *OpStats) Pop {
	pos := r.head.Load()
	for {
		s := &r.slots[pos&r.mask]
		raw := s.seq.Load()
		dif := int64((raw &^ dead) - (pos + 1))
		switch {
		case dif == 0:
			if r.head.CompareAndSwap(pos, pos+1) {
				if raw&dead == 0 {
					r.consume(s, pos, out, st)
					return PopOK
				}
				r.release(s, pos, st) // a tombstone: nothing to read, give the slot back
				pos++
				continue
			}
			st.Retries++
		case dif < 0:
			return PopEmpty // not yet published for this lap: empty, or a hole
		default:
			st.Retries++
		}
		pos = r.head.Load()
	}
}

// Pop spins, then parks, until an event arrives, the deadline passes, or the
// queue is closed and every position ever reserved has been claimed.
func (r *Ring[T, Pad]) Pop(out *T, until time.Time, st *OpStats) Pop {
	for {
		if r.TryPop(out, st) == PopOK {
			return PopOK
		}
		// A producer mid-window publishes within nanoseconds unless it has been
		// descheduled, and a park costs microseconds: spin first.
		for i := 0; i < spins && !r.headPublished(); i++ {
			tsc.Pause()
		}
		if r.TryPop(out, st) == PopOK {
			return PopOK
		}

		// Only now, about to sleep, look at the producers' counter. After a
		// close nobody parks: what remains is owed by producers that already
		// hold their positions, so yield and retry until all are claimed.
		if word := r.tail.Load(); word&closedBit != 0 {
			// Drained at the close position, not at the tail, which refused
			// tickets keep moving. Zero means Close has set the bit and not yet
			// recorded where: not drained yet.
			if end := r.closedAt.Load(); end != 0 && r.head.Load() == end-1 {
				return PopDrained
			}
			if !time.Now().Before(until) {
				return PopTimeout
			}
			runtime.Gosched()
			continue
		}
		if r.opt.Spin {
			if r.opt.Yield {
				runtime.Gosched()
			}
			if !time.Now().Before(until) {
				return PopTimeout
			}
			continue
		}

		if h := r.opt.Hooks; h != nil && h.Parking != nil {
			h.Parking()
		}
		late := r.opt.Bug == BugLateAnnounce
		if !late {
			r.notEmpty.Prepare(st)
		}
		if r.TryPop(out, st) == PopOK { // the re-check
			if !late {
				r.notEmpty.Cancel(st)
			}
			return PopOK
		}
		if late {
			// The bug: a publication between the re-check and this line reads a
			// sleeper count of zero and wakes nobody.
			r.notEmpty.Prepare(st)
		}
		word := r.tail.Load()
		if word&closedBit != 0 {
			r.notEmpty.Cancel(st)
			continue
		}
		if word>>1 > r.head.Load() {
			st.Holes++ // reserved, unpublished: a producer is inside its window
		}
		now := time.Now()
		if !now.Before(until) {
			r.notEmpty.Cancel(st)
			return PopTimeout
		}
		st.Parks++
		if !r.notEmpty.Wait(min(until.Sub(now), r.opt.Backstop), st) && time.Now().Before(until) {
			st.Backstops++
		}
		// The first pop after a wake may be the front of a burst. If there is
		// more behind it, wake the next sleeper rather than leave that to
		// whichever producer publishes next.
		if r.TryPop(out, st) == PopOK {
			if r.headPublished() && r.notEmpty.Notify() {
				st.Wakes++
			}
			return PopOK
		}
	}
}

func (r *Ring[T, Pad]) consume(s *slot[T, Pad], pos uint64, out *T, st *OpStats) {
	*out = s.val
	if !r.opt.Retain {
		// A consumed slot that still holds a pointer keeps its referent alive
		// until the next lap overwrites it — 13 ms at 5 M/s, never at rest.
		var zero T
		s.val = zero
	}
	if h := r.opt.Hooks; h != nil && h.Claimed != nil {
		h.Claimed(pos)
	}
	r.release(s, pos, st)
}

func (r *Ring[T, Pad]) release(s *slot[T, Pad], pos uint64, st *OpStats) {
	s.seq.Store(pos + uint64(len(r.slots))) // hand the slot to the producer one lap on
	if pos%r.fullWake == 0 && r.notFull.Notify() {
		st.Wakes++
	}
}

func (r *Ring[T, Pad]) headPublished() bool {
	h := r.head.Load()
	return r.slots[h&r.mask].seq.Load()&^dead == h+1
}

// ---------------------------------------------------------------- shutdown

// Close: after it returns no reservation of either kind can succeed. Positions
// reserved before it are still owed; consumers drain until they have claimed
// every one below the position Close recorded.
func (r *Ring[T, Pad]) Close() {
	if old := r.tail.Or(closedBit); old&closedBit == 0 {
		r.closedAt.Store(old>>1 + 1)
	}
	r.notEmpty.Close()
	r.notFull.Close()
}

// ProducersDone is a no-op: the closed bit already tells consumers when to stop.
func (r *Ring[T, Pad]) ProducersDone() {}

func (r *Ring[T, Pad]) Closed() bool { return r.tail.Load()&closedBit != 0 }

// Occupancy is positions reserved but not yet claimed. The two counters are
// read one after the other, so it reads as empty rather than as 2^64 when the
// claimed count has moved past the reserved one.
func (r *Ring[T, Pad]) Occupancy() uint64 {
	reserved, claimed := r.tail.Load()>>1, r.head.Load()
	if reserved > claimed {
		return reserved - claimed
	}
	return 0
}

func (r *Ring[T, Pad]) Sleepers() int { return r.notEmpty.Sleepers() }
