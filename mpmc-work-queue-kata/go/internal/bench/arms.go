package bench

import (
	"context"
	"fmt"
	"runtime"
	"sync/atomic"

	"turnstile/internal/turnstile"
)

type ev = turnstile.Event

// ---------------------------------------------------------------- rings

// ringArm runs a Ring behind the driver's interface. Its element is E — the
// padded LineEvent, or the bare Event, packed — and its blocking push is the
// ring's Push (a ticket) or PushContext (a compare-and-swap).
type ringArm[E any] struct {
	q      *turnstile.Ring[E]
	ticket bool
	in     func(ev) E
	out    func(E) ev
}

func lineIn(e ev) turnstile.LineEvent  { return turnstile.LineEvent{Event: e} }
func lineOut(e turnstile.LineEvent) ev { return e.Event }
func same(e ev) ev                     { return e }

func (a *ringArm[E]) Producer() Producer { return &ringProducer[E]{a.q.Producer(), a} }
func (a *ringArm[E]) Consumer() Consumer { return &ringConsumer[E]{a.q.Consumer(), a} }
func (a *ringArm[E]) Close()             { a.q.Close() }
func (a *ringArm[E]) ProducersDone()     {} // the close position already tells consumers when to stop
func (a *ringArm[E]) Len() int           { return a.q.Len() }

type ringProducer[E any] struct {
	p *turnstile.Producer[E]
	a *ringArm[E]
}

func (h *ringProducer[E]) TryPush(e ev) error     { return h.p.TryPush(h.a.in(e)) }
func (h *ringProducer[E]) Stats() turnstile.Stats { return h.p.Stats() }
func (h *ringProducer[E]) Push(e ev) error {
	if h.a.ticket {
		return h.p.Push(h.a.in(e))
	}
	return h.p.PushContext(context.Background(), h.a.in(e))
}

type ringConsumer[E any] struct {
	c *turnstile.Consumer[E]
	a *ringArm[E]
}

func (h *ringConsumer[E]) Stats() turnstile.Stats { return h.c.Stats() }
func (h *ringConsumer[E]) Pop() (ev, error) {
	v, err := h.c.Pop()
	return h.a.out(v), err
}

// ring is the answer's ring, slots padded to a line, of the given capacity.
func ring(capacity int, ticket bool, v *turnstile.Variant) Queue {
	q := turnstile.New[turnstile.LineEvent](capacity)
	if v != nil {
		q = turnstile.NewVariant[turnstile.LineEvent](capacity, *v)
	}
	return &ringArm[turnstile.LineEvent]{q, ticket, lineIn, lineOut}
}

// packed is the same ring with the bare Event as its element: 40-byte slots,
// so neighbours share lines.
func packed(capacity int, ticket bool) Queue {
	return &ringArm[ev]{turnstile.New[ev](capacity), ticket, same, same}
}

// packedVariant is a packed ring built from a Variant: the small rings the bug
// stress runs on.
func packedVariant(capacity int, ticket bool, v turnstile.Variant) Queue {
	return &ringArm[ev]{turnstile.NewVariant[ev](capacity, v), ticket, same, same}
}

var (
	ringTicket = func() Queue { return ring(Capacity, true, nil) }
	ringCAS    = func() Queue { return ring(Capacity, false, nil) }
	ringPacked = func() Queue { return packed(Capacity, true) }
)

func ringWith(ticket bool, v turnstile.Variant) func() Queue {
	return func() Queue { return ring(Capacity, ticket, &v) }
}

// ---------------------------------------------------------------- channels

// chanArm is a buffered channel behind the driver's interface.
//
// The channel is already a bounded MPMC queue: runtime.hchan is a ring under
// one runtime mutex, with a queue of parked senders and one of parked
// receivers. Its one hazard for this brief is shutdown. A send on a closed
// channel panics, so with 32 senders the data channel cannot be the shutdown
// signal: Close closes a separate one, and ProducersDone closes the data
// channel only once whoever counted the producers out knows the last send has
// returned.
type chanArm struct {
	ch      chan ev
	done    chan struct{}
	closing atomic.Bool
	send    send
}

// send is what a blocking push does on the way into the channel.
type send uint8

const (
	// sendFast tries a non-blocking send, and only when the channel is full
	// selects over the send and the shutdown signal. A non-blocking send to a
	// full channel returns without taking the lock (runtime/chan.go, chansend).
	sendFast send = iota
	// sendSelect selects over the send and the shutdown signal on every push:
	// the textbook spelling.
	sendSelect
	// sendPlain is a bare send. It never refuses, so a producer using it stops
	// only when its own quota does.
	sendPlain
)

func chanWith(s send) func() Queue {
	return func() Queue { return &chanArm{ch: make(chan ev, Capacity), done: make(chan struct{}), send: s} }
}

func (a *chanArm) Producer() Producer { return &chanProducer{a: a} }
func (a *chanArm) Consumer() Consumer { return chanConsumer{a} }
func (a *chanArm) Len() int           { return len(a.ch) }

// Close is the shutdown signal. It does not touch the data channel.
func (a *chanArm) Close() {
	if a.closing.CompareAndSwap(false, true) {
		close(a.done)
	}
}

// ProducersDone closes the data channel, after which receivers drain what it
// holds and then see it closed.
func (a *chanArm) ProducersDone() { close(a.ch) }

type chanProducer struct {
	a     *chanArm
	stats turnstile.Stats
}

func (h *chanProducer) Stats() turnstile.Stats { return h.stats }

// TryPush is select with a default. The closed check is a load of a line that
// only Close writes.
func (h *chanProducer) TryPush(e ev) error {
	if h.a.closing.Load() {
		return turnstile.ErrClosed
	}
	select {
	case h.a.ch <- e:
		return nil
	default:
		return turnstile.ErrFull
	}
}

func (h *chanProducer) Push(e ev) error {
	a := h.a
	switch a.send {
	case sendPlain:
		a.ch <- e
		return nil
	case sendSelect:
		select {
		case a.ch <- e:
			return nil
		case <-a.done:
			return turnstile.ErrClosed
		}
	}
	if a.closing.Load() {
		return turnstile.ErrClosed
	}
	select {
	case a.ch <- e:
		return nil
	default:
	}
	h.stats.Parks++
	select {
	case a.ch <- e:
		return nil
	case <-a.done:
		return turnstile.ErrClosed
	}
}

type chanConsumer struct{ a *chanArm }

func (chanConsumer) Stats() turnstile.Stats { return turnstile.Stats{} }
func (h chanConsumer) Pop() (ev, error) {
	e, ok := <-h.a.ch
	if !ok {
		return e, turnstile.ErrClosed
	}
	return e, nil
}

// ---------------------------------------------------------------- comparisons

type arm struct {
	name string
	make func() Queue
	spec func(Spec) Spec // adjusts the shared spec for this arm, if set
}

// interleave runs every arm once per repetition, repetition-major, so arms
// compared in one table share whatever state the machine is in.
func interleave(arms []arm, reps int, spec Spec, label string) [][]Result {
	out := make([][]Result, len(arms))
	for range reps {
		for i, a := range arms {
			s := spec
			if a.spec != nil {
				s = a.spec(s)
			}
			r := Run(a.make(), s)
			CheckRun(&r, label+", "+a.name)
			out[i] = append(out[i], r)
		}
	}
	return out
}

// latencyRow prints the columns every paced table shares, as medians across
// repetitions.
func latencyRow(name string, v []Result) {
	med := func(f func(Result) float64) float64 { return Median(v, f) }
	fmt.Printf("    %-28s %9s %9s %9s %9s %10s %9s %8.3f %5.0f\n", name,
		Rate(med(func(r Result) float64 { return r.Rate })),
		Ns(med(func(r Result) float64 { return Q(&r.Push, 0.5) })),
		Ns(med(func(r Result) float64 { return Q(&r.Push, 0.99) })),
		Ns(med(func(r Result) float64 { return Q(&r.Push, 0.999) })),
		Ns(med(func(r Result) float64 { return Q(&r.Ready, 0.99) })),
		Ns(med(func(r Result) float64 { return Q(&r.E2E, 0.99) })),
		med(func(r Result) float64 { return float64(r.Mallocs) / float64(max(r.Accepted, 1)) }),
		med(func(r Result) float64 { return float64(r.GCs) }))
}

func latencyHeader() {
	fmt.Printf("    %-28s %9s %9s %9s %9s %10s %9s %8s %5s\n", "", "achieved", "push p50", "p99", "p99.9",
		"behind p99", "e2e p99", "allocs/ev", "GCs")
}

// withProcs runs f with GOMAXPROCS set to n, then restores it.
func withProcs(n int, f func()) {
	old := runtime.GOMAXPROCS(n)
	defer runtime.GOMAXPROCS(old)
	f()
}
