package bench

// Phase 3 — build the sleep without a futex (step G3).
//
//  1. What a push's wake decision costs when nobody is asleep — which at 5 M/s
//     is nearly every push — for each primitive, alone and 32 at once.
//  2. What a wake costs when someone is: the waker's own time, and how long the
//     sleeper takes to run.
//  3. A race engineered to lose a wakeup, against the real protocol and against
//     one with two statements in the wrong order. Go has no fence to forget;
//     it still has an order to get wrong.
//  4. Consumers that really do sleep: low rates, where the queue is empty most
//     of the time.

import (
	"fmt"
	"math/rand/v2"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"turnstile/internal/platform"
	"turnstile/internal/tsc"
	"turnstile/internal/turnstile"
)

func Phase3() {
	Thermometer()

	// 1. Nobody asleep.
	bitmap := turnstile.NewWaiter(false)
	counted := turnstile.NewWaiter(true)
	// A token on every push with no count in front of it. Resting: it sits in a
	// full channel, so each send fails on the lock-free full check. Never
	// resting: a channel that is never full, so every send succeeds and takes the
	// lock; its elements have no size, so the capacity costs no memory.
	resting := make(chan struct{}, 1)
	resting <- struct{}{}
	sink := make(chan struct{}, 1<<26)
	var mu sync.Mutex
	cond := sync.NewCond(&mu)
	fmt.Printf("  a push's wake decision with nobody asleep, per push:\n")
	fmt.Printf("    %-50s %10s %12s\n", "", "alone", "32 at once")
	for _, c := range []struct {
		what string
		op   func()
	}{
		{"a bitmap of sleepers (the ring's)", func() { bitmap.Notify() }},
		{"a sleeper count, in front of one shared token", func() { counted.Notify() }},
		{"a token on every push, resting in the channel", func() {
			select {
			case resting <- struct{}{}:
			default:
			}
		}},
		{"a token on every push, never resting", func() {
			select {
			case sink <- struct{}{}:
			default:
			}
		}},
		{"sync.Cond: Lock, Signal, Unlock", func() { mu.Lock(); cond.Signal(); mu.Unlock() }},
	} {
		fmt.Printf("    %-50s %10s %12s\n", c.what, Ns(perOp(c.op)), Ns(perOpParallel(32, c.op)))
	}

	// 2. Someone asleep.
	fmt.Printf("\n  waking one sleeping goroutine, %d trials each; the waker's own time, and until the sleeper runs:\n",
		wakeTrials)
	fmt.Printf("    %-36s %10s %10s %14s %10s\n", "", "waker p50", "p99", "sleeper p50", "p99")
	for _, c := range []struct {
		what string
		run  func() (waker, sleeper Histogram)
	}{
		{"a token, claimed from the bitmap", wakeToken},
		{"a send to a receiver parked on it", wakeChan},
		{"sync.Cond.Signal", wakeCond},
	} {
		w, s := c.run()
		fmt.Printf("    %-36s %10s %10s %14s %10s\n", c.what, Ns(Q(&w, 0.5)), Ns(Q(&w, 0.99)), Ns(Q(&s, 0.5)),
			Ns(Q(&s, 0.99)))
	}

	// 3. The race.
	const iterations = 20_000
	fmt.Printf("\n  the engineered race, %d iterations per row, 2 ms backstop:\n", iterations/scale)
	fmt.Printf("    %-30s %6s %10s %8s %8s %6s\n", "", "spread", "re-check", "woken", "LOST", "late")
	lost := map[turnstile.Bug]int{}
	for _, spread := range []int{64, 256, 1024, 4096} {
		for _, b := range []turnstile.Bug{turnstile.BugNone, turnstile.BugLateAnnounce} {
			r := wakeRace(b, iterations/scale, spread)
			name := map[turnstile.Bug]string{turnstile.BugNone: "announce, then re-check",
				turnstile.BugLateAnnounce: "re-check, then announce"}[b]
			fmt.Printf("    %-30s %6d %10d %8d %8d %6d\n", name, spread, r.recheck, r.woken, r.lost, r.late)
			lost[b] += r.lost
		}
	}
	fmt.Printf("    spread = the consumer's random pause after its hook, in loop iterations;\n" +
		"    LOST = slept through a push whose notify had returned more than 1 ms before the backstop\n")
	fmt.Printf("    lost wakeups: %d in the real order, %d with the two statements swapped\n",
		lost[turnstile.BugNone], lost[turnstile.BugLateAnnounce])
	Check(lost[turnstile.BugNone] == 0, "announcing before the re-check, no wakeup is ever lost")

	// 4. Mostly asleep.
	if RaceEnabled {
		return
	}
	arms := []arm{
		{name: "ring: a token per sleeper", make: ringCAS},
		{name: "ring: one token, shared", make: ringWith(turnstile.Options{SharedToken: true})},
		{name: "ring: consumers never sleep", make: ringWith(turnstile.Options{Spin: true})},
		{name: "channel: its own parking", make: chanWith(turnstile.SendFast)},
	}
	fmt.Printf("\n  %d x %d, consumers spend %s per event, the queue empty most of the time; median of 3:\n",
		Producers, Consumers, Ns(WorkNs))
	fmt.Printf("    %-8s %-30s %10s %9s %9s %9s %9s %9s %6s\n", "rate", "", "wakes/push", "parks/pop",
		"push p50", "p99", "p99.9", "e2e p99", "CPUs")
	for _, rate := range []float64{20e3, 200e3} {
		spec := Spec{Producers: Producers, Consumers: Consumers, Rate: rate, Seconds: 1, WorkNs: WorkNs, Latency: true}
		res := make([][]Result, len(arms))
		cpus := make([][]float64, len(arms))
		for rep := 0; rep < 3; rep++ {
			for i, a := range arms {
				c0, t0 := platform.ProcessCPU(), tsc.Ticks()
				r := Run(a.make(), spec)
				cpus[i] = append(cpus[i], (platform.ProcessCPU()-c0)/(Clk.Ns(tsc.Ticks()-t0)/1e9))
				CheckRun(&r, "mostly asleep, "+a.name)
				res[i] = append(res[i], r)
			}
		}
		for i, a := range arms {
			v := res[i]
			med := func(f func(Result) float64) float64 { return Median(v, f) }
			fmt.Printf("    %-8s %-30s %10.3f %9.3f %9s %9s %9s %9s %6.1f\n", Rate(rate), a.name,
				med(func(r Result) float64 { return float64(r.ProducerOps.Wakes) / float64(r.Accepted) }),
				med(func(r Result) float64 { return float64(r.ConsumerOps.Parks) / float64(r.Consumed) }),
				Ns(med(func(r Result) float64 { return Q(&r.Push, 0.5) })),
				Ns(med(func(r Result) float64 { return Q(&r.Push, 0.99) })),
				Ns(med(func(r Result) float64 { return Q(&r.Push, 0.999) })),
				Ns(med(func(r Result) float64 { return Q(&r.E2E, 0.99) })),
				Median(cpus[i], func(x float64) float64 { return x }))
		}
	}
	fmt.Printf("    the channel parks its receivers itself and counts nothing: its wakes and parks read 0\n")

	// 5. Where the shared token's tail comes from.
	fmt.Printf("\n  the runtime's lock-contention profile at 200 k/s, each arm a child, slowed by profiling:\n")
	fmt.Printf("    %-28s %9s %18s %14s %14s\n", "", "push p99", "estimated waiting", "in sleeps", "in wakes")
	for _, name := range []string{"wake-own-token", "wake-shared-token"} {
		c, ok := contended(name)
		if !ok {
			return
		}
		label := map[string]string{"wake-own-token": "ring: a token per sleeper",
			"wake-shared-token": "ring: one token, shared"}[name]
		fmt.Printf("    %-28s %9s %16.2f s %13.0f%% %13.0f%%\n", label, Ns(c.P99), c.Total,
			100*c.Select/max(c.Total, 1e-9), 100*c.Send/max(c.Total, 1e-9))
	}
	fmt.Printf("    sleeps = waiting inside a sleeper's select; wakes = inside a producer's token send\n")
}

func lowRate() Spec {
	return Spec{Producers: Producers, Consumers: Consumers, Rate: 200e3, Seconds: 1, WorkNs: WorkNs, Latency: true}
}

func init() {
	profiled("wake-own-token", ringCAS, lowRate)
	profiled("wake-shared-token", ringWith(turnstile.Options{SharedToken: true}), lowRate)
}

const wakeTrials = 2000

// asleep waits until the other goroutine has announced it is about to sleep,
// then a further 50 µs, by which time it is parked rather than on its way.
func asleep(flag *atomic.Bool) {
	for !flag.Load() {
		runtime.Gosched()
	}
	end := tsc.Ticks() + Clk.Ticks(50_000)
	for tsc.Ticks() < end {
		tsc.Pause()
	}
	flag.Store(false)
}

func wakeToken() (waker, sleeper Histogram) {
	w := turnstile.NewWaiter(false)
	var flag atomic.Bool
	var woke, t0 atomic.Uint64
	done := make(chan struct{})
	go func() {
		var st turnstile.OpStats
		for i := 0; i < wakeTrials; i++ {
			w.Prepare(&st)
			flag.Store(true)
			w.Wait(time.Second, &st)
			woke.Store(tsc.Ticks())
		}
		close(done)
	}()
	for i := 0; i < wakeTrials; i++ {
		asleep(&flag)
		woke.Store(0)
		t0.Store(tsc.Ticks())
		w.Notify()
		waker.Record(tsc.Ticks() - t0.Load())
		for woke.Load() == 0 {
			runtime.Gosched()
		}
		sleeper.Record(woke.Load() - t0.Load())
	}
	<-done
	return
}

func wakeChan() (waker, sleeper Histogram) {
	ch := make(chan struct{})
	var flag atomic.Bool
	var woke atomic.Uint64
	done := make(chan struct{})
	go func() {
		for i := 0; i < wakeTrials; i++ {
			flag.Store(true)
			<-ch
			woke.Store(tsc.Ticks())
		}
		close(done)
	}()
	for i := 0; i < wakeTrials; i++ {
		asleep(&flag)
		woke.Store(0)
		t0 := tsc.Ticks()
		ch <- struct{}{}
		waker.Record(tsc.Ticks() - t0)
		for woke.Load() == 0 {
			runtime.Gosched()
		}
		sleeper.Record(woke.Load() - t0)
	}
	<-done
	return
}

func wakeCond() (waker, sleeper Histogram) {
	var mu sync.Mutex
	cond := sync.NewCond(&mu)
	ready := false
	var flag atomic.Bool
	var woke atomic.Uint64
	done := make(chan struct{})
	go func() {
		for i := 0; i < wakeTrials; i++ {
			mu.Lock()
			flag.Store(true)
			for !ready {
				cond.Wait()
			}
			ready = false
			mu.Unlock()
			woke.Store(tsc.Ticks())
		}
		close(done)
	}()
	for i := 0; i < wakeTrials; i++ {
		asleep(&flag)
		woke.Store(0)
		t0 := tsc.Ticks()
		mu.Lock()
		ready = true
		cond.Signal()
		mu.Unlock()
		waker.Record(tsc.Ticks() - t0)
		for woke.Load() == 0 {
			runtime.Gosched()
		}
		sleeper.Record(woke.Load() - t0)
	}
	<-done
	return
}

type race struct{ recheck, woken, lost, late int }

// wakeRace: one consumer, one producer, an eight-slot ring. Each iteration the
// consumer finds the queue empty and heads for sleep; its parking hook
// announces that, then pauses a random few iterations, and the producer —
// watching for the announcement — pauses a random few of its own and pushes.
// Some pushes land exactly between the consumer's re-check and its sleep, and
// that is where the wrong order shows. Each iteration ends one of three ways:
// the re-check saw the event, the consumer slept and was woken, or it slept
// through the push until the 2 ms backstop.
func wakeRace(bug turnstile.Bug, iterations, spread int) race {
	var parking, parkingAtAck, pushedAt atomic.Uint64
	var acked atomic.Int64
	acked.Store(-1)
	rng := rand.New(rand.NewPCG(1, uint64(spread)))
	hooks := &turnstile.Hooks{Parking: func() {
		parking.Add(1)
		for n := rng.IntN(spread); n > 0; n-- {
			tsc.Pause()
		}
	}}
	q := turnstile.NewRing[ev, struct{}](8, turnstile.Options{Backstop: 2 * time.Millisecond, Hooks: hooks, Bug: bug})
	oneMs := Clk.Ticks(1e6)
	var out race
	done := make(chan struct{})
	go func() {
		var st turnstile.OpStats
		var e ev
		forever := time.Now().Add(time.Hour)
		for i := 0; i < iterations; i++ {
			parks, backstops := st.Parks, st.Backstops
			if q.Pop(&e, forever, &st) != turnstile.PopOK {
				break
			}
			now := tsc.Ticks()
			switch {
			case st.Backstops > backstops:
				// Slept through it: lost if the producer's push, notify and all,
				// had returned long before the backstop fired; merely late if the
				// producer was itself descheduled in between.
				if now-pushedAt.Load() > oneMs {
					out.lost++
				} else {
					out.late++
				}
			case st.Parks > parks:
				out.woken++
			default:
				out.recheck++
			}
			// Hand over the announcement count with the acknowledgement: the next
			// announcement may come before the producer looks.
			parkingAtAck.Store(parking.Load())
			acked.Store(int64(i))
		}
		close(done)
	}()
	var st turnstile.OpStats
	prng := rand.New(rand.NewPCG(2, uint64(spread)))
	var seen uint64
	for i := 0; i < iterations; i++ {
		for parking.Load() == seen {
			tsc.Pause()
		}
		for n := prng.IntN(64); n > 0; n-- {
			tsc.Pause()
		}
		q.Push(turnstile.MakeEvent(0, uint32(i), tsc.Ticks()), time.Now().Add(5*time.Second), &st)
		pushedAt.Store(tsc.Ticks())
		for acked.Load() < int64(i) {
			runtime.Gosched()
		}
		seen = parkingAtAck.Load()
	}
	<-done
	return out
}
