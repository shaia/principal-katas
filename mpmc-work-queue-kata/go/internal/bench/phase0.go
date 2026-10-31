package bench

// Phase 0 — the instrument, before any result depends on it.
//
// Go's clock is the problem this phase exists for: on Windows time.Now advances
// at the clock interrupt, not continuously. So the benchmark reads the
// timestamp counter instead, and this phase prints both, checks the counter is
// invariant and calibrated, measures what a sleep and a timer actually wait, and
// checks the counter agrees across cores — end-to-end latency is a stamp taken
// on one core and read on another.

import (
	"errors"
	"fmt"
	"os"
	"runtime"
	"slices"
	"sync"
	"sync/atomic"
	"time"

	"turnstile/internal/platform"
	"turnstile/internal/tsc"
)

func Phase0() {
	fmt.Printf("  %s %s/%s; GOMAXPROCS %d of %d logical CPUs; GODEBUG=%q; race detector %v\n",
		runtime.Version(), runtime.GOOS, runtime.GOARCH, runtime.GOMAXPROCS(0), runtime.NumCPU(),
		os.Getenv("GODEBUG"), RaceEnabled)

	step := tsc.NowTick(100)
	calls, distinct := Clk.NowDistinct(5 * time.Millisecond)
	fmt.Printf("  time.Now     smallest observable step %s; %d calls across 5 ms returned %d distinct readings\n",
		Ns(float64(step.Nanoseconds())), calls, distinct)
	fmt.Printf("  counter      %.3f GHz, invariant %v, %.2f ns per read — %.0fx finer than time.Now here\n",
		Clk.GHz(), tsc.Invariant(), Clk.ReadCost(), float64(step.Nanoseconds())*Clk.GHz())
	Check(tsc.Native && tsc.Invariant(), "the counter is invariant, so one calibration holds for the whole run")
	agree := Clk.Agreement(200 * time.Millisecond)
	fmt.Printf("  calibration  a second 200 ms window agrees to %+.4f %%\n", 100*agree)
	Check(agree > -1e-3 && agree < 1e-3, "the calibration agrees with an independent window to 0.1 %%")

	// What a sleep and a timer wait for. The paced producers sleep between bursts
	// and a park's backstop is a timer, so both floors show up in later tables.
	fmt.Printf("  asked for    %10s %10s %10s   (20 samples each)\n", "best", "median", "worst")
	for _, c := range []struct {
		what string
		d    time.Duration
		wait func(time.Duration)
	}{
		{"time.Sleep", time.Microsecond, time.Sleep},
		{"time.Sleep", 100 * time.Microsecond, time.Sleep},
		{"time.Sleep", time.Millisecond, time.Sleep},
		{"timer, select", 50 * time.Microsecond, selectTimer},
		{"timer, select", 2 * time.Millisecond, selectTimer},
	} {
		v := make([]float64, 20)
		for i := range v {
			t0 := tsc.Ticks()
			c.wait(c.d)
			v[i] = Clk.Ns(tsc.Ticks() - t0)
		}
		slices.Sort(v)
		fmt.Printf("  %-13s %-8v %8s %10s %10s\n", c.what, c.d, Ns(v[0]), Ns(v[len(v)/2]), Ns(v[len(v)-1]))
	}

	Thermometer()

	// Skew: a ping-pong between two pinned threads. The round trip with the
	// smallest latency bounds the offset best.
	for _, pair := range [][2]int{{0, 2}, {0, runtime.NumCPU() - 1}} {
		skew, rtt, err := counterSkew(pair[0], pair[1])
		if err != nil {
			fmt.Printf("  skew         CPU %d to %d: not measured (%v)\n", pair[0], pair[1], err)
			continue
		}
		fmt.Printf("  skew         CPU %d to CPU %d: %+.0f ns (best round trip %s)\n", pair[0], pair[1], skew, Ns(rtt))
		Check(skew > -100 && skew < 100, "TSC skew between cores under 100 ns, so cross-core latencies mean something")
	}
}

var timer = time.NewTimer(time.Hour)

func selectTimer(d time.Duration) {
	never := make(chan struct{})
	timer.Reset(d)
	select {
	case <-never:
	case <-timer.C:
	}
}

func counterSkew(a, b int) (skewNs, rttNs float64, err error) {
	const rounds = 20000
	var ping, pong, remote atomic.Uint64
	var bestRTT uint64 = 1 << 63
	var offset float64
	start := make(chan struct{})
	errs := make(chan error, 2)
	var wg sync.WaitGroup
	wg.Add(2)
	// Neither goroutine unlocks its thread: the runtime retires a thread whose
	// goroutine exits locked, so the pinning dies with it.
	go func() {
		defer wg.Done()
		runtime.LockOSThread()
		errs <- platform.PinThread(b)
		<-start
		for i := uint64(1); i <= rounds; i++ {
			for ping.Load() != i {
			}
			remote.Store(tsc.Ticks())
			pong.Store(i)
		}
	}()
	go func() {
		defer wg.Done()
		runtime.LockOSThread()
		errs <- platform.PinThread(a)
		<-start
		for i := uint64(1); i <= rounds; i++ {
			t1 := tsc.Ticks()
			ping.Store(i)
			for pong.Load() != i {
			}
			t3 := tsc.Ticks()
			if rtt := t3 - t1; rtt < bestRTT {
				bestRTT = rtt
				offset = float64(int64(remote.Load())-int64(t1+rtt/2)) * Clk.NsPerTick()
			}
		}
	}()
	e1, e2 := <-errs, <-errs
	if e := errors.Join(e1, e2); e != nil {
		// Let both goroutines run their rounds unpinned rather than leave them blocked.
		close(start)
		wg.Wait()
		return 0, 0, e
	}
	close(start)
	wg.Wait()
	return offset, Clk.Ns(bestRTT), nil
}
