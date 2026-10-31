package bench

// Phase 2 — port the slot protocol (step G2): the invariants, on every arm.
//
//  1. Full size, saturated, 32 × 16: both reservations, both full policies and
//     both at once, consumers that never sleep, and the channel three ways.
//  2. Four slots, 8 × 8: a lap wraps every four events with slots still
//     reserved, which is where a protocol that only looks right fails.
//  3. No consumer: exactly 65,536 accepted, none overwritten.
//  4. The collector: what a consumed slot keeps alive.

import (
	"fmt"
	"runtime"
	"weak"

	"turnstile/internal/turnstile"
)

func Phase2() {
	Thermometer()
	full := Spec{Producers: Producers, Consumers: Consumers, Events: 100_000 / scale}
	fmt.Printf("  %d x %d on %d slots, %d events per producer, saturated:\n", full.Producers, full.Consumers,
		Capacity, full.Events)
	fmt.Printf("    %-34s %10s %10s %10s %6s %9s\n", "", "accepted", "rejected", "consumed", "torn", "disorder")
	row := func(name string, q Queue, s Spec) {
		r := Run(q, s)
		CheckRun(&r, name)
		fmt.Printf("    %-34s %10d %10d %10d %6d %9d\n", name, r.Accepted, r.Rejected, r.Consumed, r.Torn, r.Disorder)
	}
	block, reject, mixed := full, full, full
	reject.Mode, mixed.Mode = Reject, Mixed
	row("ring, CAS, block", ringCAS(), block)
	row("ring, ticket, block", ringTicket(), block)
	row("ring, reject", ringCAS(), reject)
	row("ring, half reject, half ticket", ringTicket(), mixed)
	row("ring, consumers never sleep", ringWith(turnstile.Options{Spin: true})(), block)
	row("channel, bare send", chanWith(turnstile.SendPlain)(), block)
	row("channel, select over send and done", chanWith(turnstile.SendSelect)(), block)
	row("channel, fast path, then select", chanWith(turnstile.SendFast)(), block)
	row("channel, reject", chanWith(turnstile.SendFast)(), reject)

	tiny := Spec{Producers: 8, Consumers: 8, Events: 20_000 / scale}
	fmt.Printf("  %d x %d on 4 slots, %d events per producer:\n", tiny.Producers, tiny.Consumers, tiny.Events)
	four := func(o turnstile.Options) Queue { return turnstile.NewRing[ev, struct{}](4, o) }
	tb, tr, tm := tiny, tiny, tiny
	tr.Mode, tm.Mode = Reject, Mixed
	row("4 slots, CAS, block", four(turnstile.Options{}), tb)
	row("4 slots, ticket, block", four(turnstile.Options{Reserve: turnstile.ReserveTicket}), tb)
	row("4 slots, reject", four(turnstile.Options{}), tr)
	row("4 slots, half reject, half ticket", four(turnstile.Options{Reserve: turnstile.ReserveTicket}), tm)

	// No consumer: 32 producers offer twice the capacity between them.
	for _, c := range []struct {
		name string
		q    Queue
	}{{"ring", ringCAS()}, {"channel", chanWith(turnstile.SendFast)()}} {
		r := Run(c.q, Spec{Producers: Producers, Consumers: 0, Events: 2 * Capacity / Producers, Mode: Reject})
		Check(r.Offered == r.Accepted+r.Rejected, "%s, no consumers: offered == accepted + rejected", c.name)
		Check(r.Accepted == Capacity, "%s: with no consumer, exactly 65,536 events are accepted — none overwritten (%d)",
			c.name, r.Accepted)
		fmt.Printf("  no consumer, %s: %d offered, %d accepted, %d refused as full\n", c.name, r.Offered, r.Accepted,
			r.Rejected)
	}

	// The collector.
	fmt.Printf("  the collector: 1,000 events, each pointing at 1 KiB, pushed and consumed, then a GC —\n")
	fmt.Printf("  how many referents the queue still keeps alive:\n")
	for _, c := range []struct {
		name string
		q    func() (push func(*[1024]byte), pop func() *[1024]byte, keep any)
	}{
		{"ring, consumed slot zeroed", func() (func(*[1024]byte), func() *[1024]byte, any) {
			return ringOfPointers(turnstile.Options{})
		}},
		{"ring, consumed slot left as it was", func() (func(*[1024]byte), func() *[1024]byte, any) {
			return ringOfPointers(turnstile.Options{Retain: true})
		}},
		{"channel", func() (func(*[1024]byte), func() *[1024]byte, any) {
			ch := make(chan *[1024]byte, 1024)
			return func(p *[1024]byte) { ch <- p }, func() *[1024]byte { return <-ch }, ch
		}},
	} {
		push, pop, keep := c.q()
		n := retained(push, pop)
		runtime.KeepAlive(keep)
		fmt.Printf("    %-36s %5d of 1000\n", c.name, n)
		if c.name != "ring, consumed slot left as it was" {
			Check(n == 0, "%s: a consumed event's referent is collectable (%d of 1000 still alive)", c.name, n)
		}
	}
}

func ringOfPointers(o turnstile.Options) (func(*[1024]byte), func() *[1024]byte, any) {
	r := turnstile.NewRing[*[1024]byte, struct{}](1024, o)
	var st turnstile.OpStats
	return func(p *[1024]byte) { r.TryPush(p, &st) },
		func() *[1024]byte { var p *[1024]byte; r.TryPop(&p, &st); return p }, r
}

func retained(push func(*[1024]byte), pop func() *[1024]byte) int {
	ws := make([]weak.Pointer[[1024]byte], 1000)
	for i := range ws {
		p := new([1024]byte)
		ws[i] = weak.Make(p)
		push(p)
	}
	for range ws {
		if pop() == nil {
			Check(false, "the collector probe popped nothing")
		}
	}
	runtime.GC()
	n := 0
	for _, w := range ws {
		if w.Value() != nil {
			n++
		}
	}
	return n
}
