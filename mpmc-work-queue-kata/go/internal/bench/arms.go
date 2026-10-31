package bench

import (
	"fmt"
	"runtime"

	"turnstile/internal/turnstile"
)

type ev = turnstile.Event

// The queues the phases compare, each built fresh for every run.
func ringWith(o turnstile.Options) func() Queue {
	return func() Queue { return turnstile.NewRing[ev, turnstile.LinePad](Capacity, o) }
}

var (
	ringTicket = ringWith(turnstile.Options{Reserve: turnstile.ReserveTicket})
	ringCAS    = ringWith(turnstile.Options{})
	ringPacked = func() Queue {
		return turnstile.NewRing[ev, struct{}](Capacity, turnstile.Options{Reserve: turnstile.ReserveTicket})
	}
)

func chanWith(s turnstile.Send) func() Queue {
	return func() Queue { return turnstile.NewChan[ev](Capacity, s) }
}

type arm struct {
	name string
	make func() Queue
	spec func(Spec) Spec // adjusts the shared spec for this arm, if set
}

// interleave runs every arm once per repetition, repetition-major, so arms
// compared in one table share whatever state the machine is in.
func interleave(arms []arm, reps int, spec Spec, label string) [][]Result {
	out := make([][]Result, len(arms))
	for rep := 0; rep < reps; rep++ {
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
