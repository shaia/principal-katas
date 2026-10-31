package bench

// Phase 5 — close is a panic on the send side (step G5).
//
//  1. Close under load: 32 producers at 1 M/s, closed 200 ms in, for every
//     arm. Every producer must be refused, every accepted event consumed —
//     and the textbook select can accept an event after shutdown began.
//  2. The panic the protocol exists to avoid: a send on a closed channel.
//  3. What a producer pays per push to learn that shutdown has begun.

import (
	"context"
	"fmt"
	"strings"
	"sync/atomic"
	"time"

	"turnstile/internal/turnstile"
)

func Phase5() {
	Thermometer()

	// 1. Close under load.
	spec := Spec{Producers: Producers, Consumers: Consumers, Rate: 1e6, Seconds: 1, WorkNs: WorkNs,
		CloseAfter: 200 * time.Millisecond}
	reject := func(s Spec) Spec { s.Mode = Reject; return s }
	arms := []arm{
		{name: "ring, CAS, block", make: ringCAS},
		{name: "ring, ticket, block", make: ringTicket},
		{name: "ring, reject", make: ringCAS, spec: reject},
		{name: "channel, fast path, then select", make: chanWith(turnstile.SendFast)},
		{name: "channel, select over send and done", make: chanWith(turnstile.SendSelect)},
		{name: "channel, reject", make: chanWith(turnstile.SendFast), spec: reject},
	}
	const rounds = 5
	res := interleave(arms, rounds, spec, "close under load")
	fmt.Printf("  32 x 16 at 1 M/s, closed 200 ms in with producers running; %d rounds per arm:\n", rounds)
	fmt.Printf("    %-36s %9s %10s %16s %12s\n", "", "refused", "consumed", "accepted after", "drain after")
	fmt.Printf("    %-36s %9s %10s %16s %12s\n", "", "as closed", "", "Close returned", "close, max")
	for i, a := range arms {
		var refused, consumed, after uint64
		var drain float64
		for _, r := range res[i] {
			Check(r.RefusedClosed == Producers, "%s: every producer was refused once the queue closed (%d of %d)",
				a.name, r.RefusedClosed, Producers)
			refused += r.RefusedClosed
			consumed += r.Consumed
			after += r.AfterClose
			if r.TEnd > r.TClose {
				drain = max(drain, Clk.Ns(r.TEnd-r.TClose))
			}
		}
		fmt.Printf("    %-36s %9d %10d %16d %12s\n", a.name, refused, consumed, after, Ns(drain))
		if !strings.Contains(a.name, "select over") {
			Check(after == 0, "%s: no push that began after Close returned was accepted", a.name)
		}
	}

	// 2. The panic.
	q := turnstile.NewChan[ev](8, turnstile.SendFast)
	q.ProducersDone()
	var msg string
	func() {
		defer func() {
			if err, ok := recover().(error); ok {
				msg = err.Error()
			}
		}()
		var st turnstile.OpStats
		q.TryPush(turnstile.MakeEvent(0, 0, 0), &st)
	}()
	Check(msg == "send on closed channel", "a send on a closed channel panics with the runtime's message (%q)", msg)
	fmt.Printf("\n  a send on a closed data channel: panic(%q), recovered here, and fatal anywhere it is not\n", msg)

	// 3. The per-push check.
	var flag atomic.Bool
	done := make(chan struct{})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	ctxDone := ctx.Done()
	fmt.Printf("\n  learning that shutdown has begun, per push, while it has not:\n")
	fmt.Printf("    %-50s %10s %12s\n", "", "alone", "32 at once")
	sink := 0
	for _, c := range []struct {
		what string
		op   func()
	}{
		{"an atomic.Bool load", func() {
			if flag.Load() {
				sink++
			}
		}},
		{"select { case <-done: default: }", func() {
			select {
			case <-done:
				sink++
			default:
			}
		}},
		{"ctx.Err() != nil", func() {
			if ctx.Err() != nil {
				sink++
			}
		}},
		{"select { case <-ctx.Done(): default: }", func() {
			select {
			case <-ctx.Done():
				sink++
			default:
			}
		}},
		{"the same, with Done() read once outside the loop", func() {
			select {
			case <-ctxDone:
				sink++
			default:
			}
		}},
	} {
		fmt.Printf("    %-50s %10s %12s\n", c.what, Ns(perOp(c.op)), Ns(perOpParallel(32, c.op)))
	}
	Check(sink == 0, "nothing reported shutdown while none had begun")
}
