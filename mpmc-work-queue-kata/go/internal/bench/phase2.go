package bench

// Phase 2 — port the slot protocol (step G2): the invariants, on every arm.
//
//  1. Full size, saturated, 32 × 16: both blocking pushes, the reject policy
//     and a mix, consumers that never sleep, packed slots, and the channel
//     three ways.
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
	row("ring, Push (ticket)", ringTicket(), block)
	row("ring, PushContext (CAS)", ringCAS(), block)
	row("ring, TryPush", ringCAS(), reject)
	row("ring, half TryPush, half Push", ringTicket(), mixed)
	row("ring, consumers never sleep", ringWith(true, turnstile.Variant{Spin: true})(), block)
	row("ring, slots packed", ringPacked(), block)
	row("channel, bare send", chanWith(sendPlain)(), block)
	row("channel, select over send and done", chanWith(sendSelect)(), block)
	row("channel, fast path, then select", chanWith(sendFast)(), block)
	row("channel, reject", chanWith(sendFast)(), reject)

	tiny := Spec{Producers: 8, Consumers: 8, Events: 20_000 / scale}
	fmt.Printf("  %d x %d on 4 slots, %d events per producer:\n", tiny.Producers, tiny.Consumers, tiny.Events)
	tb, tr, tm := tiny, tiny, tiny
	tr.Mode, tm.Mode = Reject, Mixed
	row("4 slots, Push (ticket)", packed(4, true), tb)
	row("4 slots, PushContext (CAS)", packed(4, false), tb)
	row("4 slots, TryPush", packed(4, false), tr)
	row("4 slots, half TryPush, half Push", packed(4, true), tm)

	// No consumer: 32 producers offer twice the capacity between them.
	for _, c := range []struct {
		name string
		q    Queue
	}{{"ring", ringCAS()}, {"channel", chanWith(sendFast)()}} {
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
			return ringOfPointers(turnstile.Variant{})
		}},
		{"ring, consumed slot left as it was", func() (func(*[1024]byte), func() *[1024]byte, any) {
			return ringOfPointers(turnstile.Variant{Retain: true})
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

func ringOfPointers(v turnstile.Variant) (func(*[1024]byte), func() *[1024]byte, any) {
	q := turnstile.NewVariant[*[1024]byte](1024, v)
	p, c := q.Producer(), q.Consumer()
	return func(r *[1024]byte) { p.TryPush(r) }, func() *[1024]byte { r, _ := c.TryPop(); return r }, q
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
