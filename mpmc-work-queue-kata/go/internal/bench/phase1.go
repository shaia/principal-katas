package bench

// Phase 1 — the runtime already wrote this queue (step G1).
//
//  1. A buffered channel of 65,536, saturated, at 1, 8 and 32 senders against
//     1, 4 and 16 receivers: what one event costs, and how that moves with
//     contention on the one lock.
//  2. The number that decides whether to go further: the p99 of a send at
//     32 × 16 at the brief's 5 M/s, for the three ways a producer can send.
//  3. Why, measured twice: one receive timed by itself with the buffer kept
//     full, and the runtime's own lock-contention profile of the brief's run.
//
// What single channel operations cost, one goroutine at a time and all at
// once, is a set of ordinary benchmarks: go test -bench Channel ./internal/bench/.

import (
	"bytes"
	"fmt"
	"runtime"
	"runtime/pprof"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"

	"turnstile/internal/tsc"
	"turnstile/internal/turnstile"
)

type contention struct {
	Seconds, Rate, P99               float64
	Recv, Send, Select, Other, Total float64 // estimated seconds of waiting, by where
	Failures                         int
}

// The contention profile needs GODEBUG=runtimecontentionstacks=1 to charge a
// runtime lock's waiting to the code that waited, so it runs as a child.
func init() { profiled("chan-contention", chanWith(sendFast), briefSpec) }

// parseContention sums the mutex profile's delay, in seconds, by where each
// record's stack waited: a blocking select (a sleep, in the ring's waiters), a
// channel receive, a channel send — non-blocking ones included — or elsewhere.
func parseContention(text string) contention {
	var c contention
	hz := 1.0
	for _, rec := range strings.Split(text, "\n\n") {
		lines := strings.Split(strings.TrimSpace(rec), "\n")
		for i, l := range lines {
			if v, ok := strings.CutPrefix(l, "cycles/second="); ok {
				hz, _ = strconv.ParseFloat(v, 64)
				lines = lines[i+1:]
				break
			}
		}
		var cycles float64
		var sel, recv, send bool
		for _, l := range lines {
			if f := strings.Fields(l); len(f) > 2 && f[2] == "@" {
				cycles, _ = strconv.ParseFloat(f[0], 64)
			}
			// A select parks through selparkcommit on the scheduler's stack,
			// where selectgo is no longer a frame.
			sel = sel || strings.Contains(l, "runtime.selectgo") || strings.Contains(l, "runtime.selparkcommit")
			recv = recv || strings.Contains(l, "runtime.chanrecv")
			send = send || strings.Contains(l, "runtime.chansend") || strings.Contains(l, "runtime.selectnbsend")
		}
		switch {
		case sel:
			c.Select += cycles
		case recv:
			c.Recv += cycles
		case send:
			c.Send += cycles
		default:
			c.Other += cycles
		}
	}
	c.Recv, c.Send, c.Select, c.Other = c.Recv/hz, c.Send/hz, c.Select/hz, c.Other/hz
	c.Total = c.Recv + c.Send + c.Select + c.Other
	return c
}

// profiled runs one queue under the runtime's lock-contention profile, which
// needs GODEBUG=runtimecontentionstacks=1 to charge a runtime lock's waiting to
// the code that waited — so it is registered as a child experiment.
func profiled(name string, q func() Queue, spec func() Spec) {
	children[name] = func() any {
		runtime.SetMutexProfileFraction(1)
		r := Run(q(), spec())
		CheckRun(&r, name+" under the contention profiler")
		var b bytes.Buffer
		pprof.Lookup("mutex").WriteTo(&b, 1)
		c := parseContention(b.String())
		c.Seconds, c.Rate, c.P99, c.Failures = r.Seconds, r.Rate, Q(&r.Push, 0.99), Failures()
		return c
	}
}

func contended(name string) (contention, bool) {
	var c contention
	if err := inChild(name, []string{"GODEBUG=runtimecontentionstacks=1"}, &c); err != nil {
		Check(false, "%v", err)
		return c, false
	}
	Check(c.Failures == 0, "%s under the contention profiler: its invariants held", name)
	return c, true
}

// receiveTimes times each receive of 16 consumers while `producers`
// goroutines keep the buffer full, rejecting and yielding when it is.
func receiveTimes(producers int, work uint64) (rate float64, h Histogram) {
	const perConsumer = 50000 / scale
	ch := make(chan ev, Capacity)
	var stop atomic.Bool
	var pw, cw sync.WaitGroup
	e := turnstile.MakeEvent(0, 0, 0)
	pw.Add(producers)
	for p := 0; p < producers; p++ {
		go func() {
			defer pw.Done()
			for !stop.Load() {
				select {
				case ch <- e:
				default:
					runtime.Gosched()
				}
			}
		}()
	}
	for len(ch) < Capacity/2 {
		runtime.Gosched()
	}
	hs := make([]Histogram, Consumers)
	cw.Add(Consumers)
	t0 := tsc.Ticks()
	for c := 0; c < Consumers; c++ {
		go func(c int) {
			defer cw.Done()
			for i := 0; i < perConsumer; i++ {
				a := tsc.Ticks()
				<-ch
				hs[c].Record(tsc.Ticks() - a)
				if work != 0 {
					tsc.Burn(work)
				}
			}
		}(c)
	}
	cw.Wait()
	secs := Clk.Ns(tsc.Ticks()-t0) / 1e9
	stop.Store(true)
	pw.Wait()
	for i := range hs {
		h.Merge(&hs[i])
	}
	return Consumers * perConsumer / secs, h
}

func Phase1() {
	Thermometer()

	// 1. The grid.
	total := uint64(2_000_000 / scale)
	senders, receivers := []int{1, 8, 32}, []int{1, 4, 16}
	grid := map[[2]int][]Result{}
	var ring []Result
	for rep := 0; rep < 3; rep++ {
		for _, s := range senders {
			for _, r := range receivers {
				res := Run(chanWith(sendPlain)(), Spec{Producers: s, Consumers: r, Events: total / uint64(s)})
				CheckRun(&res, fmt.Sprintf("channel, %d x %d saturated", s, r))
				grid[[2]int{s, r}] = append(grid[[2]int{s, r}], res)
			}
		}
		res := Run(ringTicket(), Spec{Producers: 32, Consumers: 16, Events: total / 32})
		CheckRun(&res, "ring, 32 x 16 saturated")
		ring = append(ring, res)
	}
	perEvent := func(v []Result) string {
		return Ns(Median(v, func(r Result) float64 { return 1e9 / r.Rate }))
	}
	fmt.Printf("  saturated, %d events per cell, nothing done per event — time per event, median of 3:\n", total)
	fmt.Printf("    %-22s %10s %10s %10s\n", "senders \\ receivers", "1", "4", "16")
	for _, s := range senders {
		fmt.Printf("    %-22d %10s %10s %10s\n", s, perEvent(grid[[2]int{s, 1}]), perEvent(grid[[2]int{s, 4}]),
			perEvent(grid[[2]int{s, 16}]))
	}
	g := grid[[2]int{32, 16}]
	fmt.Printf("    at 32 x 16: channel %s, the ring (ticket) %s; allocations per event %.4f, GCs %.0f\n",
		Rate(Median(g, func(r Result) float64 { return r.Rate })),
		Rate(Median(ring, func(r Result) float64 { return r.Rate })),
		Median(g, func(r Result) float64 { return float64(r.Mallocs) / float64(r.Accepted) }),
		Median(g, func(r Result) float64 { return float64(r.GCs) }))

	// 2. The brief.
	spec := Spec{Producers: Producers, Consumers: Consumers, Rate: BriefRate, Seconds: 1, WorkNs: WorkNs, Latency: true}
	arms := []arm{
		{name: "bare send", make: chanWith(sendPlain)},
		{name: "select over send and done", make: chanWith(sendSelect)},
		{name: "fast path, then select", make: chanWith(sendFast)},
	}
	res := interleave(arms, 3, spec, "channel at the brief's rate")
	fmt.Printf("\n  32 x 16 at 5 M/s, block on full, consumers spend %s per event; median of 3:\n", Ns(WorkNs))
	latencyHeader()
	for i, a := range arms {
		latencyRow(a.name, res[i])
	}
	p99 := Median(res[0], func(r Result) float64 { return Q(&r.Push, 0.99) })
	verdict := "meets"
	if p99 > BudgetNs {
		verdict = "misses"
	}
	fmt.Printf("    the channel's p99 send, %s, %s the brief's 2 us\n", Ns(p99), verdict)

	// 3. Why.
	fmt.Printf("\n  one receive, timed by itself: 16 consumers, the buffer kept full by producers that\n" +
		"  reject and yield, so every receive finds an event waiting:\n")
	fmt.Printf("    %-10s %-10s %10s %10s %10s %10s\n", "producers", "work", "throughput", "recv p50", "p99", "p99.9")
	for _, p := range []int{1, 8, 32} {
		for _, w := range []float64{0, WorkNs} {
			rate, h := receiveTimes(p, Clk.Ticks(w))
			fmt.Printf("    %-10d %-10s %10s %10s %10s %10s\n", p, Ns(w), Rate(rate), Ns(Q(&h, 0.5)), Ns(Q(&h, 0.99)),
				Ns(Q(&h, 0.999)))
		}
	}
	c, ok := contended("chan-contention")
	if !ok {
		return
	}
	// The runtime does not time each waiter. At each contended unlock it takes
	// the mean wait of the oldest and newest waiter and multiplies it by the
	// number waiting (runtime/lock_spinbit.go, unlock2), so the total is an
	// estimate that can exceed goroutines × seconds. The split is the finding.
	// A send that found the channel full waits in the fast path's select.
	fmt.Printf("\n  the runtime's lock-contention profile of the brief's run (a child, slowed by profiling to %s):\n",
		Rate(c.Rate))
	fmt.Printf("    estimated waiting for runtime locks: %.0f s across a %.2f s run; %.0f %% of it in channel\n"+
		"    receives, %.0f %% in channel sends, %.0f %% elsewhere\n",
		c.Total, c.Seconds, 100*c.Recv/c.Total, 100*(c.Send+c.Select)/c.Total, 100*c.Other/c.Total)
}
