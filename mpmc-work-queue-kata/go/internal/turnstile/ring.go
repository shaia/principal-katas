package turnstile

import (
	"context"
	"runtime"
	"sync/atomic"
	"time"

	"turnstile/internal/tsc"
)

// Ring is the main kata's per-slot sequenced queue. For the slot that holds
// position p = lap·N + i:
//
//	seq == p        free for the producer that reserves p
//	seq == p + 1    published: the consumer that claims p may read it
//	seq == p + N    released: free for the producer of p + N, one lap on
//
// "Reserved but not yet published" is not a value of seq. It is seq == p while
// the tail has already passed p, visible only by reading both.
//
// Every sync/atomic operation is sequentially consistent, so the two handoffs
// through a slot — publication to the consumer, release to the next lap's
// producer — need no ordering annotations, and the value is a plain field: the
// Go memory model makes a plain write sequenced before an atomic store visible
// to a plain read sequenced after an atomic load that observes it. The cost is
// that every publication is an XCHG on amd64 rather than a plain store.
//
// A reservation is a debt, and every path out of a push pays it. A CAS
// reservation cannot happen after Close, because the closed bit lives in the
// word it swaps. A ticket can, because an Add cannot be refused, so Close
// records where the tail stood, consumers drain exactly to there, and a ticket
// past it is refused without touching its slot: nobody is owed it. The
// tombstone — seq set to p + 1 with the dead bit, and no value — is kept for
// the one debt that is owed and cannot be paid with a value: a TryPushFunc
// whose fill panicked. Consumers claim it, skip it and release it.
//
// Each slot is the sequence followed by the value. The ring does not pad it:
// Go cannot size padding from a type parameter, so a caller who wants a slot to
// itself pads the element, as LineEvent does.
type Ring[T any] struct {
	_              [64]byte
	tail           atomic.Uint64 // (next position << 1) | closed; written by producers
	_              [64]byte
	head           atomic.Uint64 // next position to claim; written by consumers
	_              [64]byte
	closedAt       atomic.Uint64 // 1 + the tail's position when Close set the bit; 0 before
	slots          []slot[T]
	mask, fullWake uint64
	backstop       time.Duration
	v              *Variant // nil for a ring from New
	notEmpty       waiter
	notFull        waiter
}

type slot[T any] struct {
	seq atomic.Uint64
	val T
}

const (
	closedBit = 1
	dead      = 1 << 63

	// A consumer spins this many PAUSE instructions before parking: a producer
	// mid-window publishes within nanoseconds unless it has been descheduled,
	// and a park costs microseconds. Variant.NoSpin measures whether it pays.
	spins = 128
	// A ticket holder whose slot is still occupied yields this many times, then
	// sleeps between checks. It is waiting on a full ring, whose pace the
	// consumers already set, and nobody wakes it for its own slot.
	yields     = 64
	ticketPoll = 200 * time.Microsecond
)

// New returns an empty ring. Capacity must be a power of two, at least 2.
func New[T any](capacity int) *Ring[T] { return newRing[T](capacity, nil) }

// NewVariant returns a ring that differs from New's as v says.
func NewVariant[T any](capacity int, v Variant) *Ring[T] { return newRing[T](capacity, &v) }

func newRing[T any](capacity int, v *Variant) *Ring[T] {
	if capacity < 2 || capacity&(capacity-1) != 0 {
		panic("turnstile: capacity must be a power of two, at least 2")
	}
	q := &Ring[T]{slots: make([]slot[T], capacity), mask: uint64(capacity - 1), backstop: 50 * time.Millisecond, v: v}
	if v != nil && v.Backstop > 0 {
		q.backstop = v.Backstop
	}
	// Consumers wake a producer blocked on full once per this many releases: a
	// blocked producer can use any free slot, and waking one per release made
	// every pop pay for a wake.
	q.fullWake = 64
	if capacity < 128 {
		q.fullWake = uint64(capacity / 2)
	}
	// Writing every sequence also faults in every page of the ring here, on the
	// constructing goroutine, so no producer takes a first-touch fault inside its
	// window. No slot's initial sequence is zero, so the design cannot skip it.
	for i := range q.slots {
		q.slots[i].seq.Store(uint64(i))
	}
	q.notEmpty.init(v != nil && v.SharedToken)
	q.notFull.init(false)
	return q
}

// Cap is the number of slots.
func (q *Ring[T]) Cap() int { return len(q.slots) }

// Len is positions reserved but not yet claimed: a diagnostic, racy by nature.
// The two counters are read one after the other, so it reads as empty rather
// than as 2^64 when the claimed count has passed the reserved one.
func (q *Ring[T]) Len() int {
	reserved, claimed := q.tail.Load()>>1, q.head.Load()
	if reserved > claimed {
		return int(reserved - claimed)
	}
	return 0
}

// Sleepers is how many consumers are asleep: a diagnostic, racy by nature.
func (q *Ring[T]) Sleepers() int { return q.notEmpty.sleepers() }

// Close stops the ring accepting. After it returns no push of either kind can
// succeed. Positions reserved before it are still owed; consumers drain until
// they have claimed every one below the position Close recorded, and then their
// pops return ErrClosed. Close may be called more than once.
func (q *Ring[T]) Close() {
	if old := q.tail.Or(closedBit); old&closedBit == 0 {
		q.closedAt.Store(old>>1 + 1)
	}
	q.notEmpty.close()
	q.notFull.close()
}

// ---------------------------------------------------------------- producers

// Producer is one goroutine's way of pushing. It is not safe for concurrent
// use: give each producing goroutine its own. A ring serves at most 64.
type Producer[T any] struct {
	q     *Ring[T]
	stats Stats
	sl    sleeper
}

// Producer returns a new handle for one producing goroutine.
func (q *Ring[T]) Producer() *Producer[T] {
	return &Producer[T]{q: q, sl: q.notFull.sleeper("producers")}
}

// Stats is what this handle's pushes have cost so far.
func (p *Producer[T]) Stats() Stats { return p.stats }

// TryPush is the reject policy: it never waits, and returns ErrFull when the
// slot at the tail still holds the previous lap, or ErrClosed.
func (p *Producer[T]) TryPush(v T) error {
	s, pos, err := p.q.claim(&p.stats)
	if err != nil {
		return err
	}
	p.q.publish(s, pos, v, &p.stats)
	return nil
}

// Push is the block policy without a deadline, like a channel send: it waits
// for room, and returns ErrClosed if the ring closes first.
//
// It reserves with a ticket — one Add that never fails and never retries —
// and then waits for its own slot to empty. A ticket cannot be handed back,
// which is why Push takes no context: once it has its position it must wait
// for that slot, however long the consumers take. PushContext honours a
// deadline instead, and pays for it in retries.
func (p *Producer[T]) Push(v T) error {
	// The pre-check spares a closed ring a ticket in the common case. The race
	// it leaves, a close between this load and the Add, is handled in pushTicket.
	if p.q.tail.Load()&closedBit != 0 {
		return ErrClosed
	}
	return p.pushTicket(v)
}

func (p *Producer[T]) pushTicket(v T) error {
	q := p.q
	word := q.tail.Add(2) - 2
	if word&closedBit != 0 {
		return ErrClosed // past the close position: owed to nobody
	}
	pos := word >> 1
	s := &q.slots[pos&q.mask]
	q.awaitTurn(s, pos, &p.stats)
	q.publish(s, pos, v, &p.stats)
	return nil
}

// PushContext is the block policy with a deadline: it waits for room until ctx
// is done, and then returns ctx.Err(). It reserves by compare-and-swap, look
// first and claim second, so it never holds a position it might have to give
// back.
func (p *Producer[T]) PushContext(ctx context.Context, v T) error {
	parked := false
	for {
		err := p.TryPush(v)
		if err == ErrFull {
			p.sl.prepare()
			if err = p.TryPush(v); err != ErrFull {
				p.sl.cancel()
			}
		}
		if err != ErrFull {
			if parked && err == nil {
				p.q.chainFullWake(&p.stats)
			}
			return err
		}
		if err := ctx.Err(); err != nil {
			p.sl.cancel()
			return err
		}
		p.stats.Parks++
		parked = true
		p.sl.wait(p.q.backstop, ctx.Done())
	}
}

// TryPushFunc reserves a slot as TryPush does and lets fill build the value in
// place. Whatever fill does, the reservation is paid: if it panics, a deferred
// function publishes a tombstone, consumers skip it, and the panic carries on
// to the caller. A defer runs on a panic and on a normal return alike, so there
// is no path on which the debt is forgotten.
func (p *Producer[T]) TryPushFunc(fill func(*T)) error {
	q := p.q
	s, pos, err := q.claim(&p.stats)
	if err != nil {
		return err
	}
	published := false
	defer func() {
		if !published {
			var zero T
			s.val = zero
			s.seq.Store((pos + 1) | dead)
			// Values published behind this position became reachable just now,
			// and their consumers may be asleep waiting for this one.
			q.notEmpty.notify()
		}
	}()
	fill(&s.val)
	s.seq.Store(pos + 1)
	published = true
	if q.notEmpty.notify() {
		p.stats.Wakes++
	}
	return nil
}

// claim reserves the tail position by compare-and-swap if its slot is free.
// The closed bit lives in the word it swaps, so a close and a reservation are
// one decision: the swap fails if the ring closed.
func (q *Ring[T]) claim(st *Stats) (*slot[T], uint64, error) {
	word := q.tail.Load()
	for {
		if word&closedBit != 0 {
			return nil, 0, ErrClosed
		}
		pos := word >> 1
		s := &q.slots[pos&q.mask]
		// Signed, and defined: Go's unsigned subtraction wraps, so the
		// difference is right across a wrap of the 64-bit position.
		dif := int64((s.seq.Load() &^ dead) - pos)
		switch {
		case dif == 0 || (dif < 0 && q.v != nil && q.v.Bug == BugNoFullCheck):
			if q.tail.CompareAndSwap(word, word+2) {
				return s, pos, nil
			}
			st.Retries++
		case dif < 0:
			return nil, 0, ErrFull // the slot still holds the previous lap
		default:
			st.Retries++ // another producer took pos; the tail read was stale
		}
		word = q.tail.Load()
	}
}

// publish is the window. Everything between the reservation and the store of
// pos + 1 is time during which every consumer behind this position waits.
func (q *Ring[T]) publish(s *slot[T], pos uint64, v T, st *Stats) {
	if q.v == nil {
		s.val = v
		s.seq.Store(pos + 1)
	} else if q.v.Bug == BugPublishFirst {
		s.seq.Store(pos + 1)
		s.val = v
	} else {
		s.val = v
		if h := q.v.Hooks.Reserved; h != nil {
			h(pos)
		}
		s.seq.Store(pos + 1)
	}
	if q.notEmpty.notify() {
		st.Wakes++
	}
}

// awaitTurn is a ticket holder waiting for the previous lap to leave its slot:
// spin, then yield, then poll on a timer.
func (q *Ring[T]) awaitTurn(s *slot[T], pos uint64, st *Stats) {
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
func (q *Ring[T]) chainFullWake(st *Stats) {
	t := q.tail.Load() >> 1
	if q.slots[t&q.mask].seq.Load()&^dead == t && q.notFull.notify() {
		st.Wakes++
	}
}

// ---------------------------------------------------------------- consumers

// Consumer is one goroutine's way of popping. It is not safe for concurrent
// use: give each consuming goroutine its own. A ring serves at most 64.
type Consumer[T any] struct {
	q     *Ring[T]
	stats Stats
	sl    sleeper
}

// Consumer returns a new handle for one consuming goroutine.
func (q *Ring[T]) Consumer() *Consumer[T] {
	return &Consumer[T]{q: q, sl: q.notEmpty.sleeper("consumers")}
}

// Stats is what this handle's pops have cost so far.
func (c *Consumer[T]) Stats() Stats { return c.stats }

// TryPop takes the value at the head if one is published, and reports whether
// it did. It never reads the tail: "empty" is decided by the head slot alone,
// so false means empty, or a hole — a position reserved and not yet published.
func (c *Consumer[T]) TryPop() (T, bool) { return c.q.tryPop(&c.stats) }

// Pop waits for a value, like a channel receive, and returns ErrClosed once the
// ring has closed and everything accepted before the close has been taken.
func (c *Consumer[T]) Pop() (T, error) { return c.PopContext(context.Background()) }

// PopContext is Pop with a deadline: it returns ctx.Err() when ctx is done
// first. It spins briefly, then parks.
func (c *Consumer[T]) PopContext(ctx context.Context) (T, error) {
	q, st := c.q, &c.stats
	var zero T
	for {
		if v, ok := q.tryPop(st); ok {
			return v, nil
		}
		if q.v == nil || !q.v.NoSpin {
			for i := 0; i < spins && !q.headPublished(); i++ {
				tsc.Pause()
			}
			if v, ok := q.tryPop(st); ok {
				return v, nil
			}
		}

		// Only now, about to sleep, look at the producers' counter. After a
		// close nobody parks: what remains is owed by producers that already
		// hold their positions, so yield and retry until all are claimed.
		if word := q.tail.Load(); word&closedBit != 0 {
			// Drained at the close position, not at the tail, which refused
			// tickets keep moving. Zero means Close has set the bit and not yet
			// recorded where: not drained yet.
			if end := q.closedAt.Load(); end != 0 && q.head.Load() == end-1 {
				return zero, ErrClosed
			}
			if err := ctx.Err(); err != nil {
				return zero, err
			}
			runtime.Gosched()
			continue
		}
		if err := ctx.Err(); err != nil {
			return zero, err
		}
		if q.v != nil && q.v.Spin {
			if q.v.Yield {
				runtime.Gosched()
			}
			continue
		}

		late := q.v != nil && q.v.Bug == BugLateAnnounce
		if q.v != nil && q.v.Hooks.Parking != nil {
			q.v.Hooks.Parking()
		}
		if !late {
			c.sl.prepare()
		}
		if v, ok := q.tryPop(st); ok { // the re-check
			if !late {
				c.sl.cancel()
			}
			return v, nil
		}
		if late {
			// The bug: a publication between the re-check and this line reads no
			// announcement and wakes nobody.
			c.sl.prepare()
		}
		word := q.tail.Load()
		if word&closedBit != 0 {
			c.sl.cancel()
			continue
		}
		if word>>1 > q.head.Load() {
			st.Holes++ // reserved, unpublished: a producer is inside its window
		}
		st.Parks++
		woken := c.sl.wait(q.backstop, ctx.Done())
		if err := ctx.Err(); err != nil {
			return zero, err
		}
		if !woken {
			st.Backstops++
		}
		// The first pop after a wake may be the front of a burst. If there is
		// more behind it, wake the next sleeper rather than leave that to
		// whichever producer publishes next.
		if v, ok := q.tryPop(st); ok {
			if q.headPublished() && q.notEmpty.notify() {
				st.Wakes++
			}
			return v, nil
		}
	}
}

func (q *Ring[T]) tryPop(st *Stats) (T, bool) {
	pos := q.head.Load()
	for {
		s := &q.slots[pos&q.mask]
		raw := s.seq.Load()
		dif := int64((raw &^ dead) - (pos + 1))
		switch {
		case dif == 0:
			if q.head.CompareAndSwap(pos, pos+1) {
				if raw&dead == 0 {
					return q.consume(s, pos, st), true
				}
				q.release(s, pos, st) // a tombstone: nothing to read, give the slot back
				pos++
				continue
			}
			st.Retries++
		case dif < 0:
			var zero T
			return zero, false // not yet published for this lap: empty, or a hole
		default:
			st.Retries++
		}
		pos = q.head.Load()
	}
}

func (q *Ring[T]) consume(s *slot[T], pos uint64, st *Stats) T {
	v := s.val
	if q.v == nil || !q.v.Retain {
		// A consumed slot that still holds a pointer keeps its referent alive
		// until the next lap overwrites it — 13 ms at 5 M/s, never at rest.
		var zero T
		s.val = zero
	}
	q.release(s, pos, st)
	return v
}

func (q *Ring[T]) release(s *slot[T], pos uint64, st *Stats) {
	s.seq.Store(pos + uint64(len(q.slots))) // hand the slot to the producer one lap on
	if pos%q.fullWake == 0 && q.notFull.notify() {
		st.Wakes++
	}
}

func (q *Ring[T]) headPublished() bool {
	h := q.head.Load()
	return q.slots[h&q.mask].seq.Load()&^dead == h+1
}
