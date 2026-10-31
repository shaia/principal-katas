package bench

// Phase 4 — preemption you did not schedule (step G4).
//
//  1. The hole, with every P occupied by a spinning consumer. A producer
//     reserves a slot and is descheduled inside its window — runtime.Gosched
//     in its hook, which puts it on the global run queue, as a preemption
//     does — while every P is held by a consumer waiting for that slot and more
//     consumers are queued behind them: three for every P, so a P that looks
//     for work finds a spinner before it finds the producer. How long until the
//     producer runs again and publishes, for four ways of waiting, with
//     asynchronous preemption on and off.
//  2. GOMAXPROCS against the brief's 48 goroutines: below, at and above the
//     machine's 32 logical CPUs.
//
// A panic inside the window is a test (TestPanicInWindowPaysTombstone), and
// what the defer that pays for it costs is a benchmark (BenchmarkPushFuncPop).

import (
	"context"
	"fmt"
	"runtime"
	"slices"
	"sync"
	"sync/atomic"
	"time"

	"turnstile/internal/tsc"
	"turnstile/internal/turnstile"
)

const holeProcs, holeSpinners, holeTrials = 4, 12, 5

var holeArms = []struct {
	name  string
	tight bool // a spin on a flag, with no calls in the loop, instead of a ring
	v     turnstile.Variant
}{
	{"a tight spin, no calls in the loop", true, turnstile.Variant{}},
	{"Pop, spin only", false, turnstile.Variant{Spin: true}},
	{"Pop, spin with Gosched", false, turnstile.Variant{Spin: true, Yield: true}},
	{"Pop, spin, then park (the ring's)", false, turnstile.Variant{}},
}

// holeResult is, per arm, every trial's hole in nanoseconds.
type holeResult struct {
	Holes    [][]float64
	Failures int
}

func init() {
	children["hole"] = func() any {
		runtime.GOMAXPROCS(holeProcs)
		out := holeResult{Holes: make([][]float64, len(holeArms))}
		for t := 0; t < holeTrials; t++ {
			for i, a := range holeArms {
				out.Holes[i] = append(out.Holes[i], hole(a.tight, a.v))
			}
		}
		out.Failures = Failures()
		return out
	}
}

// hole runs one trial and returns how long the producer stayed inside its
// window, in nanoseconds. Spinners give up after a second.
//
// The producer starts the spinners itself. A go statement puts the new
// goroutine on the creating P's own run queue, so the ones that have not yet
// found a P are queued where the producer is running; when it yields, its P
// takes one of them, and the producer waits on the global queue behind every P.
func hole(tight bool, v turnstile.Variant) float64 {
	giveUp := time.Second
	var started atomic.Int32
	var reservedAt, publishedAt atomic.Uint64
	var wg sync.WaitGroup
	spawn := func(spin func()) {
		for c := 0; c < holeSpinners; c++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				started.Add(1)
				spin()
			}()
		}
		for started.Load() < holeProcs-1 {
		}
	}

	wg.Add(1)
	if tight {
		var flag atomic.Bool
		end := tsc.Ticks() + Clk.Ticks(float64(giveUp.Nanoseconds()))
		go func() {
			defer wg.Done()
			// Ticks is assembly without a stack check, so this loop has no
			// point at which a spinner could stop of its own accord.
			spawn(func() {
				for !flag.Load() && tsc.Ticks() < end {
				}
			})
			reservedAt.Store(tsc.Ticks())
			runtime.Gosched()
			flag.Store(true)
			publishedAt.Store(tsc.Ticks())
		}()
		wg.Wait()
		return Clk.Ns(publishedAt.Load() - reservedAt.Load())
	}

	v.Hooks.Reserved = func(uint64) {
		reservedAt.Store(tsc.Ticks())
		runtime.Gosched()
	}
	q := turnstile.NewVariant[ev](64, v)
	ctx, cancel := context.WithTimeout(context.Background(), giveUp)
	defer cancel()
	go func() {
		defer wg.Done()
		spawn(func() {
			if _, err := q.Consumer().PopContext(ctx); err == nil {
				q.Close() // the other consumers drain and leave
			}
		})
		q.Producer().TryPush(turnstile.MakeEvent(0, 0, 0))
		publishedAt.Store(tsc.Ticks())
	}()
	wg.Wait()
	return Clk.Ns(publishedAt.Load() - reservedAt.Load())
}

func Phase4() {
	Thermometer()
	if RaceEnabled {
		fmt.Println("  skipped: under the race detector timing and scheduling change, so no latency means anything")
		return
	}

	// 1. The hole.
	got := map[bool]holeResult{}
	for _, on := range []bool{true, false} {
		var h holeResult
		if err := inChild("hole", asyncEnv(on), &h); err != nil {
			Check(false, "%v", err)
			return
		}
		Check(h.Failures == 0, "the hole experiment's invariants held in its child")
		got[on] = h
	}
	fmt.Printf("  a producer descheduled inside its window while %d consumers wait for it on %d Ps;\n"+
		"  how long until it publishes, median and worst of %d (spinners give up after 1 s):\n",
		holeSpinners, holeProcs, holeTrials)
	fmt.Printf("    %-38s %12s %12s %14s %12s\n", "", "async on", "worst", "async off", "worst")
	for i, a := range holeArms {
		on, off := got[true].Holes[i], got[false].Holes[i]
		fmt.Printf("    %-38s %12s %12s %14s %12s\n", a.name, Ns(medianOf(on)), Ns(slices.Max(on)),
			Ns(medianOf(off)), Ns(slices.Max(off)))
	}

	// 2. GOMAXPROCS.
	fmt.Printf("\n  32 x 16 at 5 M/s, the ticket ring, GOMAXPROCS against 48 goroutines on %d logical CPUs; median of 3:\n",
		runtime.NumCPU())
	latencyHeader()
	procs := []int{16, 32, 48}
	res := make([][]Result, len(procs))
	for rep := 0; rep < 3; rep++ {
		for i, n := range procs {
			withProcs(n, func() {
				r := Run(ringTicket(), briefSpec())
				CheckRun(&r, fmt.Sprintf("GOMAXPROCS %d", n))
				res[i] = append(res[i], r)
			})
		}
	}
	for i, n := range procs {
		latencyRow(fmt.Sprintf("GOMAXPROCS %d", n), res[i])
	}
	for i, n := range procs {
		fmt.Printf("    GOMAXPROCS %d: consumers parked with a slot reserved and unpublished %.0f times per run\n", n,
			Median(res[i], func(r Result) float64 { return float64(r.ConsumerOps.Holes) }))
	}
}

func medianOf(v []float64) float64 { return Median(v, func(x float64) float64 { return x }) }
