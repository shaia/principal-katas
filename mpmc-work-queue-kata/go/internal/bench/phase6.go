package bench

// Phase 6 — measure the tail you promised (step G6), and decide.
//
//  1. The brief's rate with every arm side by side, interleaved: the channel
//     and the ring with each reservation. Then the C++ track's other two rows
//     for the channel and the ticket: 16 × 8 at 5 M/s, and 32 × 16 at 1 M/s.
//  2. The decision, written before the run: the ring is kept only if its p99
//     beats the channel's in every repetition, not only at the median.
//  3. The saturated ceiling, with the slot padded to a line and packed.
//  4. Asynchronous preemption on and off, for the ring and the channel at the
//     brief's rate — a setting the runtime reads once, so each side is a child
//     process, the pairs launched back to back.

import (
	"fmt"
	"time"

	"turnstile/internal/turnstile"
)

func briefSpec() Spec {
	return Spec{Producers: Producers, Consumers: Consumers, Rate: BriefRate, Seconds: 1, WorkNs: WorkNs, Latency: true}
}

type summary struct {
	Rate, P50, P99, P999, Behind99, E2E99 float64
	Holes                                 uint64
	Failures                              int
}

func summarize(r *Result) summary {
	return summary{Rate: r.Rate, P50: Q(&r.Push, 0.5), P99: Q(&r.Push, 0.99), P999: Q(&r.Push, 0.999),
		Behind99: Q(&r.Ready, 0.99), E2E99: Q(&r.E2E, 0.99), Holes: r.ConsumerOps.Holes, Failures: Failures()}
}

func init() {
	for name, q := range map[string]func() Queue{"tail-ticket": ringTicket, "tail-chan": chanWith(turnstile.SendFast)} {
		children[name] = func() any {
			r := Run(q(), briefSpec())
			CheckRun(&r, name)
			return summarize(&r)
		}
	}
}

// burstLine says what arrival process a paced run actually offered: how often
// each producer woke, and how many events it pushed back to back.
func burstLine(v []Result, s Spec) {
	perProducer := Median(v, func(r Result) float64 { return float64(r.Bursts) / float64(s.Producers) })
	perBurst := Median(v, func(r Result) float64 { return float64(r.Offered) / float64(max(r.Bursts, 1)) })
	fmt.Printf("    each producer woke every %s and pushed %.0f events back to back\n",
		Ns(s.Seconds*1e9/perProducer), perBurst)
}

func Phase6() {
	Thermometer()
	if RaceEnabled {
		fmt.Println("  skipped: under the race detector timing and scheduling change, so no latency means anything")
		return
	}

	// 1. The brief's rate.
	arms := []arm{
		{name: "channel, fast path", make: chanWith(turnstile.SendFast)},
		{name: "ring, ticket", make: ringTicket},
		{name: "ring, CAS", make: ringCAS},
	}
	res := interleave(arms, 3, briefSpec(), "the tail at 32 x 16")
	fmt.Printf("  32 x 16 at 5 M/s, block on full, consumers spend %s per event; median of 3:\n", Ns(WorkNs))
	latencyHeader()
	for i, a := range arms {
		latencyRow(a.name, res[i])
	}
	burstLine(res[1], briefSpec())

	// The arrival process is part of the experiment. The C++ harness's
	// producers ask for the same 200 µs nap, and on Windows sleep_for returns
	// tens of milliseconds later, so each pushes thousands of events back to
	// back; Go's timer returns in about half a millisecond. Before comparing
	// with the C++ track, the same arms under its arrival process.
	coarse := briefSpec()
	coarse.Nap = 37 * time.Millisecond
	cr := interleave(arms, 3, coarse, "the tail with the C++ harness's bursts")
	fmt.Printf("  the same, each producer napping 37 ms between bursts, as the C++ harness's did:\n")
	for i, a := range arms {
		latencyRow(a.name, cr[i])
	}
	burstLine(cr[1], coarse)
	fmt.Printf("  the same two arms at the C++ track's other rows:\n")
	for _, row := range []struct {
		what string
		p, c int
		rate float64
	}{{"16 x 8 at 5 M/s", 16, 8, 5e6}, {"32 x 16 at 1 M/s", 32, 16, 1e6}} {
		spec := briefSpec()
		spec.Producers, spec.Consumers, spec.Rate = row.p, row.c, row.rate
		pair := []arm{{name: row.what + ", channel", make: chanWith(turnstile.SendFast)},
			{name: row.what + ", ticket", make: ringTicket}}
		r := interleave(pair, 3, spec, "the tail at "+row.what)
		for i, a := range pair {
			latencyRow(a.name, r[i])
		}
	}

	// 2. The decision.
	worstRing, bestChan := 0.0, 1e18
	for _, r := range res[1] {
		worstRing = max(worstRing, Q(&r.Push, 0.99))
	}
	for _, r := range res[0] {
		bestChan = min(bestChan, Q(&r.Push, 0.99))
	}
	ticket99 := Median(res[1], func(r Result) float64 { return Q(&r.Push, 0.99) })
	fmt.Printf("\n  the decision: the ticket ring's worst p99 of three, %s, against the channel's best, %s — %s\n",
		Ns(worstRing), Ns(bestChan), map[bool]string{true: "the ring stays", false: "delete the ring"}[worstRing < bestChan])
	fmt.Printf("  the brief: the ticket ring's median p99 at 32 x 16 is %s, which %s 2 us\n", Ns(ticket99),
		map[bool]string{true: "meets", false: "misses"}[ticket99 <= BudgetNs])

	// 3. The ceiling.
	sat := Spec{Producers: Producers, Consumers: Consumers, Events: 250_000}
	satArms := []arm{
		{name: "channel, bare send", make: chanWith(turnstile.SendPlain)},
		{name: "channel, fast path", make: chanWith(turnstile.SendFast)},
		{name: "ring, ticket, slot per line", make: ringTicket},
		{name: "ring, ticket, slots packed", make: ringPacked},
		{name: "ring, CAS", make: ringCAS},
	}
	sr := interleave(satArms, 3, sat, "saturated")
	fmt.Printf("\n  saturated, 32 x 16, %d events per producer, nothing done per event; median of 3:\n", sat.Events)
	fmt.Printf("    %-28s %10s %14s %12s\n", "", "throughput", "retries/push", "parks/pop")
	for i, a := range satArms {
		fmt.Printf("    %-28s %10s %14.3f %12.4f\n", a.name,
			Rate(Median(sr[i], func(r Result) float64 { return r.Rate })),
			Median(sr[i], func(r Result) float64 { return float64(r.ProducerOps.Retries) / float64(r.Accepted) }),
			Median(sr[i], func(r Result) float64 { return float64(r.ConsumerOps.Parks) / float64(r.Consumed) }))
	}

	// 4. Asynchronous preemption.
	fmt.Printf("\n  asynchronous preemption, 32 x 16 at 5 M/s, each run a child process; median of 3:\n")
	fmt.Printf("    %-28s %9s %9s %9s %9s %10s\n", "", "achieved", "push p50", "p99", "p99.9", "behind p99")
	for _, name := range []string{"tail-ticket", "tail-chan"} {
		got := map[bool][]summary{}
		for rep := 0; rep < 3; rep++ {
			for _, on := range []bool{true, false} {
				var s summary
				if err := inChild(name, asyncEnv(on), &s); err != nil {
					Check(false, "%v", err)
					continue
				}
				Check(s.Failures == 0, "%s in a child process: its invariants held", name)
				got[on] = append(got[on], s)
			}
		}
		for _, on := range []bool{true, false} {
			v := got[on]
			if len(v) == 0 {
				continue
			}
			label := map[string]string{"tail-ticket": "ring, ticket", "tail-chan": "channel"}[name] +
				map[bool]string{true: ", async on", false: ", async off"}[on]
			fmt.Printf("    %-28s %9s %9s %9s %9s %10s\n", label,
				Rate(Median(v, func(s summary) float64 { return s.Rate })),
				Ns(Median(v, func(s summary) float64 { return s.P50 })),
				Ns(Median(v, func(s summary) float64 { return s.P99 })),
				Ns(Median(v, func(s summary) float64 { return s.P999 })),
				Ns(Median(v, func(s summary) float64 { return s.Behind99 })))
		}
	}
}
