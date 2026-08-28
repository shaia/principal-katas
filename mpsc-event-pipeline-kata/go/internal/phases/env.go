package phases

import (
	"fmt"
	"runtime"
	"runtime/debug"
	"time"

	"weir/internal/verify"
	"weir/internal/weir"
)

// Environment is phase 0, and it has no C++ counterpart.
//
// It exists because the obvious Go translation of this benchmark produces a
// flattering lie. On Windows, Go's time.Now() monotonic reading comes from
// KUSER_SHARED_DATA.InterruptTime scaled by 100 ns — not QueryPerformanceCounter
// — so it advances only at the system clock interrupt. Measured on the
// development machine: 237,201 calls across 5 ms produced six distinct values.
//
// The C++ answer's steady_clock is QPC at ~100 ns resolution, so its reported
// p50 of 0.5-3.3 us is a real measurement. A Go port using time.Now() would
// report p50 = 0.0 us with a tail quantized to whole milliseconds, and would
// look *faster* than the C++. That is the worst kind of wrong: a flattering
// number with no bug to find.
//
// So this phase prints the instrument before any result that depends on it. It
// is the direct descendant of the C++ writeup's "measurement bugs, caught by
// disbelieving the numbers", and it is the strongest argument for having done
// the port at all.
func Environment(clock *weir.Clock) {
	fmt.Println()
	fmt.Println("== 0. environment: what is the instrument? ==")
	fmt.Println()

	wallRes := weir.MeasuredTimeNowResolution(200)
	tscRes := clock.NanosPerTick()

	fmt.Printf("  clock   time.Now() smallest observable tick : %10.1f us  (measured)\n",
		float64(wallRes.Nanoseconds())/1000)
	fmt.Printf("  clock   TSC resolution                      : %10.3f ns  (%.3f GHz, invariant=%v)\n",
		tscRes, clock.HzApprox()/1e9, clock.Invariant())
	fmt.Printf("  clock   TSC read cost                       : %10.2f ns\n", clock.ReadCostNanos())
	fmt.Printf("  clock   ratio                               : %10.0fx coarser if we used time.Now()\n",
		float64(wallRes.Nanoseconds())/tscRes)

	// Sleep granularity: the reason the pacer spins.
	best := time.Hour
	for i := 0; i < 20; i++ {
		t0 := time.Now()
		time.Sleep(100 * time.Microsecond)
		if d := time.Since(t0); d < best {
			best = d
		}
	}
	fmt.Printf("  timer   time.Sleep(100us) best actual        : %10.1f us  (so the pacer spins)\n",
		float64(best.Nanoseconds())/1000)

	fmt.Printf("  sched   GOMAXPROCS=%d  NumCPU=%d  %s/%s  %s\n",
		runtime.GOMAXPROCS(0), runtime.NumCPU(), runtime.GOOS, runtime.GOARCH, runtime.Version())
	fmt.Printf("  layout  Event=%d B (pointer-free, noscan)  SpscRing=%d B  %d slots x %d events = %d KiB\n",
		weir.EventSize, 128+weir.RingCapacity*weir.EventSize, weir.MaxProducers,
		weir.RingCapacity, weir.MaxProducers*weir.RingCapacity*weir.EventSize/1024)

	if raceEnabled {
		fmt.Println()
		fmt.Println("  !! built with -race: sync/atomic is de-intrinsified into TSan calls,")
		fmt.Println("  !! so this binary is 5-20x slower and is a different program.")
		fmt.Println("  !! REFUSING to report measurements. Run without -race.")
	}

	verify.Check(clock.Native(), "phase 0: a hardware tick counter is available")
	verify.Check(clock.Invariant(),
		"phase 0: TSC is invariant, so calibrating once and extrapolating is sound")
	verify.Check(!raceEnabled, "phase 0: not a -race build (measurements would be meaningless)")

	debug.SetGCPercent(debug.SetGCPercent(100)) // touch it so the import is real
	fmt.Println()
}
