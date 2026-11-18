package bench

// Phase 5 — close is a panic on the send side (step G5).
//
//  1. Close under load: 32 producers at 1 M/s, closed 200 ms in, for every
//     arm. Every producer must be refused, every accepted event consumed —
//     and the textbook select can accept an event after shutdown began.
//
// The panic the protocol exists to avoid — a send on a closed channel — is a
// test (TestSendOnClosedChannelPanics), and what a producer pays per push to
// learn that shutdown has begun is a benchmark (BenchmarkShutdownCheck).

import (
	"fmt"
	"strings"
	"time"
)

func Phase5() {
	Thermometer()

	// 1. Close under load.
	spec := Spec{Producers: Producers, Consumers: Consumers, Rate: 1e6, Seconds: 1, WorkNs: WorkNs,
		CloseAfter: 200 * time.Millisecond}
	reject := func(s Spec) Spec { s.Mode = Reject; return s }
	arms := []arm{
		{name: "ring, Push (ticket)", make: ringTicket},
		{name: "ring, PushContext (CAS)", make: ringCAS},
		{name: "ring, TryPush", make: ringCAS, spec: reject},
		{name: "channel, fast path, then select", make: chanWith(sendFast)},
		{name: "channel, select over send and done", make: chanWith(sendSelect)},
		{name: "channel, reject", make: chanWith(sendFast), spec: reject},
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
}
